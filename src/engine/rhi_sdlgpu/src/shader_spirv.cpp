// GLSL -> SPIR-V for SDL GPU (see shader_spirv.h). The only translation unit of the backend that
// sees glslang; its std:: API types stay inside it.

#include "shader_spirv.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <cstdlib>
#include <mutex>
#include <string>  // mce-allow: glslang input text
#include <vector>  // mce-allow: glslang returns SPIR-V as std::vector<uint32_t>

namespace mce::rhi::sdlgpu {
namespace {

using Text = std::string;  // mce-allow: glslang input text

void init_glslang() {
    static std::once_flag once;
    std::call_once(once, [] {
        glslang::InitializeProcess();
        std::atexit([] { glslang::FinalizeProcess(); });
    });
}

bool is_ident(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

// Replaces every whole-word `from` with `to`; returns the number of replacements.
u32 replace_word(Text& s, StringView from, StringView to) {
    u32 n = 0;
    usize at = 0;
    while ((at = s.find(from.data(), at, from.size())) != Text::npos) {
        const bool whole = (at == 0 || !is_ident(s[at - 1])) && (at + from.size() >= s.size() || !is_ident(s[at + from.size()]));
        if (!whole) {
            at += from.size();
            continue;
        }
        s.replace(at, from.size(), to.data(), to.size());
        at += to.size();
        ++n;
    }
    return n;
}

void insert_after_version(Text& s, StringView line) {
    const usize v = s.find("#version");
    const usize eol = v == Text::npos ? Text::npos : s.find('\n', v);
    if (eol != Text::npos) s.insert(eol + 1, line.data(), line.size());
}

// Renames the vertex entry point `void main(` to mce_user_main and appends a main that calls it,
// then fixes clip z for Vulkan's [0, w] and negates y (shader_spirv.h). False when there is no main.
bool wrap_vertex_main(Text& s) {
    usize at = 0;
    while ((at = s.find("main", at)) != Text::npos) {
        const bool whole = (at == 0 || !is_ident(s[at - 1])) && (at + 4 >= s.size() || !is_ident(s[at + 4]));
        usize p = at + 4;
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
        usize b = at;
        while (b > 0 && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) --b;
        const bool after_void = b >= 4 && s.compare(b - 4, 4, "void") == 0 && (b == 4 || !is_ident(s[b - 5]));
        if (whole && after_void && p < s.size() && s[p] == '(') {
            s.replace(at, 4, "mce_user_main");
            s.append(
                "\nvoid main() {\n"
                "    mce_user_main();\n"
                "    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n"
                "    gl_Position.y = -gl_Position.y;\n"
                "}\n");
            return true;
        }
        at += 4;
    }
    return false;
}

bool glsl_to_spirv(ShaderStage stage, const Text& text, std::vector<u32>& out, FixedString<1024>& log) {  // mce-allow
    init_glslang();
    const EShLanguage lang = stage == ShaderStage::Vertex ? EShLangVertex : EShLangFragment;
    glslang::TShader shader(lang);
    const char* src = text.c_str();
    const int len = static_cast<int>(text.size());
    shader.setStringsWithLengths(&src, &len, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    const auto msgs = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
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

// ---- SPIR-V binding rewrite --------------------------------------------------------------------

constexpr u32 kOpTypeImage = 25;
constexpr u32 kOpTypeSampledImage = 27;
constexpr u32 kOpTypeArray = 28;
constexpr u32 kOpTypeStruct = 30;
constexpr u32 kOpTypePointer = 32;
constexpr u32 kOpVariable = 59;
constexpr u32 kOpDecorate = 71;
constexpr u32 kDecorationBlock = 2;
constexpr u32 kDecorationBufferBlock = 3;
constexpr u32 kDecorationBinding = 33;
constexpr u32 kDecorationDescriptorSet = 34;
constexpr u32 kStorageUniformConstant = 0;
constexpr u32 kStorageUniform = 2;
constexpr u32 kStorageStorageBuffer = 12;
constexpr u32 kDim2D = 1;

enum class Kind : u8 { None, Sampler, Uniform, Storage };

struct IdInfo {
    u32 op = 0;
    u32 a = 0, b = 0, c = 0, d = 0;  // per-op operands (see collect)
    u32 binding_word = 0;            // index of the Binding literal, 0 = none
    u32 set_word = 0;                // index of the DescriptorSet literal, 0 = none
    bool block = false, buffer_block = false;
};

struct Resource {
    Kind kind = Kind::None;
    u32 gl_binding = 0;
    u32 binding_word = 0;
    u32 set_word = 0;
    SamplerKind sampler = SamplerKind::Tex2D;
};

ResultV rewrite_bindings(ShaderStage stage, Array<u32>& words, StageResources& res, FixedString<1024>& log,
                         const char* name) {
    if (words.size() < 5) return Error(Errc::ShaderCompile, "rhi.sdlgpu.spirv", name);
    const u32 bound = words[3];
    Array<IdInfo> ids;
    ids.resize(bound);
    Array<u32> variables;
    for (usize i = 5; i < words.size();) {
        const u32 count = words[i] >> 16, op = words[i] & 0xFFFFu;
        if (count == 0 || i + count > words.size()) return Error(Errc::ShaderCompile, "rhi.sdlgpu.spirv", name);
        auto id = [&](usize k) -> IdInfo* { return words[i + k] < bound ? &ids[words[i + k]] : nullptr; };
        switch (op) {
            case kOpTypeImage:  // result, sampled type, dim, depth, arrayed -> a, b, c, d
                if (IdInfo* r = id(1)) *r = IdInfo{op, words[i + 2], words[i + 3], words[i + 4], words[i + 5]};
                break;
            case kOpTypeSampledImage:  // result, image type
            case kOpTypeArray:         // result, element type, length
                if (IdInfo* r = id(1)) { r->op = op; r->a = words[i + 2]; }
                break;
            case kOpTypeStruct:
                if (IdInfo* r = id(1)) r->op = op;
                break;
            case kOpTypePointer:  // result, storage class, type
                if (IdInfo* r = id(1)) { r->op = op; r->a = words[i + 2]; r->b = words[i + 3]; }
                break;
            case kOpVariable:  // result type, result, storage class
                if (IdInfo* r = id(2)) {
                    r->op = op;
                    r->a = words[i + 1];
                    r->b = words[i + 3];
                    variables.push_back(words[i + 2]);
                }
                break;
            case kOpDecorate:  // target, decoration, literals
                if (IdInfo* t = id(1); t && count >= 3) {
                    const u32 deco = words[i + 2];
                    if (deco == kDecorationBinding && count >= 4) t->binding_word = static_cast<u32>(i + 3);
                    if (deco == kDecorationDescriptorSet && count >= 4) t->set_word = static_cast<u32>(i + 3);
                    if (deco == kDecorationBlock) t->block = true;
                    if (deco == kDecorationBufferBlock) t->buffer_block = true;
                }
                break;
            default: break;
        }
        i += count;
    }

    Array<Resource> found;
    for (const u32 v : variables) {
        const IdInfo& var = ids[v];
        if (!var.binding_word) continue;
        if (var.a >= bound || ids[var.a].op != kOpTypePointer) continue;
        const u32 storage = ids[var.a].a;
        u32 type = ids[var.a].b;
        while (type < bound && ids[type].op == kOpTypeArray) type = ids[type].a;
        if (type >= bound) continue;
        Resource r;
        r.gl_binding = words[var.binding_word];
        r.binding_word = var.binding_word;
        r.set_word = var.set_word;
        if (storage == kStorageUniformConstant && ids[type].op == kOpTypeSampledImage) {
            const u32 image = ids[type].a;
            if (image >= bound || ids[image].op != kOpTypeImage || ids[image].b != kDim2D) {
                log.append("only 2D and 2D-array samplers are supported");
                return Error(Errc::ShaderCompile, "rhi.sdlgpu.spirv", name);
            }
            const bool depth = ids[image].c == 1, arrayed = ids[image].d == 1;
            r.kind = Kind::Sampler;
            r.sampler = depth ? (arrayed ? SamplerKind::Shadow2DArray : SamplerKind::Shadow2D)
                              : (arrayed ? SamplerKind::Tex2DArray : SamplerKind::Tex2D);
        } else if (storage == kStorageStorageBuffer || (storage == kStorageUniform && ids[type].buffer_block)) {
            r.kind = Kind::Storage;
        } else if (storage == kStorageUniform && ids[type].block) {
            r.kind = Kind::Uniform;
        } else {
            continue;  // inputs, outputs, push constants: untouched
        }
        if (!r.set_word) {
            log.append("resource without a DescriptorSet decoration");
            return Error(Errc::ShaderCompile, "rhi.sdlgpu.spirv", name);
        }
        found.push_back(r);
    }

    // Samplers and storage buffers are compacted in GL-binding order; uniform banks keep their number.
    auto sort_by_binding = [&](Kind k, u8* out, u32 cap, u8& count) -> bool {
        count = 0;
        for (u32 gl = 0; gl < 256; ++gl) {
            for (const Resource& r : found) {
                if (r.kind != k || r.gl_binding != gl) continue;
                if (count >= cap) return false;
                out[count++] = static_cast<u8>(gl);
                break;
            }
        }
        return true;
    };
    if (!sort_by_binding(Kind::Sampler, res.sampler_slot, kMaxStageSamplers, res.sampler_count) ||
        !sort_by_binding(Kind::Storage, res.storage_binding, kMaxStageStorage, res.storage_count)) {
        log.append("too many samplers or storage buffers in one stage");
        return Error(Errc::ShaderCompile, "rhi.sdlgpu.spirv", name);
    }
    const u32 resource_set = stage == ShaderStage::Vertex ? 0 : 2;
    const u32 uniform_set = resource_set + 1;
    res.uniform_count = 0;
    for (const Resource& r : found) {
        switch (r.kind) {
            case Kind::Sampler:
                for (u32 i = 0; i < res.sampler_count; ++i)
                    if (res.sampler_slot[i] == r.gl_binding) {
                        res.sampler_kind[i] = r.sampler;
                        words[r.binding_word] = i;
                    }
                words[r.set_word] = resource_set;
                break;
            case Kind::Storage:
                for (u32 i = 0; i < res.storage_count; ++i)
                    if (res.storage_binding[i] == r.gl_binding) words[r.binding_word] = res.sampler_count + i;
                words[r.set_word] = resource_set;
                break;
            case Kind::Uniform:
                if (r.gl_binding >= kMaxUniformBanks) {
                    log.append("uniform block binding outside the banks 0..3");
                    return Error(Errc::ShaderCompile, "rhi.sdlgpu.spirv", name);
                }
                res.uniform_count = static_cast<u8>(r.gl_binding + 1 > res.uniform_count ? r.gl_binding + 1 : res.uniform_count);
                words[r.set_word] = uniform_set;
                break;
            case Kind::None: break;
        }
    }
    return ok();
}

}  // namespace

ResultV compile_glsl(ShaderStage stage, StringView glsl, const char* debug_name, Array<u32>& spirv,
                     StageResources& resources, FixedString<1024>& log) {
    const char* name = debug_name ? debug_name : "";
    Text text(glsl.data(), glsl.size());
    replace_word(text, "gl_VertexID", "gl_VertexIndex");
    if (replace_word(text, "gl_InstanceID", "(gl_InstanceIndex - gl_BaseInstanceARB)") > 0 &&
        text.find("GL_ARB_shader_draw_parameters") == Text::npos)
        insert_after_version(text, "#extension GL_ARB_shader_draw_parameters : enable\n");
    if (stage == ShaderStage::Vertex && !wrap_vertex_main(text)) {
        log.append("vertex shader has no main()");
        return Error(Errc::ShaderCompile, "rhi.sdlgpu.glslang", name);
    }

    std::vector<u32> words;  // mce-allow: glslang API type
    if (!glsl_to_spirv(stage, text, words, log)) return Error(Errc::ShaderCompile, "rhi.sdlgpu.glslang", name);
    spirv.resize(words.size());
    for (usize i = 0; i < words.size(); ++i) spirv[i] = words[i];
    resources = StageResources{};
    return rewrite_bindings(stage, spirv, resources, log, name);
}

}  // namespace mce::rhi::sdlgpu
