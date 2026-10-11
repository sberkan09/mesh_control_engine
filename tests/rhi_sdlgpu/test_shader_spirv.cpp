// GLSL -> SPIR-V for SDL GPU (mce_rhi_sdlgpu's shader_spirv, ADR-0038) without a GPU: the binding
// rewrite into SDL's fixed resource sets and the per-stage tables the device binds from.

#include <doctest/doctest.h>

#include <cstdio>

#include "mce/core.h"
#include "shader_spirv.h"

using namespace mce;
using namespace mce::rhi::sdlgpu;

namespace {

constexpr const char* kVS = R"(#version 450 core
#extension GL_ARB_shader_draw_parameters : require
layout(std140, binding = 0) uniform FrameU { mat4 view_proj; };
layout(std140, binding = 2) uniform MaterialU { vec4 tint; };
layout(std430, binding = 3) readonly buffer Objects { mat4 model[]; };
layout(std430, binding = 7) readonly buffer Joints { mat4 joints[]; };
layout(location = 0) in vec3 a_pos;
layout(location = 2) in vec2 a_uv;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_tint;
out gl_PerVertex { vec4 gl_Position; };
void main() {
    v_uv = a_uv + vec2(gl_VertexID);
    v_tint = tint;
    gl_Position = view_proj * joints[0] * model[gl_BaseInstanceARB + gl_InstanceID] * vec4(a_pos, 1.0);
}
)";

constexpr const char* kFS = R"(#version 450 core
layout(binding = 9) uniform sampler2DArrayShadow u_shadow;
layout(binding = 6) uniform sampler2DArray u_atlas;
layout(std140, binding = 1) uniform PassU { vec4 fog; };
layout(std430, binding = 5) readonly buffer Lights { vec4 lights[]; };
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_tint;
layout(location = 0) out vec4 o_color;
void main() {
    vec4 c = texture(u_atlas, vec3(v_uv, 1.0)) * v_tint * lights[0] + fog;
    o_color = c * texture(u_shadow, vec4(v_uv, 0.0, 0.5));
}
)";

// DescriptorSet decorations of every resource in the module.
void sets_of(const Array<u32>& w, Array<u32>& sets) {
    for (usize i = 5; i < w.size();) {
        const u32 count = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (op == 71 && count >= 4 && w[i + 2] == 34) sets.push_back(w[i + 3]);
        i += count ? count : 1;
    }
}

}  // namespace

TEST_CASE("shader_spirv: vertex stage resources land in sets 0 and 1") {
    FixedString<1024> log;
    Array<u32> spv;
    StageResources res;
    auto r = compile_glsl(ShaderStage::Vertex, kVS, "t.vert", spv, res, log);
    if (!r) std::fprintf(stderr, "%s\n", log.c_str());
    REQUIRE(r.has_value());
    if (!r) return;
    CHECK(spv.size() > 5);
    CHECK(spv[0] == 0x07230203u);  // SPIR-V magic
    CHECK(res.sampler_count == 0);
    REQUIRE(res.storage_count == 2);
    CHECK(res.storage_binding[0] == 3);  // compacted in GL-binding order
    CHECK(res.storage_binding[1] == 7);
    CHECK(res.uniform_count == 3);       // banks 0 and 2 used: slots 0..2 declared
    Array<u32> sets;
    sets_of(spv, sets);
    CHECK(sets.size() == 4);
    for (const u32 s : sets) CHECK((s == 0 || s == 1));
}

TEST_CASE("shader_spirv: fragment samplers are compacted and typed") {
    FixedString<1024> log;
    Array<u32> spv;
    StageResources res;
    auto r = compile_glsl(ShaderStage::Fragment, kFS, "t.frag", spv, res, log);
    if (!r) std::fprintf(stderr, "%s\n", log.c_str());
    REQUIRE(r.has_value());
    if (!r) return;
    REQUIRE(res.sampler_count == 2);
    CHECK(res.sampler_slot[0] == 6);
    CHECK(res.sampler_kind[0] == SamplerKind::Tex2DArray);
    CHECK(res.sampler_slot[1] == 9);
    CHECK(res.sampler_kind[1] == SamplerKind::Shadow2DArray);
    REQUIRE(res.storage_count == 1);
    CHECK(res.storage_binding[0] == 5);
    CHECK(res.uniform_count == 2);
    Array<u32> sets;
    sets_of(spv, sets);
    CHECK(sets.size() == 4);
    for (const u32 s : sets) CHECK((s == 2 || s == 3));
}

TEST_CASE("shader_spirv: compile errors come back as Results with the log") {
    FixedString<1024> log;
    Array<u32> spv;
    StageResources res;
    auto r = compile_glsl(ShaderStage::Vertex, "#version 450 core\nvoid main() { this is not glsl }\n", "broken.vert",
                          spv, res, log);
    REQUIRE_FALSE(r.has_value());
    if (r) return;
    CHECK(r.error().code == Errc::ShaderCompile);
    CHECK(r.error().path == StringView("broken.vert"));
    CHECK(log.size() > 0);
}
