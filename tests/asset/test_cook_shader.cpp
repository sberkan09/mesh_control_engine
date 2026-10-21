// FR-AST-060 / assets.md §5.2: the shader cook — stage from the file name, `// @variant` defines
// enumerated into every combination, glslang validation per variant with errors at the source
// line, the binding-bank conformance check, and the stored SPIR-V + reflection blob.

#include <doctest/doctest.h>

#include <cstring>
#include <string>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"

using namespace mce;
using namespace cook_test;

namespace {

constexpr const char* kVert = R"(#version 450 core
// A fullscreen triangle.
layout(location = 0) out vec2 v_uv;
out gl_PerVertex { vec4 gl_Position; };
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char* kFrag = R"(#version 450 core
// @variant RED
// @variant HALF
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(std140, binding = 1) uniform PassU { vec4 u_tint; };
layout(binding = 6) uniform sampler2DArray u_atlas;
void main() {
#if RED
    vec4 c = vec4(1.0, 0.0, 0.0, 1.0);
#else
    vec4 c = vec4(0.0, 0.0, 1.0, 1.0);
#endif
#if HALF
    c.rgb *= 0.5;
#endif
    o_color = c * u_tint * texture(u_atlas, vec3(v_uv, 0.0));
}
)";

std::string diag_text(const cook::CookReport& r) {
    std::string s;
    for (const cook::Diagnostic& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += std::string(line.view().data(), line.view().size()) + "\n";
    }
    return s;
}

cook::CookReport cook_one(const std::string& root, const char* name, const char* src, const char* guid) {
    write_file(root + "/assets/shaders/" + name, src);
    write_file(root + "/assets/shaders/" + std::string(name) + ".mceasset", sidecar(guid, "shader"));
    cook::CookService svc;
    return svc.cook(StringView(root.c_str()), cook::CookOptions{});
}

}  // namespace

TEST_CASE("shader cook: variants, SPIR-V and reflection land in CHNK_SHD") {
    const std::string root = fresh_dir("shader_cook");
    write_file(root + "/assets/shaders/fullscreen.vert.glsl", kVert);
    write_file(root + "/assets/shaders/fullscreen.vert.glsl.mceasset", sidecar("5d000000-0000-4000-8000-000000000001", "shader"));
    const cook::CookReport r = cook_one(root, "tint.frag.glsl", kFrag, "5d000000-0000-4000-8000-000000000002");
    INFO(diag_text(r));
    REQUIRE(r.ok());

    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));

    auto load = [&](const char* guid) {
        const asset::AssetKey key = assets.find(asset::parse_guid(guid).value());
        REQUIRE(key != asset::kInvalidAssetKey);
        auto h = assets.load_sync<asset::Shader>(key);
        REQUIRE(h);
        auto v = asset::ShaderView::read(assets.resolve(h.value()));
        REQUIRE(v);
        return v.value();
    };
    const asset::ShaderView vs = load("5d000000-0000-4000-8000-000000000001");
    CHECK(vs.header->stage == asset::ShaderStageKind::Vertex);
    CHECK(vs.header->define_count == 0);
    REQUIRE(vs.variants.size() == 1);
    const Span<const u32> words = vs.spirv(vs.variants[0]);
    REQUIRE(words.size() > 5);
    CHECK(words[0] == 0x07230203u);  // the SPIR-V magic
    CHECK(vs.source == StringView(kVert));

    const asset::ShaderView fs = load("5d000000-0000-4000-8000-000000000002");
    CHECK(fs.header->stage == asset::ShaderStageKind::Fragment);
    REQUIRE(fs.header->define_count == 2);
    CHECK(fs.define(0) == StringView("RED"));
    CHECK(fs.define(1) == StringView("HALF"));
    REQUIRE(fs.variants.size() == 4);
    for (u32 mask = 0; mask < 4; ++mask) {
        const asset::ShaderVariant* v = fs.find(mask);
        REQUIRE(v);
        CHECK(fs.spirv(*v)[0] == 0x07230203u);
        bool block = false, sampler = false, input = false, output = false;
        for (const asset::ShaderResource& res : fs.resources_of(*v)) {
            const StringView name(res.name);
            if (res.kind == asset::ShaderResourceKind::UniformBlock && name == StringView("PassU")) {
                block = res.binding == 1 && res.size == 16;
            } else if (res.kind == asset::ShaderResourceKind::Sampler && name == StringView("u_atlas")) {
                sampler = res.binding == 6;
            } else if (res.kind == asset::ShaderResourceKind::Input && name == StringView("v_uv")) {
                input = res.binding == 0;
            } else if (res.kind == asset::ShaderResourceKind::Output && name == StringView("o_color")) {
                output = res.binding == 0;
            }
        }
        CHECK(block);
        CHECK(sampler);
        CHECK(input);
        CHECK(output);
    }
    CHECK(fs.find(4) == nullptr);
    // Different defines, different code.
    const Span<const u32> a = fs.spirv(*fs.find(0)), b = fs.spirv(*fs.find(1));
    CHECK((a.size() != b.size() || std::memcmp(a.data(), b.data(), a.size() * 4) != 0));

    // compose: the defines go right after #version, the rest is the authored text.
    Array<char> text;
    fs.compose(1, text);
    const std::string composed(text.data(), text.size());
    CHECK(composed.rfind("#version 450 core\n#define RED 1\n#define HALF 0\n// @variant RED", 0) == 0);
    CHECK(composed.size() == std::string(kFrag).size() + std::string("#define RED 1\n#define HALF 0\n").size());
}

TEST_CASE("shader cook: compile errors carry the source line and the variant") {
    const std::string root = fresh_dir("shader_cook_errors");
    const cook::CookReport r = cook_one(root, "bad.frag.glsl", R"(#version 450 core
// @variant FANCY
layout(location = 0) out vec4 o_color;
void main() {
#if FANCY
    o_color = undeclared_thing;
#else
    o_color = vec4(1.0);
#endif
}
)",
                                        "5d000000-0000-4000-8000-000000000011");
    CHECK_FALSE(r.ok());
    const std::string text = diag_text(r);
    INFO(text);
    CHECK(text.find("bad.frag.glsl:6") != std::string::npos);
    CHECK(text.find("undeclared_thing") != std::string::npos);
    CHECK(text.find("FANCY=1") != std::string::npos);
    CHECK(text.find("FANCY=0") == std::string::npos);  // that variant compiles
}

TEST_CASE("shader cook: the binding-bank table is enforced") {
    const std::string root = fresh_dir("shader_cook_banks");
    const cook::CookReport r = cook_one(root, "banks.frag.glsl", R"(#version 450 core
layout(location = 0) out vec4 o_color;
layout(std140, binding = 5) uniform OddU { vec4 u_x; };
layout(binding = 2) uniform sampler2D u_low;
uniform sampler2D u_unbound;
void main() { o_color = u_x + texture(u_low, vec2(0.0)) + texture(u_unbound, vec2(0.0)); }
)",
                                        "5d000000-0000-4000-8000-000000000021");
    CHECK_FALSE(r.ok());
    const std::string text = diag_text(r);
    INFO(text);
    CHECK(text.find("uniform block 'OddU' uses binding 5") != std::string::npos);
    CHECK(text.find("sampler 'u_low' uses binding 2") != std::string::npos);
    CHECK(text.find("'u_unbound' needs an explicit layout(binding") != std::string::npos);

    const std::string root2 = fresh_dir("shader_cook_names");
    const cook::CookReport r2 = cook_one(root2, "noext.glsl", kVert, "5d000000-0000-4000-8000-000000000022");
    CHECK_FALSE(r2.ok());
    CHECK(diag_text(r2).find(".vert.glsl") != std::string::npos);

    const std::string root3 = fresh_dir("shader_cook_variants");
    const cook::CookReport r3 = cook_one(root3, "many.frag.glsl", R"(#version 450 core
// @variant A
// @variant B
// @variant C
// @variant D
// @variant E
// @variant F
// @variant G
// @variant A
layout(location = 0) out vec4 o_color;
void main() { o_color = vec4(1.0); }
)",
                                         "5d000000-0000-4000-8000-000000000023");
    CHECK_FALSE(r3.ok());
    const std::string t3 = diag_text(r3);
    CHECK(t3.find("duplicate") != std::string::npos);
    CHECK(t3.find("at most 6") != std::string::npos);
}
