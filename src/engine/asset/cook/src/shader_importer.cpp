// .glsl -> CHNK_SHD: one GLSL 4.5 stage, validated per variant (assets.md §5.2, FR-AST-060).
//
// The stage comes from the file name: name.vert.glsl, name.frag.glsl, name.comp.glsl. Variants are
// declared in the source with header comment lines, each a boolean define:
//
//   #version 450 core
//   // @variant LIT          -> LIT is 0 or 1; two defines = four variants
//   // @variant ALPHA_TEST
//
// Every combination is compiled by glslang (OpenGL client semantics, as the GL driver will see it)
// to SPIR-V; a compile error is a cook error at the source line. The cook then checks the
// binding-bank table (rhi/types.h `bank`, rhi-renderer.md §3.3): every uniform block, storage
// block and sampler has an explicit layout(binding), uniform blocks use 0..3 (Frame, Pass,
// Material, Object), samplers 6..15, storage blocks 0..7. The chunk keeps the authored source (GL
// recompiles it, composed with the variant's defines), the SPIR-V and a reflection list per
// variant (FR-AST-060's stored reflection blob).
//
// sidecar settings: none.

#include <glslang/Include/Types.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>  // mce-allow: glslang returns SPIR-V as std::vector

#include "importer_common.h"
#include "mce/asset/formats.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.shader";

using asset::ShaderResource;
using asset::ShaderResourceKind;
using asset::ShaderStageKind;
using asset::ShaderVariant;

void init_glslang() {
    static std::once_flag once;
    std::call_once(once, [] {
        glslang::InitializeProcess();
        std::atexit([] { glslang::FinalizeProcess(); });
    });
}

bool ends_with(StringView s, StringView suffix) { return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix; }

bool stage_of(StringView path, ShaderStageKind& out) {
    if (ends_with(path, ".vert.glsl")) out = ShaderStageKind::Vertex;
    else if (ends_with(path, ".frag.glsl")) out = ShaderStageKind::Fragment;
    else if (ends_with(path, ".comp.glsl")) out = ShaderStageKind::Compute;
    else return false;
    return true;
}

EShLanguage language(ShaderStageKind s) {
    switch (s) {
        case ShaderStageKind::Vertex: return EShLangVertex;
        case ShaderStageKind::Fragment: return EShLangFragment;
        case ShaderStageKind::Compute: return EShLangCompute;
    }
    return EShLangVertex;
}

bool is_ident(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

// "// @variant NAME" lines; `line` is 1-based for diagnostics.
struct VariantDecl {
    StringView name;
    u32 line;
};

void scan_variants(StringView src, Array<VariantDecl>& out, StringView file, DiagList& diags) {
    u32 line = 1;
    usize i = 0;
    while (i < src.size()) {
        usize eol = src.find('\n', i);
        if (eol == StringView::npos) eol = src.size();
        StringView l = src.substr(i, eol - i);
        while (!l.empty() && (l.front() == ' ' || l.front() == '\t')) l = l.substr(1);
        if (l.size() >= 2 && l.substr(0, 2) == "//") {
            l = l.substr(2);
            while (!l.empty() && (l.front() == ' ' || l.front() == '\t')) l = l.substr(1);
            if (l.size() >= 8 && l.substr(0, 8) == "@variant") {
                l = l.substr(8);
                while (!l.empty() && (l.front() == ' ' || l.front() == '\t')) l = l.substr(1);
                usize n = 0;
                while (n < l.size() && is_ident(l[n])) ++n;
                const StringView name = l.substr(0, n);
                StringView rest = l.substr(n);
                while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t' || rest.front() == '\r')) rest = rest.substr(1);
                if (name.empty() || (name[0] >= '0' && name[0] <= '9') || name.size() >= 24 || !rest.empty()) {
                    diags.error(Errc::ParseError, file, line, 1, kStage, "'// @variant' takes one identifier of at most 23 characters");
                } else {
                    bool dup = false;
                    for (const VariantDecl& d : out) dup |= d.name == name;
                    if (dup) diags.error(Errc::ParseError, file, line, 1, kStage, "duplicate '// @variant'");
                    else out.push_back(VariantDecl{name, line});
                }
            }
        }
        i = eol + 1;
        ++line;
    }
    if (out.size() > asset::kMaxShaderDefines) {
        String msg;
        appendf(msg, "at most %u '// @variant' defines (%u variants)", asset::kMaxShaderDefines, 1u << asset::kMaxShaderDefines);
        diags.error(Errc::ParseError, file, out[asset::kMaxShaderDefines].line, 1, kStage, msg);
    }
}

// glslang info log lines look like "ERROR: 0:12: 'foo' : undeclared identifier"; report each at its
// line of the authored source (the variant preamble adds no lines: it is a separate string).
void report_log(const char* log, StringView file, u32 mask, Span<const VariantDecl> defines, DiagList& diags) {
    String variant;
    for (usize i = 0; i < defines.size(); ++i) {
        appendf(variant, "%s%.*s=%u", i ? " " : "", static_cast<int>(defines[i].name.size()), defines[i].name.data(), (mask >> i) & 1u);
    }
    StringView rest(log);
    bool any = false;
    while (!rest.empty()) {
        usize eol = rest.find('\n');
        if (eol == StringView::npos) eol = rest.size();
        const StringView l = rest.substr(0, eol);
        rest = rest.substr(eol < rest.size() ? eol + 1 : eol);
        if (l.size() < 6 || l.substr(0, 6) != "ERROR:") continue;
        StringView m = l.substr(6);
        while (!m.empty() && m.front() == ' ') m = m.substr(1);
        u32 line = 0;
        // "<string>:<line>: message"
        const usize c1 = m.find(':');
        if (c1 != StringView::npos) {
            const usize c2 = m.find(':', c1 + 1);
            if (c2 != StringView::npos) {
                u32 v = 0;
                bool digits = c2 > c1 + 1;
                for (usize k = c1 + 1; k < c2; ++k) {
                    if (m[k] < '0' || m[k] > '9') digits = false;
                    else v = v * 10 + static_cast<u32>(m[k] - '0');
                }
                if (digits) {
                    line = v;
                    m = m.substr(c2 + 1);
                    while (!m.empty() && m.front() == ' ') m = m.substr(1);
                }
            }
        }
        if (m.empty()) continue;
        String msg;
        appendf(msg, "%.*s", static_cast<int>(m.size()), m.data());
        if (!variant.empty()) appendf(msg, " [variant %.*s]", static_cast<int>(variant.view().size()), variant.view().data());
        diags.error(Errc::ShaderCompile, file, line, 1, kStage, msg);
        any = true;
    }
    if (!any) diags.error(Errc::ShaderCompile, file, 0, 0, kStage, "glslang rejected the shader");
}

void put_name(ShaderResource& r, const char* name) {
    const usize n = std::strlen(name);
    std::memcpy(r.name, name, n < sizeof(r.name) - 1 ? n : sizeof(r.name) - 1);
}

bool conformant(const ShaderResource& r, const char* full_name, StringView file, DiagList& diags) {
    const char* what = nullptr;
    i32 lo = 0, hi = 0;
    switch (r.kind) {
        case ShaderResourceKind::UniformBlock: what = "uniform block", lo = 0, hi = 3; break;
        case ShaderResourceKind::StorageBlock: what = "storage block", lo = 0, hi = 7; break;
        case ShaderResourceKind::Sampler: what = "sampler", lo = 6, hi = 15; break;
        default: return true;
    }
    if (r.binding == 0xFFFFu) {
        String msg;
        appendf(msg, "%s '%s' needs an explicit layout(binding = N) (binding banks, rhi-renderer.md §3.3)", what, full_name);
        diags.error(Errc::ParseError, file, 0, 0, kStage, msg);
        return false;
    }
    if (static_cast<i32>(r.binding) < lo || static_cast<i32>(r.binding) > hi) {
        String msg;
        appendf(msg, "%s '%s' uses binding %u; the bank table allows %d..%d (rhi-renderer.md §3.3)", what, full_name, r.binding, lo,
                hi);
        diags.error(Errc::ParseError, file, 0, 0, kStage, msg);
        return false;
    }
    return true;
}

u16 binding_of(int b) { return b < 0 ? u16(0xFFFF) : static_cast<u16>(b); }

struct Compiled {
    u32 mask = 0;
    Array<u32> spirv;
    Array<ShaderResource> resources;
};

bool compile_variant(ShaderStageKind stage, StringView src, u32 mask, Span<const VariantDecl> defines, Compiled& out, StringView file,
                     DiagList& diags) {
    init_glslang();
    const EShLanguage lang = language(stage);
    glslang::TShader shader(lang);
    const char* text = src.data();
    const int len = static_cast<int>(src.size());
    shader.setStringsWithLengths(&text, &len, 1);
    String preamble;
    for (usize i = 0; i < defines.size(); ++i)
        appendf(preamble, "#define %.*s %u\n", static_cast<int>(defines[i].name.size()), defines[i].name.data(), (mask >> i) & 1u);
    shader.setPreamble(preamble.c_str());
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientOpenGL, 450);
    shader.setEnvClient(glslang::EShClientOpenGL, glslang::EShTargetOpenGL_450);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    const auto msgs = static_cast<EShMessages>(EShMsgSpvRules);
    if (!shader.parse(GetDefaultResources(), 450, ECoreProfile, false, false, msgs)) {
        report_log(shader.getInfoLog(), file, mask, defines, diags);
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msgs)) {
        report_log(program.getInfoLog(), file, mask, defines, diags);
        return false;
    }
    program.buildReflection(EShReflectionIntermediateIO | EShReflectionSeparateBuffers);

    bool ok = true;
    auto add = [&](ShaderResourceKind kind, const char* name, int binding, int size) {
        ShaderResource r{};
        r.kind = kind;
        put_name(r, name);
        r.binding = binding_of(binding);
        r.size = size > 0 ? static_cast<u32>(size) : 0u;
        ok &= conformant(r, name, file, diags);
        out.resources.push_back(r);
    };
    for (int i = 0; i < program.getNumUniformBlocks(); ++i) {
        const glslang::TObjectReflection& b = program.getUniformBlock(i);
        add(ShaderResourceKind::UniformBlock, b.name.c_str(), b.getBinding(), b.size);
    }
    for (int i = 0; i < program.getNumBufferBlocks(); ++i) {
        const glslang::TObjectReflection& b = program.getBufferBlock(i);
        add(ShaderResourceKind::StorageBlock, b.name.c_str(), b.getBinding(), b.size);
    }
    for (int i = 0; i < program.getNumUniformVariables(); ++i) {
        const glslang::TObjectReflection& u = program.getUniform(i);
        const glslang::TType* t = u.getType();
        if (!t || t->getBasicType() != glslang::EbtSampler) continue;
        add(ShaderResourceKind::Sampler, u.name.c_str(), u.getBinding(), 0);
    }
    for (int i = 0; i < program.getNumPipeInputs(); ++i) {
        const glslang::TObjectReflection& v = program.getPipeInput(i);
        if (v.name.compare(0, 3, "gl_") == 0) continue;
        add(ShaderResourceKind::Input, v.name.c_str(), static_cast<int>(v.layoutLocation()), 0);
    }
    for (int i = 0; i < program.getNumPipeOutputs(); ++i) {
        const glslang::TObjectReflection& v = program.getPipeOutput(i);
        if (v.name.compare(0, 3, "gl_") == 0) continue;
        add(ShaderResourceKind::Output, v.name.c_str(), static_cast<int>(v.layoutLocation()), 0);
    }
    if (!ok) return false;

    std::vector<unsigned int> words;  // mce-allow: glslang's SPIR-V output type
    glslang::SpvOptions opts;
    opts.disableOptimizer = true;
    glslang::GlslangToSpv(*program.getIntermediate(lang), words, &opts);
    if (words.empty()) {
        diags.error(Errc::ShaderCompile, file, 0, 0, kStage, "glslang produced no SPIR-V");
        return false;
    }
    out.mask = mask;
    out.spirv.resize(words.size());
    std::memcpy(out.spirv.data(), words.data(), words.size() * sizeof(u32));
    return true;
}

class ShaderImporter final : public IImporter {
public:
    StringView id() const override { return "shader"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::Shader; }
    bool native() const override { return false; }
    bool handles_extension(StringView ext) const override { return ext == ".glsl"; }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const StringView file = in.display_path;
        const u32 errors_before = diags.error_count();
        if (in.settings.valid() && !detail::check_keys(in.settings, {}, file, kStage, diags)) return false;
        ShaderStageKind stage;
        if (!stage_of(in.source_path, stage)) {
            diags.error(Errc::InvalidArgument, file, 0, 0, kStage, "name a shader <name>.vert.glsl, <name>.frag.glsl or <name>.comp.glsl");
            return false;
        }
        const StringView src(reinterpret_cast<const char*>(in.source.data()), in.source.size());
        if (src.find('\0') != StringView::npos) {
            diags.error(Errc::ParseError, file, 0, 0, kStage, "shader source contains a NUL byte");
            return false;
        }
        Array<VariantDecl> defines;
        scan_variants(src, defines, file, diags);
        if (diags.error_count() != errors_before) return false;

        const u32 variant_count = 1u << defines.size();
        Array<Compiled> compiled;
        compiled.resize(variant_count);
        for (u32 mask = 0; mask < variant_count; ++mask)
            compile_variant(stage, src, mask, defines.span(), compiled[mask], file, diags);
        if (diags.error_count() != errors_before) return false;

        asset::ShaderHeader h{};
        h.stage = stage;
        h.define_count = static_cast<u8>(defines.size());
        h.variant_count = static_cast<u16>(variant_count);
        h.source_size = static_cast<u32>(src.size());
        for (usize i = 0; i < defines.size(); ++i) std::memcpy(h.defines[i], defines[i].name.data(), defines[i].name.size());
        for (const Compiled& c : compiled) h.resource_count += static_cast<u32>(c.resources.size());

        const usize variants_at = sizeof(h);
        const usize resources_at = variants_at + variant_count * sizeof(ShaderVariant);
        const usize source_at = resources_at + h.resource_count * sizeof(ShaderResource);
        usize spirv_at = (source_at + src.size() + 3) & ~usize(3);
        Array<byte> payload;
        usize total = spirv_at;
        for (const Compiled& c : compiled) total += c.spirv.size() * sizeof(u32);
        payload.resize(total);
        std::memset(payload.data(), 0, total);
        u32 first = 0;
        for (u32 i = 0; i < variant_count; ++i) {
            const Compiled& c = compiled[i];
            ShaderVariant v{};
            v.mask = c.mask;
            v.spirv_offset = static_cast<u32>(spirv_at);
            v.spirv_words = static_cast<u32>(c.spirv.size());
            v.first_resource = first;
            v.resource_count = static_cast<u32>(c.resources.size());
            std::memcpy(payload.data() + variants_at + i * sizeof(ShaderVariant), &v, sizeof(v));
            std::memcpy(payload.data() + resources_at + first * sizeof(ShaderResource), c.resources.data(),
                        c.resources.size() * sizeof(ShaderResource));
            std::memcpy(payload.data() + spirv_at, c.spirv.data(), c.spirv.size() * sizeof(u32));
            first += v.resource_count;
            spirv_at += c.spirv.size() * sizeof(u32);
        }
        std::memcpy(payload.data(), &h, sizeof(h));
        std::memcpy(payload.data() + source_at, src.data(), src.size());

        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::Shader;
        d.version = asset::chunk_version(asset::SHADER_VERSION, 0);
        d.guid = in.guid;
        out.add(d, payload.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_shader_importer() { return mem::make_unique<ShaderImporter>(mem::heap()); }

}  // namespace mce::cook
