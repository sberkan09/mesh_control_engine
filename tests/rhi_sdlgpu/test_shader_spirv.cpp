// GLSL -> SPIR-V for SDL GPU (mce_rhi_sdlgpu's shader_spirv, ADR-0038) without a GPU: the binding
// rewrite into SDL's fixed resource sets and the per-stage tables the device binds from.

#include <doctest/doctest.h>

#include <cstdio>
#include <cstring>
#include <utility>

#include "mce/core.h"
#include "shader_spirv.h"
#include "shaders3d.h"  // render's 3D shaders (M-LENSFLARE 38c case)

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

// M-LENSFLARE 38c: the flare's two fragment stages (render's shaders3d.h, the real sources) compile for SDL GPU with
// their resources where Forward3D::record_flare binds them: FlareU at bank kPass (1), the prepass depth at sampler 7
// (occlusion), the source at 6 and the 1x1 occlusion at 8 (composite); no storage buffers.
TEST_CASE("shader_spirv: the lens flare's occlusion and composite stages") {
    FixedString<1024> log;
    Array<u32> spv;
    StageResources res;
    auto occ = compile_glsl(ShaderStage::Fragment, mce::render::shaders3d::kFlareOccFS, "lens_flare_occ.frag", spv, res, log);
    if (!occ) std::fprintf(stderr, "%s\n", log.c_str());
    REQUIRE(occ.has_value());
    if (!occ) return;
    REQUIRE(res.sampler_count == 1);
    CHECK(res.sampler_slot[0] == 7);
    CHECK(res.sampler_kind[0] == SamplerKind::Tex2D);
    CHECK(res.storage_count == 0);
    CHECK(res.uniform_count == 2);

    Array<u32> spv2;
    StageResources res2;
    auto comp = compile_glsl(ShaderStage::Fragment, mce::render::shaders3d::kFlareFS, "lens_flare.frag", spv2, res2, log);
    if (!comp) std::fprintf(stderr, "%s\n", log.c_str());
    REQUIRE(comp.has_value());
    if (!comp) return;
    REQUIRE(res2.sampler_count == 2);
    CHECK(res2.sampler_slot[0] == 6);
    CHECK(res2.sampler_slot[1] == 8);
    CHECK(res2.storage_count == 0);
    CHECK(res2.uniform_count == 2);
}

// M-OUTLINE 39c: the edge outline's fragment stage (render's shaders3d.h, the real source) compiles for SDL GPU with its
// resources where Forward3D::record_outline binds them: OutlineU at bank kPass (1), the HDR frame at sampler 6 and the
// half-resolution prepass at 7; no storage buffers.
TEST_CASE("shader_spirv: the edge outline stage") {
    FixedString<1024> log;
    Array<u32> spv;
    StageResources res;
    auto r = compile_glsl(ShaderStage::Fragment, mce::render::shaders3d::kOutlineFS, "edge_outline.frag", spv, res, log);
    if (!r) std::fprintf(stderr, "%s\n", log.c_str());
    REQUIRE(r.has_value());
    if (!r) return;
    REQUIRE(res.sampler_count == 2);
    CHECK(res.sampler_slot[0] == 6);
    CHECK(res.sampler_slot[1] == 7);
    CHECK(res.sampler_kind[0] == SamplerKind::Tex2D);
    CHECK(res.sampler_kind[1] == SamplerKind::Tex2D);
    CHECK(res.storage_count == 0);
    CHECK(res.uniform_count == 2);
}

// M-LIGHTCOOKIE 40c / 40d: the cookie bake stage and the lit stages (render's shaders3d.h, the real sources) compile for
// SDL GPU. The bake: CookieBakeU at bank kPass (1), the source at sampler 6 (a 2D array), no storage buffers. The lit
// mesh and terrain stages: all 16 sampler banks, the shared bank 2 (decal texture 2 or the cookie atlas) a 2D array, and
// six storage buffers (clusters 4, light pool 5, light indices 6, local shadows 11, decals 15 and the cookie records at
// storage bank 2), under SDL GPU's eight per stage. The lit stages carry the sun's cookie (40e) in PassU and the same
// atlas (texelFetch at sampler 2); the volumetric injection (40f) is checked below.
TEST_CASE("shader_spirv: the light cookie bake stage and the lit stages with the cookie records") {
    FixedString<1024> log;
    {
        Array<u32> spv;
        StageResources res;
        auto r = compile_glsl(ShaderStage::Fragment, mce::render::shaders3d::kCookieBakeFS, "light_cookie_bake.frag", spv, res, log);
        if (!r) std::fprintf(stderr, "%s\n", log.c_str());
        REQUIRE(r.has_value());
        if (!r) return;
        REQUIRE(res.sampler_count == 1);
        CHECK(res.sampler_slot[0] == 6);
        CHECK(res.sampler_kind[0] == SamplerKind::Tex2DArray);
        CHECK(res.storage_count == 0);
        CHECK(res.uniform_count == 2);
    }
    for (const char* src : {mce::render::shaders3d::kMeshFS, mce::render::shaders3d::kTerrainFS}) {
        Array<u32> spv;
        StageResources res;
        auto r = compile_glsl(ShaderStage::Fragment, src, "pbr.frag", spv, res, log);
        if (!r) std::fprintf(stderr, "%s\n", log.c_str());
        REQUIRE(r.has_value());
        if (!r) return;
        CHECK(res.sampler_count == 16);
        bool slot2 = false;
        for (u32 i = 0; i < res.sampler_count; ++i)
            if (res.sampler_slot[i] == 2) slot2 = res.sampler_kind[i] == SamplerKind::Tex2DArray;
        CHECK(slot2);
        CHECK(res.storage_count == 6);
        bool cookies = false;
        for (u32 i = 0; i < res.storage_count; ++i) cookies = cookies || res.storage_binding[i] == 2;
        CHECK(cookies);
    }
    // M-LIGHTCOOKIE 40f: the volumetric injection reads the same cookies (the spot records, the sun's PassU members and
    // the atlas at the bank the pass has free of decals, 2): the clusters' three lists, the local shadows and the
    // records (five storage buffers), the atlas a 2D array at sampler 2 beside the shadow maps at 9.
    {
        Array<u32> spv;
        StageResources res;
        auto r = compile_glsl(ShaderStage::Fragment, mce::render::shaders3d::kVolInjectFS, "vol_inject.frag", spv, res, log);
        if (!r) std::fprintf(stderr, "%s\n", log.c_str());
        REQUIRE(r.has_value());
        if (!r) return;
        bool slot2 = false, shadow = false;
        for (u32 i = 0; i < res.sampler_count; ++i) {
            if (res.sampler_slot[i] == 2) slot2 = res.sampler_kind[i] == SamplerKind::Tex2DArray;
            shadow = shadow || res.sampler_slot[i] == 9;
        }
        CHECK(slot2);
        CHECK(shadow);
        CHECK(res.storage_count == 5);
        bool cookies = false;
        for (u32 i = 0; i < res.storage_count; ++i) cookies = cookies || res.storage_binding[i] == 2;
        CHECK(cookies);
    }
}

// M-TRANSLUCENT 41d / 41e: the lit mesh and terrain stages with the thin-surface terms (the o_skin.w decode, PassU's
// p_translucency, the back shadow, the wrap, the two-sided SH9 and irradiance-volume reads) compile for SDL GPU with the
// resources they had: all 16 sampler banks, the SH9 irradiance (15) and the irradiance volume (5) 2D textures, the same
// six storage buffers and the uniform banks up to ObjectU (3). No new binding.
TEST_CASE("shader_spirv: the lit stages with thin-surface translucency and wrap keep their resources") {
    FixedString<1024> log;
    const char* srcs[2] = {mce::render::shaders3d::kMeshFS, mce::render::shaders3d::kTerrainFS};
    for (u32 k = 0; k < 2; ++k) {
        Array<u32> spv;
        StageResources res;
        auto r = compile_glsl(ShaderStage::Fragment, srcs[k], "pbr.frag", spv, res, log);
        if (!r) std::fprintf(stderr, "%s\n", log.c_str());
        REQUIRE(r.has_value());
        if (!r) return;
        CHECK(res.sampler_count == 16);
        bool sh9 = false, gi = false;
        for (u32 i = 0; i < res.sampler_count; ++i) {
            if (res.sampler_slot[i] == 15) sh9 = res.sampler_kind[i] == SamplerKind::Tex2D;
            if (res.sampler_slot[i] == 5) gi = res.sampler_kind[i] == SamplerKind::Tex2D;
        }
        CHECK(sh9);
        CHECK(gi);
        CHECK(res.storage_count == 6);
        CHECK(res.uniform_count == 4);  // banks up to 3 (ObjectU) declared
    }
}

// M-LENSRAIN 42c: the lens rain's fragment stage (render's shaders3d.h, the real source) compiles for SDL GPU with its
// resources where Forward3D::record_lens_rain binds them: LensRainU at bank kPass (1), the HDR frame at sampler 6; no
// storage buffers (its u32 hash, constant-bound loops and explicit textureLod / texelFetch go through glslang as is).
TEST_CASE("shader_spirv: the lens rain stage") {
    FixedString<1024> log;
    Array<u32> spv;
    StageResources res;
    auto r = compile_glsl(ShaderStage::Fragment, mce::render::shaders3d::kLensRainFS, "lens_rain.frag", spv, res, log);
    if (!r) std::fprintf(stderr, "%s\n", log.c_str());
    REQUIRE(r.has_value());
    if (!r) return;
    REQUIRE(res.sampler_count == 1);
    CHECK(res.sampler_slot[0] == 6);
    CHECK(res.sampler_kind[0] == SamplerKind::Tex2D);
    CHECK(res.storage_count == 0);
    CHECK(res.uniform_count == 2);
}

// M-HEATHAZE 43c: the heat haze's fragment stage (render's shaders3d.h, the real source) compiles for SDL GPU with its
// resources where Forward3D::record_heat_haze binds them: HazeU at bank kPass (1), the HDR frame at sampler 6 and the
// opaque prepass at sampler 7; no storage buffers (its u32 hash, the int -> uint lattice indices, the constant-bound
// loop and explicit textureLod / texelFetch go through glslang as is).
TEST_CASE("shader_spirv: the heat haze stage") {
    FixedString<1024> log;
    Array<u32> spv;
    StageResources res;
    auto r = compile_glsl(ShaderStage::Fragment, mce::render::shaders3d::kHazeFS, "haze.frag", spv, res, log);
    if (!r) std::fprintf(stderr, "%s\n", log.c_str());
    REQUIRE(r.has_value());
    if (!r) return;
    REQUIRE(res.sampler_count == 2);
    CHECK(res.sampler_slot[0] == 6);
    CHECK(res.sampler_slot[1] == 7);
    CHECK(res.sampler_kind[0] == SamplerKind::Tex2D);
    CHECK(res.sampler_kind[1] == SamplerKind::Tex2D);
    CHECK(res.storage_count == 0);
    CHECK(res.uniform_count == 2);
}

// M-UNDERWATER 44c: the underwater fragment stage (render's shaders3d.h, the real source) compiles for SDL GPU with its
// resources where Forward3D::record_underwater binds them: UnderwaterU at bank kPass (1), the HDR frame at sampler 6 and
// the opaque prepass (or ao_off_ at detail 0) at sampler 7; no storage buffers (no loop, explicit textureLod /
// texelFetch, sin and exp go through glslang as is).
TEST_CASE("shader_spirv: the underwater stage") {
    FixedString<1024> log;
    Array<u32> spv;
    StageResources res;
    auto r = compile_glsl(ShaderStage::Fragment, mce::render::shaders3d::kUnderwaterFS, "underwater.frag", spv, res, log);
    if (!r) std::fprintf(stderr, "%s\n", log.c_str());
    REQUIRE(r.has_value());
    if (!r) return;
    REQUIRE(res.sampler_count == 2);
    CHECK(res.sampler_slot[0] == 6);
    CHECK(res.sampler_slot[1] == 7);
    CHECK(res.sampler_kind[0] == SamplerKind::Tex2D);
    CHECK(res.sampler_kind[1] == SamplerKind::Tex2D);
    CHECK(res.storage_count == 0);
    CHECK(res.uniform_count == 2);
}

// M-SUNSHAFTS 45c: the sun shafts' two fragment stages (render's shaders3d.h, the real sources) compile for SDL GPU with
// their resources where Forward3D::record_sun_shafts binds them: ShaftsU at bank kPass (1); the accumulation reads the HDR
// frame at sampler 6 and the opaque prepass at 7, the composite adds the ray buffer at 8; no storage buffers (a constant
// loop bound with a uniform break, explicit textureLod / texelFetch, exp of non-positive numbers only).
TEST_CASE("shader_spirv: the sun shafts' accumulation and composite stages") {
    FixedString<1024> log;
    Array<u32> spv;
    StageResources res;
    auto r = compile_glsl(ShaderStage::Fragment, mce::render::shaders3d::kShaftsFS, "sun_shafts.frag", spv, res, log);
    if (!r) std::fprintf(stderr, "%s\n", log.c_str());
    REQUIRE(r.has_value());
    if (!r) return;
    REQUIRE(res.sampler_count == 2);
    CHECK(res.sampler_slot[0] == 6);
    CHECK(res.sampler_slot[1] == 7);
    CHECK(res.sampler_kind[0] == SamplerKind::Tex2D);
    CHECK(res.sampler_kind[1] == SamplerKind::Tex2D);
    CHECK(res.storage_count == 0);
    CHECK(res.uniform_count == 2);
    Array<u32> spv2;
    StageResources res2;
    auto comp = compile_glsl(ShaderStage::Fragment, mce::render::shaders3d::kShaftsCompFS, "sun_shafts_comp.frag", spv2, res2, log);
    if (!comp) std::fprintf(stderr, "%s\n", log.c_str());
    REQUIRE(comp.has_value());
    if (!comp) return;
    REQUIRE(res2.sampler_count == 3);
    CHECK(res2.sampler_slot[0] == 6);
    CHECK(res2.sampler_slot[1] == 7);
    CHECK(res2.sampler_slot[2] == 8);
    CHECK(res2.storage_count == 0);
    CHECK(res2.uniform_count == 2);
}

// M-CLOUDSHADOW 51c: the cloud shadow fragment stage (render's shaders3d.h, the real source: the sky's cloud noise through
// MCE_GLSL_CLOUD_NOISE and CloudShadowU) compiles for SDL GPU with its resources where Forward3D::record_cloud_shadow binds
// them: CloudShadowU at bank kPass (1), post_source_ at sampler 6 and the opaque prepass at 7; no storage buffers.
TEST_CASE("shader_spirv: the cloud shadow stage") {
    FixedString<1024> log;
    Array<u32> spv;
    StageResources res;
    auto r = compile_glsl(ShaderStage::Fragment, mce::render::shaders3d::kCloudShadowFS, "cloud_shadow.frag", spv, res, log);
    if (!r) std::fprintf(stderr, "%s\n", log.c_str());
    REQUIRE(r.has_value());
    if (!r) return;
    REQUIRE(res.sampler_count == 2);
    CHECK(res.sampler_slot[0] == 6);
    CHECK(res.sampler_slot[1] == 7);
    CHECK(res.sampler_kind[0] == SamplerKind::Tex2D);
    CHECK(res.sampler_kind[1] == SamplerKind::Tex2D);
    CHECK(res.storage_count == 0);
    CHECK(res.uniform_count == 2);
}

// M-CIRRUS 53b: the sky pass and the water, whose fragment stages carry the cirrus layer (apply_cirrus through
// MCE_GLSL_SKY_LAYERS, p_cirrus / p_cirrus2 in PassU), compile for SDL GPU (render's shaders3d.h, the real sources); the
// layer's text alone (kCirrusLayerGLSL, the GL twin test's) is in both. PassU stays at bank kPass (1).
TEST_CASE("shader_spirv: the sky and water stages with the cirrus layer") {
    CHECK(std::strstr(mce::render::shaders3d::kSkyFS, mce::render::shaders3d::kCirrusLayerGLSL) != nullptr);
    CHECK(std::strstr(mce::render::shaders3d::kWaterFS, mce::render::shaders3d::kCirrusLayerGLSL) != nullptr);
    CHECK(std::strstr(mce::render::shaders3d::kSkyFS, "c = apply_cirrus(c, d, sun, cs);") != nullptr);
    CHECK(std::strstr(mce::render::shaders3d::kWaterFS, "c = cirrus_layer(c, r, sun, dot(r, sun), pix);") != nullptr);
    CHECK(std::strlen(mce::render::shaders3d::kSkyFS) < 65535);
    for (const auto& [src, name] : {std::pair{mce::render::shaders3d::kSkyFS, "sky.frag"},
                                    std::pair{mce::render::shaders3d::kWaterFS, "water.frag"}}) {
        FixedString<1024> log;
        Array<u32> spv;
        StageResources res;
        auto r = compile_glsl(ShaderStage::Fragment, src, name, spv, res, log);
        if (!r) std::fprintf(stderr, "%s\n", log.c_str());
        REQUIRE(r.has_value());
        if (!r) return;
        CHECK(res.uniform_count >= 2);  // FrameU (0) and PassU (1)
        CHECK(res.storage_count == 0);
    }
}

// M-HALO 54b: the sky pass and the water, whose cirrus layer now carries the ice halo (halo_term through
// MCE_GLSL_HALO, p_halo the last vec4 of PassU), compile for SDL GPU; the halo's text alone (kHaloGLSL, the GL twin
// test's) is in both, cirrus_layer calls it behind p_halo.x > 0, and the sky stage stays under the 64 KiB budget.
TEST_CASE("shader_spirv: the sky and water stages with the ice halo") {
    using mce::render::shaders3d::kHaloGLSL;
    using mce::render::shaders3d::kSkyFS;
    using mce::render::shaders3d::kWaterFS;
    CHECK(std::strstr(kSkyFS, kHaloGLSL) != nullptr);
    CHECK(std::strstr(kWaterFS, kHaloGLSL) != nullptr);
    CHECK(std::strstr(kSkyFS, "if (p_halo.x > 0.0) c += halo_term(d, sun, cs, a, pix);") != nullptr);
    CHECK(std::strstr(kSkyFS, "vec4 p_halo;") != nullptr);
    CHECK(std::strstr(kSkyFS, "p_free") == nullptr);  // PassU has no free vec4 left
    std::printf("shader_spirv: kSkyFS %zu bytes, kWaterFS %zu bytes\n", std::strlen(kSkyFS), std::strlen(kWaterFS));
    CHECK(std::strlen(kSkyFS) < 65536);
    CHECK(std::strlen(kWaterFS) < 65536);
    for (const auto& [src, name] : {std::pair{kSkyFS, "sky.frag"}, std::pair{kWaterFS, "water.frag"}}) {
        FixedString<1024> log;
        Array<u32> spv;
        StageResources res;
        auto r = compile_glsl(ShaderStage::Fragment, src, name, spv, res, log);
        if (!r) std::fprintf(stderr, "%s\n", log.c_str());
        REQUIRE(r.has_value());
        if (!r) return;
        CHECK(res.uniform_count >= 2);  // FrameU (0) and PassU (1)
        CHECK(res.storage_count == 0);
    }
}

// M-CAUSTICS 52b: the caustics fragment stage (render's shaders3d.h, the real source: MCE_GLSL_CAUSTICS_NOISE and
// CausticsU) compiles for SDL GPU with its resources where Forward3D::record_caustics binds them: CausticsU at bank kPass
// (1), the volumetric fog at sampler 4, post_source_ at 6 and the opaque prepass at 7; no storage buffers.
TEST_CASE("shader_spirv: the caustics stage") {
    FixedString<1024> log;
    Array<u32> spv;
    StageResources res;
    auto r = compile_glsl(ShaderStage::Fragment, mce::render::shaders3d::kCausticsFS, "caustics.frag", spv, res, log);
    if (!r) std::fprintf(stderr, "%s\n", log.c_str());
    REQUIRE(r.has_value());
    if (!r) return;
    REQUIRE(res.sampler_count == 3);
    CHECK(res.sampler_slot[0] == 4);
    CHECK(res.sampler_slot[1] == 6);
    CHECK(res.sampler_slot[2] == 7);
    for (u32 i = 0; i < 3; ++i) CHECK(res.sampler_kind[i] == SamplerKind::Tex2D);
    CHECK(res.storage_count == 0);
    CHECK(res.uniform_count == 2);
}
