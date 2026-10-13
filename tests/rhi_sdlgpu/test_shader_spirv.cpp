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

namespace {

// Every SDL compute binding class: samplers and read-only buffers (set 0), storage images (readonly
// ones too) and read-write buffers (set 1), uniforms (set 2).
constexpr const char* kCS = R"(#version 450 core
layout(local_size_x = 8, local_size_y = 4, local_size_z = 2) in;
layout(std140, binding = 1) uniform PassU { uvec4 grid; };
layout(binding = 7) uniform sampler2D u_tex;
layout(rgba8, binding = 3) uniform readonly image2D u_in;
layout(rgba8, binding = 0) uniform writeonly image2D u_out;
layout(std430, binding = 9) readonly buffer Lights { vec4 lights[]; };
layout(std430, binding = 8) readonly buffer Bounds { float bounds[]; };
layout(std430, binding = 4) writeonly buffer Ranges { uvec2 ranges[]; };
layout(std430, binding = 10) buffer Counter { uint total; uint dropped; };
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    vec4 c = imageLoad(u_in, p) + texture(u_tex, vec2(p)) + lights[0] * bounds[0];
    imageStore(u_out, p, c);
    ranges[p.x] = uvec2(atomicAdd(total, grid.x), dropped);
}
)";

// (set, binding) of every resource in the module, in decoration order.
struct SetBinding {
    u32 set = ~0u, binding = ~0u;
};
void set_bindings(const Array<u32>& w, Array<SetBinding>& out, Array<u32>& ids) {
    for (usize i = 5; i < w.size();) {
        const u32 count = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (op == 71 && count >= 4 && (w[i + 2] == 33 || w[i + 2] == 34)) {
            usize k = 0;
            while (k < ids.size() && ids[k] != w[i + 1]) ++k;
            if (k == ids.size()) {
                ids.push_back(w[i + 1]);
                out.push_back({});
            }
            (w[i + 2] == 34 ? out[k].set : out[k].binding) = w[i + 3];
        }
        i += count ? count : 1;
    }
}

}  // namespace

TEST_CASE("shader_spirv: compute resources follow SDL's compute layout") {
    FixedString<1024> log;
    Array<u32> spv;
    StageResources res;
    auto r = compile_glsl(ShaderStage::Compute, kCS, "t.comp", spv, res, log);
    if (!r) std::fprintf(stderr, "%s\n", log.c_str());
    REQUIRE(r.has_value());
    if (!r) return;
    CHECK(res.local_size[0] == 8);
    CHECK(res.local_size[1] == 4);
    CHECK(res.local_size[2] == 2);
    REQUIRE(res.sampler_count == 1);
    CHECK(res.sampler_slot[0] == 7);
    REQUIRE(res.rw_image_count == 2);
    CHECK(res.rw_image_unit[0] == 0);
    CHECK(res.rw_image_unit[1] == 3);
    REQUIRE(res.storage_count == 2);  // read-only, GL-binding order
    CHECK(res.storage_binding[0] == 8);
    CHECK(res.storage_binding[1] == 9);
    REQUIRE(res.rw_storage_count == 2);  // writeonly counts as read-write
    CHECK(res.rw_storage_binding[0] == 4);
    CHECK(res.rw_storage_binding[1] == 10);
    CHECK(res.uniform_count == 2);

    Array<SetBinding> sb;
    Array<u32> ids;
    set_bindings(spv, sb, ids);
    REQUIRE(sb.size() == 8);
    // Expected (set, binding) pairs, as a sorted multiset.
    u32 found[3][8] = {};
    for (const SetBinding& b : sb)
        if (b.set < 3 && b.binding < 8) ++found[b.set][b.binding];
    CHECK(found[0][0] == 1);  // sampler
    CHECK(found[0][1] == 1);  // Bounds (binding 8)
    CHECK(found[0][2] == 1);  // Lights (binding 9)
    CHECK(found[1][0] == 1);  // u_out (unit 0)
    CHECK(found[1][1] == 1);  // u_in (unit 3, readonly)
    CHECK(found[1][2] == 1);  // Ranges (binding 4)
    CHECK(found[1][3] == 1);  // Counter (binding 10)
    CHECK(found[2][1] == 1);  // uniform bank 1
}

TEST_CASE("shader_spirv: storage images outside compute are compile errors") {
    FixedString<1024> log;
    Array<u32> spv;
    StageResources res;
    auto r = compile_glsl(ShaderStage::Fragment,
                          "#version 450 core\nlayout(rgba8, binding = 0) uniform readonly image2D u_in;\n"
                          "layout(location = 0) out vec4 o;\nvoid main() { o = imageLoad(u_in, ivec2(0)); }\n",
                          "image.frag", spv, res, log);
    REQUIRE_FALSE(r.has_value());
    if (r) return;
    CHECK(r.error().code == Errc::ShaderCompile);
}
