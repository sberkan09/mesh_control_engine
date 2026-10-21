// GLSL -> SPIR-V -> MSL (see shader_cross.h). The only translation unit that sees glslang and
// SPIRV-Cross; their std:: API types stay inside it. SPIRV-Cross reports failures by throwing
// spirv_cross::CompilerError, so this file alone is built with exceptions (CMakeLists.txt) and turns
// them into Results at the boundary.

#include "shader_cross.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>
#include <spirv_msl.hpp>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>  // mce-allow: SPIRV-Cross returns std::string
#include <vector>  // mce-allow: glslang/SPIRV-Cross exchange SPIR-V as std::vector<uint32_t>

#include "mce/log.h"

namespace mce::rhi::metal {

MslSource& MslSource::operator=(MslSource&& o) noexcept {
    if (this != &o) {
        std::free(text_);
        text_ = o.text_;
        size_ = o.size_;
        for (u32 i = 0; i < 3; ++i) local_size[i] = o.local_size[i];
        o.text_ = nullptr;
        o.size_ = 0;
    }
    return *this;
}

MslSource::~MslSource() { std::free(text_); }

MslSource MslSource::copy_of(StringView s) {
    MslSource out;
    out.text_ = static_cast<char*>(std::malloc(s.size() + 1));
    MCE_ASSERT(out.text_);
    std::memcpy(out.text_, s.data(), s.size());
    out.text_[s.size()] = '\0';
    out.size_ = s.size();
    return out;
}

namespace {

using SpirV = std::vector<u32>;  // mce-allow: glslang/SPIRV-Cross API type

void init_glslang() {
    static std::once_flag once;
    std::call_once(once, [] {
        glslang::InitializeProcess();
        std::atexit([] { glslang::FinalizeProcess(); });
    });
}

bool is_ident(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

// GL's gl_InstanceID excludes the base instance; Metal's [[instance_id]] (which SPIRV-Cross maps it
// to) includes it. Rewrite every use as (gl_InstanceID - gl_BaseInstanceARB), enabling the extension
// right after #version when the shader did not.
std::string fix_instance_id(StringView glsl) {  // mce-allow: glslang input text
    constexpr StringView kId = "gl_InstanceID";
    std::string out;  // mce-allow
    out.reserve(glsl.size() + 128);
    usize i = 0;
    while (i < glsl.size()) {
        const usize at = glsl.find(kId, i);
        if (at == StringView::npos) break;
        const bool whole = (at == 0 || !is_ident(glsl[at - 1])) &&
                           (at + kId.size() >= glsl.size() || !is_ident(glsl[at + kId.size()]));
        out.append(glsl.substr(i, at - i));
        out.append(whole ? "(gl_InstanceID - gl_BaseInstanceARB)" : "gl_InstanceID");
        i = at + kId.size();
    }
    out.append(glsl.substr(i));
    if (out.find("GL_ARB_shader_draw_parameters") == std::string::npos) {  // mce-allow
        const usize v = out.find("#version");
        const usize eol = v == std::string::npos ? std::string::npos : out.find('\n', v);  // mce-allow
        if (eol != std::string::npos)                                                      // mce-allow
            out.insert(eol + 1, "#extension GL_ARB_shader_draw_parameters : enable\n");
    }
    return out;
}

bool glsl_to_spirv(ShaderStage stage, StringView glsl, SpirV& out, FixedString<1024>& log) {
    init_glslang();
    const EShLanguage lang = stage == ShaderStage::Vertex     ? EShLangVertex
                             : stage == ShaderStage::Fragment ? EShLangFragment
                                                              : EShLangCompute;
    glslang::TShader shader(lang);
    std::string fixed;  // mce-allow: glslang input text
    if (stage == ShaderStage::Vertex && glsl.find("gl_InstanceID") != StringView::npos) {
        fixed = fix_instance_id(glsl);
        glsl = StringView(fixed.data(), fixed.size());
    }
    const char* text = glsl.data();
    const int len = static_cast<int>(glsl.size());
    shader.setStringsWithLengths(&text, &len, 1);
    // OpenGL client semantics: the source was written for GL 4.5 (gl_VertexID, gl_InstanceID,
    // gl_BaseInstanceARB, separable gl_PerVertex), not for Vulkan GLSL.
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientOpenGL, 450);
    shader.setEnvClient(glslang::EShClientOpenGL, glslang::EShTargetOpenGL_450);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    const auto msgs = static_cast<EShMessages>(EShMsgSpvRules);
    if (!shader.parse(GetDefaultResources(), 450, ECoreProfile, false, false, msgs)) {
        log.append(shader.getInfoLog());
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msgs)) {
        log.append(program.getInfoLog());
        return false;
    }
    glslang::SpvOptions opts;
    opts.disableOptimizer = true;
    glslang::GlslangToSpv(*program.getIntermediate(lang), out, &opts);
    return !out.empty();
}

// GL keeps uniform blocks, storage blocks, samplers and image units in separate binding namespaces;
// SPIR-V for GL has no descriptor sets. Give each namespace its own set so the remap below is unambiguous,
// then pin every resource to the fixed Metal index (shader_cross.h).
void remap_resources(spirv_cross::CompilerMSL& msl, spv::ExecutionModel model) {
    const spirv_cross::ShaderResources res = msl.get_shader_resources();
    auto bind = [&](const spirv_cross::Resource& r, u32 set, u32 buffer, u32 texture, u32 sampler) {
        const u32 b = msl.get_decoration(r.id, spv::DecorationBinding);
        msl.set_decoration(r.id, spv::DecorationDescriptorSet, set);
        spirv_cross::MSLResourceBinding mb;
        mb.stage = model;
        mb.desc_set = set;
        mb.binding = b;
        mb.count = 1;
        mb.msl_buffer = buffer + b;
        mb.msl_texture = texture + b;
        mb.msl_sampler = sampler + b;
        msl.add_msl_resource_binding(mb);
    };
    for (const auto& r : res.uniform_buffers) bind(r, 0, 0, 0, 0);
    for (const auto& r : res.storage_buffers) bind(r, 1, kStorageBufferBase, 0, 0);
    for (const auto& r : res.sampled_images) bind(r, 2, 0, 0, 0);
    for (const auto& r : res.storage_images) bind(r, 3, 0, kStorageImageBase, 0);
}

spv::ExecutionModel execution_model(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex: return spv::ExecutionModelVertex;
        case ShaderStage::Fragment: return spv::ExecutionModelFragment;
        case ShaderStage::Compute: return spv::ExecutionModelGLCompute;
    }
    return spv::ExecutionModelVertex;
}

// SPIRV-Cross bakes the Y flip in; make it a function constant so one library serves both the
// backbuffer (no flip) and texture targets (flip).
bool make_flip_switchable(std::string& msl) {  // mce-allow: SPIRV-Cross output
    constexpr StringView kMarker = "// Invert Y-axis for Metal";
    constexpr StringView kGuard = "if (mce_flip_y) ";
    usize found = 0;
    for (usize at = msl.find(kMarker.data()); at != std::string::npos;  // mce-allow
         at = msl.find(kMarker.data(), at + kGuard.size() + kMarker.size())) {
        usize line = msl.rfind('\n', at);
        line = line == std::string::npos ? 0 : line + 1;  // mce-allow
        while (line < msl.size() && (msl[line] == ' ' || msl[line] == '\t')) ++line;
        msl.insert(line, kGuard.data(), kGuard.size());
        ++found;
    }
    const usize ns = msl.find("using namespace metal;");
    if (found == 0 || ns == std::string::npos) return false;  // mce-allow
    msl.insert(ns + std::strlen("using namespace metal;"),
               "\n\nconstant bool mce_flip_y [[function_constant(" + std::to_string(kFlipYConstant) + ")]];");
    return true;
}

}  // namespace

Result<MslSource> translate_glsl(ShaderStage stage, StringView glsl, const char* debug_name, FixedString<1024>& log) {
    const char* name = debug_name ? debug_name : "";
    SpirV spirv;
    if (!glsl_to_spirv(stage, glsl, spirv, log)) return Error(Errc::ShaderCompile, "rhi.metal.glslang", name);

    std::string text;  // mce-allow: SPIRV-Cross output
    u32 local_size[3] = {1, 1, 1};
    try {
        spirv_cross::CompilerMSL msl(std::move(spirv));
        const spv::ExecutionModel model = execution_model(stage);
        spirv_cross::CompilerMSL::Options mo;
        mo.platform = spirv_cross::CompilerMSL::Options::macOS;
        mo.set_msl_version(2, 3);
        mo.enable_point_size_builtin = true;
        msl.set_msl_options(mo);
        spirv_cross::CompilerGLSL::Options go = msl.get_common_options();
        go.vertex.fixup_clipspace = true;  // GL clip z in [-w, w] -> Metal [0, w]
        go.vertex.flip_vert_y = stage == ShaderStage::Vertex;
        msl.set_common_options(go);
        remap_resources(msl, model);
        text = msl.compile();
        if (stage == ShaderStage::Compute)
            for (u32 i = 0; i < 3; ++i)
                local_size[i] = msl.get_execution_mode_argument(spv::ExecutionModeLocalSize, i);
    } catch (const spirv_cross::CompilerError& e) {
        log.append(e.what());
        return Error(Errc::ShaderCompile, "rhi.metal.spirv_cross", name);
    }
    if (stage == ShaderStage::Vertex && !make_flip_switchable(text)) {
        log.append("vertex shader writes no gl_Position");
        return Error(Errc::ShaderCompile, "rhi.metal.flip", name);
    }
    MslSource out = MslSource::copy_of(StringView(text.data(), text.size()));
    for (u32 i = 0; i < 3; ++i) out.local_size[i] = local_size[i] ? local_size[i] : 1;
    return out;
}

}  // namespace mce::rhi::metal
