// GLSL -> MSL translation (mce_rhi_metal's shader_cross, ADR-0028) without a GPU: the fixed binding
// map, the clip-space fixup and the switchable Y flip are visible in the generated source.

#include <doctest/doctest.h>

#include "mce/core.h"
#include "shader_cross.h"

using namespace mce;
using namespace mce::rhi::metal;

namespace {

bool contains(const MslSource& s, const char* needle) { return s.view().find(needle) != StringView::npos; }

constexpr const char* kVS = R"(#version 450 core
#extension GL_ARB_shader_draw_parameters : require
layout(std140, binding = 0) uniform FrameU { mat4 view_proj; };
layout(std140, binding = 2) uniform MaterialU { vec4 tint; };
layout(std430, binding = 3) readonly buffer Objects { mat4 model[]; };
layout(location = 0) in vec3 a_pos;
layout(location = 2) in vec2 a_uv;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_tint;
out gl_PerVertex { vec4 gl_Position; };
void main() {
    v_uv = a_uv;
    v_tint = tint;
    gl_Position = view_proj * model[gl_BaseInstanceARB + gl_InstanceID] * vec4(a_pos, 1.0);
}
)";

constexpr const char* kFS = R"(#version 450 core
layout(binding = 6) uniform sampler2DArray u_atlas;
layout(binding = 9) uniform sampler2DShadow u_shadow;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_tint;
layout(location = 0) out vec4 o_color;
void main() {
    vec4 c = texture(u_atlas, vec3(v_uv, 1.0)) * v_tint;
    if (c.a < 0.01) discard;
    o_color = c * texture(u_shadow, vec3(v_uv, 0.5));
}
)";

}  // namespace

TEST_CASE("shader_cross: vertex stage binding map, base instance, clip fixup, flip constant") {
    FixedString<1024> log;
    auto r = translate_glsl(ShaderStage::Vertex, kVS, "t.vert", log);
    REQUIRE_MESSAGE(r.has_value(), log.c_str());
    if (!r) return;
    const MslSource& msl = r.value();
    CHECK(contains(msl, "[[buffer(0)]]"));   // FrameU at bank 0
    CHECK(contains(msl, "[[buffer(2)]]"));   // MaterialU at bank 2
    CHECK(contains(msl, "[[buffer(11)]]"));  // Objects SSBO binding 3 -> kStorageBufferBase + 3
    CHECK(contains(msl, "[[base_instance]]"));
    // GL's gl_InstanceID excludes the base instance; Metal's [[instance_id]] includes it.
    CHECK(contains(msl, "[[instance_id]]"));
    CHECK(contains(msl, "- int(gl_BaseInstance)"));
    CHECK(contains(msl, "[[attribute(0)]]"));
    CHECK(contains(msl, "[[attribute(2)]]"));
    CHECK(contains(msl, "[[function_constant(0)]]"));
    CHECK(contains(msl, "if (mce_flip_y)"));
    CHECK(contains(msl, ".w) * 0.5"));  // z: [-w, w] -> [0, w]
    CHECK(contains(msl, kEntryPoint));
}

TEST_CASE("shader_cross: fragment stage samplers keep their GL binding numbers") {
    FixedString<1024> log;
    auto r = translate_glsl(ShaderStage::Fragment, kFS, "t.frag", log);
    REQUIRE_MESSAGE(r.has_value(), log.c_str());
    if (!r) return;
    const MslSource& msl = r.value();
    CHECK(contains(msl, "texture2d_array<float> u_atlas [[texture(6)]]"));
    CHECK(contains(msl, "[[sampler(6)]]"));
    CHECK(contains(msl, "u_shadow [[texture(9)]]"));
    CHECK(contains(msl, "sample_compare("));  // sampler2DShadow keeps its depth comparison
    CHECK(contains(msl, "[[sampler(9)]]"));
    CHECK(contains(msl, "discard_fragment()"));
    CHECK_FALSE(contains(msl, "mce_flip_y"));  // fragment stages have no flip
}

TEST_CASE("shader_cross: GLSL errors are Results carrying the compiler log") {
    FixedString<1024> log;
    auto r = translate_glsl(ShaderStage::Vertex, "#version 450 core\nvoid main() { nope }\n", "bad.vert", log);
    REQUIRE_FALSE(r.has_value());
    if (r) return;
    CHECK(r.error().code == Errc::ShaderCompile);
    CHECK(r.error().path == StringView("bad.vert"));
    CHECK(log.c_str()[0] != '\0');
}

TEST_CASE("shader_cross: compute stage keeps the binding map and reports its work-group size") {
    constexpr const char* kCS = R"(#version 450 core
layout(local_size_x = 8, local_size_y = 4) in;
layout(std140, binding = 1) uniform PassU { uvec4 n; };
layout(std430, binding = 10) buffer Counter { uint total; };
layout(std430, binding = 8) readonly buffer In { uint src[]; };
layout(binding = 7) uniform sampler2D u_tex;
layout(rgba8, binding = 2) uniform writeonly image2D u_out;
layout(rgba8, binding = 3) uniform readonly image2D u_in;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    atomicAdd(total, src[p.x] + n.x);
    imageStore(u_out, p, imageLoad(u_in, p) + textureLod(u_tex, vec2(0.5), 0.0));
}
)";
    FixedString<1024> log;
    auto r = translate_glsl(ShaderStage::Compute, kCS, "t.comp", log);
    REQUIRE_MESSAGE(r.has_value(), log.c_str());
    if (!r) return;
    const MslSource& msl = r.value();
    CHECK(msl.local_size[0] == 8);
    CHECK(msl.local_size[1] == 4);
    CHECK(msl.local_size[2] == 1);
    CHECK(contains(msl, "kernel void main0("));
    CHECK(contains(msl, "[[buffer(1)]]"));   // PassU at bank 1
    CHECK(contains(msl, "[[buffer(18)]]"));  // Counter SSBO binding 10 -> kStorageBufferBase + 10
    CHECK(contains(msl, "[[buffer(16)]]"));  // In SSBO binding 8
    CHECK(contains(msl, "[[texture(7)]]"));
    CHECK(contains(msl, "[[sampler(7)]]"));
    CHECK(contains(msl, "access::write> u_out [[texture(18)]]"));  // image unit 2 -> kStorageImageBase + 2
    CHECK(contains(msl, "u_in [[texture(19)]]"));
    CHECK(contains(msl, "atomic_fetch_add_explicit"));
    CHECK_FALSE(contains(msl, "mce_flip_y"));
}
