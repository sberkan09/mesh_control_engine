// GLSL -> SPIR-V -> WGSL for WebGPU (see shader_wgsl.h). The only translation unit of the backend
// that sees glslang and Tint; their std:: API types stay inside it.

#include "shader_wgsl.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <cstdlib>
#include <cstring>
#include <string>  // mce-allow: glslang input text, Tint output text
#include <vector>  // mce-allow: glslang and Tint take SPIR-V as std::vector<uint32_t>

#include "src/tint/api/common/binding_point.h"
#include "src/tint/lang/core/ir/module.h"
#include "src/tint/lang/spirv/reader/reader.h"
#include "src/tint/lang/wgsl/writer/writer.h"

namespace mce::rhi::webgpu {
namespace {

using Text = std::string;  // mce-allow: glslang input text

void init_glslang() {
    static bool done = false;  // one thread: the device's (web builds are single-threaded at this layer)
    if (done) return;
    done = true;
    glslang::InitializeProcess();
    std::atexit([] { glslang::FinalizeProcess(); });
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

// Renames the vertex entry point `void main(` to mce_user_main and appends a main that calls it,
// then fixes clip z for WebGPU's [0, w] and negates y (shader_wgsl.h). False when there is no main.
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
    const EShLanguage lang = stage == ShaderStage::Vertex     ? EShLangVertex
                             : stage == ShaderStage::Fragment ? EShLangFragment
                                                              : EShLangCompute;
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

constexpr u32 kOpExecutionMode = 16;
constexpr u32 kOpTypeImage = 25;
constexpr u32 kOpTypeSampledImage = 27;
constexpr u32 kOpTypeArray = 28;
constexpr u32 kOpTypeStruct = 30;
constexpr u32 kOpTypePointer = 32;
constexpr u32 kOpVariable = 59;
constexpr u32 kOpDecorate = 71;
constexpr u32 kOpMemberDecorate = 72;
constexpr u32 kExecutionModeLocalSize = 17;
constexpr u32 kDecorationNonWritable = 24;
constexpr u32 kDecorationBlock = 2;
constexpr u32 kDecorationBufferBlock = 3;
constexpr u32 kDecorationBinding = 33;
constexpr u32 kDecorationDescriptorSet = 34;
constexpr u32 kStorageUniformConstant = 0;
constexpr u32 kStorageUniform = 2;
constexpr u32 kStorageStorageBuffer = 12;
constexpr u32 kDim2D = 1;
constexpr u32 kImageSampledStorage = 2;  // OpTypeImage "Sampled" operand: used without a sampler

struct IdInfo {
    u32 op = 0;
    u32 a = 0, b = 0, c = 0, d = 0, e = 0;  // per-op operands (see the parse loop)
    u32 binding_word = 0;                   // index of the Binding literal, 0 = none
    u32 set_word = 0;                       // index of the DescriptorSet literal, 0 = none
    u32 members = 0;                        // OpTypeStruct member count
    u32 nonwritable_members = 0;            // members decorated NonWritable
    bool block = false, buffer_block = false, nonwritable = false;
};

ResultV rewrite_bindings(ShaderStage stage, Array<u32>& words, StageResources& res, FixedString<1024>& log,
                         const char* name) {
    if (words.size() < 5) return Error(Errc::ShaderCompile, "rhi.webgpu.spirv", name);
    const u32 bound = words[3];
    Array<IdInfo> ids;
    ids.resize(bound);
    Array<u32> variables;
    for (usize i = 5; i < words.size();) {
        const u32 count = words[i] >> 16, op = words[i] & 0xFFFFu;
        if (count == 0 || i + count > words.size()) return Error(Errc::ShaderCompile, "rhi.webgpu.spirv", name);
        auto id = [&](usize k) -> IdInfo* { return words[i + k] < bound ? &ids[words[i + k]] : nullptr; };
        switch (op) {
            case kOpTypeImage:  // result, sampled type, dim, depth, arrayed, ms, sampled -> a, b, c, d, e
                if (IdInfo* r = id(1); r && count >= 8) {
                    r->op = op;
                    r->a = words[i + 2];
                    r->b = words[i + 3];
                    r->c = words[i + 4];
                    r->d = words[i + 5];
                    r->e = words[i + 7];
                }
                break;
            case kOpTypeSampledImage:  // result, image type
            case kOpTypeArray:         // result, element type, length
                if (IdInfo* r = id(1)) {
                    r->op = op;
                    r->a = words[i + 2];
                }
                break;
            case kOpTypeStruct:  // result, member types
                if (IdInfo* r = id(1)) {
                    r->op = op;
                    r->members = count - 2;
                }
                break;
            case kOpExecutionMode:  // entry point, mode, literals
                if (count >= 6 && words[i + 2] == kExecutionModeLocalSize)
                    for (u32 k = 0; k < 3; ++k) res.local_size[k] = words[i + 3 + k];
                break;
            case kOpMemberDecorate:  // struct, member, decoration
                if (IdInfo* t = id(1); t && count >= 4 && words[i + 3] == kDecorationNonWritable) ++t->nonwritable_members;
                break;
            case kOpTypePointer:  // result, storage class, type
                if (IdInfo* r = id(1)) {
                    r->op = op;
                    r->a = words[i + 2];
                    r->b = words[i + 3];
                }
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
                    if (deco == kDecorationNonWritable) t->nonwritable = true;
                }
                break;
            default: break;
        }
        i += count;
    }

    for (const u32 v : variables) {
        const IdInfo& var = ids[v];
        if (!var.binding_word) continue;
        if (var.a >= bound || ids[var.a].op != kOpTypePointer) continue;
        const u32 storage = ids[var.a].a;
        u32 type = ids[var.a].b;
        while (type < bound && ids[type].op == kOpTypeArray) type = ids[type].a;
        if (type >= bound) continue;
        const u32 gl = words[var.binding_word];
        u32 group = 0;
        if (storage == kStorageUniformConstant && ids[type].op == kOpTypeSampledImage) {
            const u32 image = ids[type].a;
            if (image >= bound || ids[image].op != kOpTypeImage || ids[image].b != kDim2D) {
                log.append("only 2D and 2D-array samplers are supported");
                return Error(Errc::ShaderCompile, "rhi.webgpu.spirv", name);
            }
            if (gl >= kMaxSamplers) {
                log.append("sampler binding outside 0..15");
                return Error(Errc::ShaderCompile, "rhi.webgpu.spirv", name);
            }
            const bool depth = ids[image].c == 1, arrayed = ids[image].d == 1;
            res.sampler_mask |= 1u << gl;
            res.sampler_kind[gl] = depth ? (arrayed ? SamplerKind::Shadow2DArray : SamplerKind::Shadow2D)
                                         : (arrayed ? SamplerKind::Tex2DArray : SamplerKind::Tex2D);
            group = kGroupTextures;
        } else if (storage == kStorageUniformConstant && ids[type].op == kOpTypeImage && ids[type].e == kImageSampledStorage) {
            if (stage != ShaderStage::Compute) {
                log.append("storage images are supported in compute shaders only");
                return Error(Errc::ShaderCompile, "rhi.webgpu.spirv", name);
            }
            if (ids[type].b != kDim2D || gl >= kMaxImageUnits) {
                log.append("only 2D storage images on units 0..7 are supported");
                return Error(Errc::ShaderCompile, "rhi.webgpu.spirv", name);
            }
            res.image_mask |= 1u << gl;
            group = kGroupImages;
        } else if (storage == kStorageStorageBuffer || (storage == kStorageUniform && ids[type].buffer_block)) {
            if (gl >= kMaxStorageBindings) {
                log.append("storage binding outside 0..15");
                return Error(Errc::ShaderCompile, "rhi.webgpu.spirv", name);
            }
            const IdInfo& block = ids[type];
            const bool readonly = var.nonwritable || (block.members > 0 && block.nonwritable_members >= block.members);
            if (!readonly && stage != ShaderStage::Compute) {
                log.append("writable storage buffers are supported in compute shaders only");
                return Error(Errc::ShaderCompile, "rhi.webgpu.spirv", name);
            }
            res.storage_mask |= 1u << gl;
            if (!readonly) res.storage_rw_mask |= 1u << gl;
            group = kGroupStorage;
        } else if (storage == kStorageUniform && ids[type].block) {
            if (gl >= kMaxUniformBanks) {
                log.append("uniform block binding outside the banks 0..3");
                return Error(Errc::ShaderCompile, "rhi.webgpu.spirv", name);
            }
            res.uniform_mask |= 1u << gl;
            group = kGroupUniforms;
        } else {
            continue;  // inputs, outputs, push constants: untouched
        }
        if (!var.set_word) {
            log.append("resource without a DescriptorSet decoration");
            return Error(Errc::ShaderCompile, "rhi.webgpu.spirv", name);
        }
        words[var.set_word] = group;  // the binding number stays the GL one (shader_wgsl.h)
    }
    return ok();
}

bool spirv_to_wgsl(const Array<u32>& words, const StageResources& res, Array<char>& out, FixedString<1024>& log) {
    std::vector<uint32_t> input(words.begin(), words.end());  // mce-allow: Tint API type
    tint::spirv::reader::Options options;
    for (u32 s = 0; s < kMaxSamplers; ++s)
        if (res.sampler_mask & (1u << s))
            options.sampler_mappings[tint::BindingPoint{kGroupTextures, s}] =
                tint::BindingPoint{kGroupTextures, kSamplerBindingBase + s};
    auto ir = tint::spirv::reader::ReadIR(input, options);
    if (ir != tint::Success) {
        log.append(ir.Failure().reason.c_str());
        return false;
    }
    tint::wgsl::writer::Options wopts;
    auto result = tint::wgsl::writer::WgslFromIR(ir.Get(), wopts);
    if (result != tint::Success) {
        log.append(result.Failure().reason.c_str());
        return false;
    }
    const std::string& text = result->wgsl;  // mce-allow: Tint API type
    out.resize(text.size() + 1);
    std::memcpy(out.data(), text.c_str(), text.size() + 1);
    return true;
}

}  // namespace

ResultV compile_glsl(ShaderStage stage, StringView glsl, const char* debug_name, Array<char>& wgsl,
                     StageResources& resources, FixedString<1024>& log) {
    const char* name = debug_name ? debug_name : "";
    Text text(glsl.data(), glsl.size());
    replace_word(text, "gl_VertexID", "gl_VertexIndex");
    replace_word(text, "gl_InstanceID", "gl_InstanceIndex");
    if (stage == ShaderStage::Vertex && !wrap_vertex_main(text)) {
        log.append("vertex shader has no main()");
        return Error(Errc::ShaderCompile, "rhi.webgpu.glslang", name);
    }

    std::vector<u32> words;  // mce-allow: glslang API type
    if (!glsl_to_spirv(stage, text, words, log)) return Error(Errc::ShaderCompile, "rhi.webgpu.glslang", name);
    Array<u32> spirv;
    spirv.resize(words.size());
    for (usize i = 0; i < words.size(); ++i) spirv[i] = words[i];
    resources = StageResources{};
    MCE_TRY(rewrite_bindings(stage, spirv, resources, log, name));
    if (!spirv_to_wgsl(spirv, resources, wgsl, log)) return Error(Errc::ShaderCompile, "rhi.webgpu.tint", name);
    return ok();
}

}  // namespace mce::rhi::webgpu
