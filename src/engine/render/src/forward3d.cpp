#include "mce/render/forward3d.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "mce/convar.h"
#include "mce/log.h"
#include "shaders3d.h"

namespace mce::render {

using namespace mce::math;

namespace {

MCE_CVAR(cv_gpu_light_clusters, "r.gpu_light_clusters", true,
         "Assign Forward+ lights in a compute pass when the GPU has compute (off = the CPU assignment)",
         CVarFlags::RequiresRestart);
MCE_CVAR(cv_ibl, "r.ibl", true, "Image-based lighting for scenes that ask for it (Sky::ibl; off = the hemisphere ambient)",
         CVarFlags::RequiresRestart);
MCE_CVAR(cv_ssao, "r.ssao", true,
         "Screen-space ambient occlusion for scenes that ask for it (AmbientOcclusion::enabled; off = no occlusion pass)",
         CVarFlags::RequiresRestart);
MCE_CVAR(cv_taa, "r.taa", true,
         "Temporal anti-aliasing for scenes that ask for it (PostFx::taa; off = FXAA only)", CVarFlags::RequiresRestart);
MCE_CVAR(cv_gi, "r.gi", true,
         "Irradiance-volume global illumination for scenes that ask for it (GiVolume::enabled; off = the sky's ambient)",
         CVarFlags::RequiresRestart);
MCE_CVAR(cv_volumetrics, "r.volumetrics", true,
         "Volumetric fog for scenes that ask for it (Volumetrics::enabled; off = the exp² fog)", CVarFlags::RequiresRestart);
MCE_CVAR(cv_post_fx, "r.post_fx", true,
         "Auto exposure, depth of field and motion blur for scenes that ask for them (PostFx; off = none of them)",
         CVarFlags::RequiresRestart);
MCE_CVAR(cv_local_shadows, "r.local_shadows", true,
         "Shadow maps for spot and point lights that ask for them (PointLight::cast_shadows; off = unshadowed)",
         CVarFlags::None);

}  // namespace

// std140 mirror of the lit passes' PassU (shaders3d.h MCE_GLSL_PASS_U): vec4/mat4 members only.
struct Forward3DPassU {
    vec4 light_dir;  // w: cascade count
    vec4 light_color;
    vec4 amb_sky;
    vec4 amb_ground;
    vec4 fog;
    mat4 cascade[kMaxCascades];
    vec4 cascade_far;
    vec4 cascade_texel;
    vec4 cascade_blend;
    vec4 sky_zenith;   // w: sky on
    vec4 sky_horizon;  // w: specular ambient
    vec4 sky_ground;   // w: sky texture max lod, -1 = gradient
    vec4 sun;          // x: cos disk, y: disk intensity, z: texture intensity, w: texture additive
    vec4 ibl;          // x: on, y: diffuse scale, z: pre-filtered layers, w: bake roughness
    vec4 probes;       // x: reflection probe count
    vec4 probe_pos[kMaxReflectionProbes];  // w: intensity
    vec4 probe_min[kMaxReflectionProbes];  // w: blend distance
    vec4 probe_max[kMaxReflectionProbes];  // w: first ibl_env layer
    vec4 ao;           // x: SSAO on
    vec4 local;        // x: local-light shadows on (M-VISUAL 4), y: shadow-map layer of cascade 0
    vec4 gi;           // x: irradiance volume on (M-VISUAL 6), y: intensity, z: normal bias, w: fade distance
    vec4 gi_min;
    vec4 gi_max;
    vec4 gi_counts;
    vec4 vol;         // volumetric fog (M-VISUAL 7): on, slices, max distance, log2(1 + max distance / kVolD0)
    vec4 vol_grid;    // columns, rows, atlas tiles across, down
    vec4 vol_medium;  // density, height falloff, base height, anisotropy
    vec4 vol_albedo;  // rgb, w: sun scale
    vec4 vol_scale;   // x: local light scale, y: ambient scale
    vec4 decals;      // x: decals to apply (M-VISUAL 9)
};

namespace {

using PassU = Forward3DPassU;

// std140 mirrors of the shader blocks (shaders3d.h): vec4/mat4 members only, so no padding rules.
struct FrameU {
    mat4 view_proj;
    mat4 view;
    mat4 inv_view_proj;
    vec4 camera_pos;
    vec4 params;    // x: exposure, y: encode sRGB, z: bloom intensity, w: bloom on
    vec4 viewport;  // w, h, 1/w, 1/h
    vec4 cluster;   // grid x, y, z, light count
    vec4 cluster_z; // slice scale, bias, near, far
};

struct LocalShadowRecord {  // std430 row of bank kLocalShadows (kMeshFS LocalShadow)
    mat4 view_proj;
    vec4 rect;  // xy: uv origin of the tile, zw: uv size
    vec4 info;  // x: shadow-map layer, y: world texel per meter of distance
};
static_assert(sizeof(LocalShadowRecord) == 96);

struct AoU {  // kAoFS / kAoBlurFS
    mat4 proj;
    vec4 params;  // x: radius, y: intensity, z: power, w: bias
    vec4 step;    // xy: blur step in uv
};

struct ProbeU {  // kProbeReprojectFS
    mat4 face_vp[6];
    vec4 pos;
};

struct GiProjectU {  // kGiProjectFS
    vec4 size;    // x: texels per row, y: rows in the batch, z: face size
    vec4 params;  // x: probes per row, y: weight of the new value
};

struct PrefilterU {  // kProbePrefilterFS
    vec4 params;  // y: samples, w: roughness
};

struct ObjectU {
    mat4 model;
    mat4 normal;
    vec4 uv_rect;
    vec4 base_color;
    vec4 emissive;  // w: emissive map
    vec4 params;    // x: alpha cutoff (0 = no mask), y: unlit, z: roughness, w: metallic
    vec4 maps;      // x: normal map, y: ORM map, z: occlusion in ORM.r, w: occlusion-only
    mat4 dequant;   // skinned draws: quantized -> bind space
    vec4 skin;      // x: first palette joint, -1 = rigid; instanced: y first visible slot, z time
    vec4 inst;      // instanced (M-VISUAL 9): wind amplitude, frequency, direction xz
};

struct ShadowU {  // the rigid shadow shader reads only `mvp`
    mat4 mvp;
    mat4 dequant;
    vec4 skin;
};

f32 palette_index(u32 palette) { return palette == ~0u ? -1.0f : f32(palette); }

struct PostU {
    vec4 texel;
    vec4 params;  // x: threshold, y: knee, z: first pass, w: exposure
};

enum Stage : u32 {
    kTsBegin, kTsClusters, kTsScale, kTsSsao, kTsShadow, kTsGi, kTsVol, kTsMain, kTsResolve, kTsTaa, kTsPostFx, kTsBloom, kTsTonemap, kTsFxaa
};

struct ExpU {  // kExposureHistCS / kExposureReduceCS
    vec4 src;     // width, height, texel step
    vec4 range;   // min log2, bins per log2, rate brightening, rate darkening
    vec4 target;  // key, compensation, low share, high share
    vec4 adapt;   // dt, snap, min, max
};

struct ExposureData {  // std430 Exposure (shaders3d.h MCE_GLSL_EXPOSURE)
    u32 bins[Forward3D::kExposureBins];
    f32 exposure;
    f32 average;
    u32 frames;
    u32 pad;
};

struct DofU {
    vec4 texel;
    vec4 focus;  // distance, in-focus half range, blur range, max radius px
};

struct MbU {
    mat4 reproject;
    vec4 texel;
    vec4 params;  // intensity, max length, taps
};

struct VelPassU {  // kVelocityVS
    mat4 jittered;
    mat4 current;
    mat4 previous;
};
struct VelObjU {
    mat4 current;
    mat4 previous;
    mat4 dequant;
    vec4 skin;  // x: this frame's first joint, y: last frame's (-1 = rigid)
};
struct TaaU {  // kTaaFS
    mat4 reproject;
    vec4 in;
    vec4 out;
    vec4 jitter;  // xy: uv jitter, z: history valid, w: blend
};

// Halton(base) radical inverse of i (i >= 1): the low-discrepancy jitter sequence.
f32 halton(u32 i, u32 base) {
    f32 f = 1.0f, r = 0.0f;
    for (; i > 0; i /= base) {
        f /= f32(base);
        r += f * f32(i % base);
    }
    return r;
}

// The sky's PassU members; returns the texture to bind at bank::kSky. A non-null `atmosphere` (the
// atmosphere's baked sky, M-VISUAL 7) stands in for the gradient and the sky texture.
rhi::TextureHandle sky_params(const TextureStore& textures, rhi::RenderDevice& device, const Sky& sky, PassU& pu,
                              rhi::TextureHandle atmosphere = {}) {
    rhi::TextureHandle sky_tex = textures.white();
    f32 sky_lod = -1.0f;
    if (!atmosphere.is_null()) {
        sky_tex = atmosphere;
        sky_lod = f32(max<u32>(device.texture_desc(sky_tex).mip_levels, 1u) - 1u);
    } else if (sky.texture != ~0u && textures.contains(sky.texture)) {
        sky_tex = textures.get(sky.texture);
        sky_lod = f32(max<u32>(device.texture_desc(sky_tex).mip_levels, 1u) - 1u);
    }
    pu.sky_zenith = vec4(sky.zenith, sky.enabled ? 1.0f : 0.0f);
    pu.sky_horizon = vec4(sky.horizon, sky.specular_ambient);
    pu.sky_ground = vec4(sky.ground, sky_lod);
    if (atmosphere.is_null())
        pu.sun = {sky.sun_size, sky.sun_intensity, sky.texture_intensity, sky.texture_additive ? 1.0f : 0.0f};
    else
        pu.sun = {sky.sun_size, sky.sun_intensity, 1.0f, 0.0f};
    return sky_tex;
}

// IEEE half from a finite float, round to nearest (the atmosphere's RGBA16F upload).
u16 to_half(f32 f) {
    u32 x;
    std::memcpy(&x, &f, 4);
    const u32 sign = (x >> 16) & 0x8000u;
    const i32 e = i32((x >> 23) & 0xFFu) - 127 + 15;
    u32 m = x & 0x7FFFFFu;
    if (e >= 31) return u16(sign | 0x7BFFu);  // clamp to the largest finite half
    if (e <= 0) {
        if (e < -10) return u16(sign);
        m |= 0x800000u;
        const u32 shift = u32(14 - e);
        return u16(sign | ((m + (1u << (shift - 1))) >> shift));
    }
    const u32 h = sign | (u32(e) << 10) | (m >> 13);
    return u16(h + ((m >> 12) & 1u));  // rounding may carry into the exponent: still correct
}

// What the IBL bake reads from the sky (the radiance function sky_radiance() evaluates).
u64 sky_radiance_key(const Sky& sky, rhi::TextureHandle sky_tex, f32 sky_lod) {
    u64 h = 1469598103934665603ull;
    auto mix = [&h](const void* p, usize n) {
        const auto* b = static_cast<const u8*>(p);
        for (usize i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    const f32 f[11] = {sky.zenith.x,  sky.zenith.y,  sky.zenith.z,  sky.horizon.x,         sky.horizon.y, sky.horizon.z,
                       sky.ground.x,  sky.ground.y,  sky.ground.z,  sky.texture_intensity, sky_lod};
    const u32 u[3] = {sky.texture_additive ? 1u : 0u, sky_tex.index, sky_tex.generation};
    mix(f, sizeof(f));
    mix(u, sizeof(u));
    return h | 1u;  // never 0 (= not baked)
}

// What a probe's capture depends on: its placement, its version and the sky lighting it.
u64 probe_key(const ReflectionProbe& p, u64 sky_key) {
    u64 h = sky_key;
    auto mix = [&h](const void* q, usize n) {
        const auto* b = static_cast<const u8*>(q);
        for (usize i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    const f32 f[9] = {p.position.x, p.position.y, p.position.z, p.bounds.min.x, p.bounds.min.y,
                      p.bounds.min.z, p.bounds.max.x, p.bounds.max.y, p.bounds.max.z};
    mix(f, sizeof(f));
    mix(&p.version, sizeof(p.version));
    return h | 1u;
}

// The six 90° views of a capture (+x, -x, +y, -y, +z, -z); any set works, the reprojection
// reads the faces through the same matrices.
void probe_face_views(vec3 pos, f32 near_z, f32 far_z, mat4 (&view)[6], mat4 (&view_proj)[6]) {
    static constexpr vec3 kDirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static constexpr vec3 kUps[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
    const mat4 proj = perspective_rh_no(1.57079633f, 1.0f, near_z, far_z);
    for (u32 f = 0; f < 6; ++f) {
        view[f] = look_at_rh(pos, pos + kDirs[f], kUps[f]);
        view_proj[f] = proj * view[f];
    }
}

// The volume's contents depend on its placement, its version and its bake (or the lack of one).
u64 gi_key(const GiVolume& gi, rhi::TextureHandle baked) {
    u64 h = 1469598103934665603ull;
    auto mix = [&h](const void* q, usize n) {
        const auto* b = static_cast<const u8*>(q);
        for (usize i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    const f32 f[6] = {gi.bounds.min.x, gi.bounds.min.y, gi.bounds.min.z, gi.bounds.max.x, gi.bounds.max.y, gi.bounds.max.z};
    const u32 u[6] = {gi.counts[0], gi.counts[1], gi.counts[2], gi.version, baked.index, baked.generation};
    mix(f, sizeof(f));
    mix(u, sizeof(u));
    return h | 1u;
}

// A sphere is outside the planes (render::frustum_planes) when below any.
bool sphere_in(const vec4 (&planes)[6], vec3 c, f32 r) {
    for (const vec4& p : planes)
        if (dot(p.xyz(), c) + p.w < -r) return false;
    return true;
}

void destroy(rhi::RenderDevice& d, rhi::TextureHandle& t) {
    if (!t.is_null()) d.destroy_texture(t);
    t = {};
}

}  // namespace

Forward3D::~Forward3D() { shutdown(); }

ResultV Forward3D::init(rhi::RenderDevice& device, const TextureStore& textures, const Forward3DDesc& desc) {
    MCE_ASSERT_MSG(!device_, "Forward3D::init twice");
    device_ = &device;
    textures_ = &textures;
    desc_ = desc;
    if (desc_.msaa < 1) desc_.msaa = 1;
    if (desc_.timestamp_base + kStageCount >= rhi::kMaxTimestamps) desc_.timestamps = false;
    if (auto r = create_pipelines(); !r) {
        shutdown();
        return r;
    }
    rhi::SamplerDesc sd;
    sd.min_filter = sd.mag_filter = rhi::Filter::Linear;
    sd.mip = rhi::MipFilter::Linear;
    sd.wrap_s = sd.wrap_t = rhi::AddressMode::Repeat;
    sd.max_aniso = 4.0f;
    material_sampler_ = device.create_sampler(sd);
    rhi::SamplerDesc ss;
    ss.min_filter = ss.mag_filter = rhi::Filter::Linear;
    ss.compare = true;
    ss.compare_op = rhi::CompareOp::LessEqual;
    shadow_sampler_ = device.create_sampler(ss);
    rhi::SamplerDesc ps;
    ps.min_filter = ps.mag_filter = rhi::Filter::Nearest;
    point_sampler_ = device.create_sampler(ps);
    rhi::SamplerDesc lc;
    lc.min_filter = lc.mag_filter = rhi::Filter::Linear;
    linear_clamp_ = device.create_sampler(lc);
    rhi::SamplerDesc es = lc;
    es.wrap_s = rhi::AddressMode::Repeat;  // around the equirect seam; clamped at the poles
    env_sampler_ = device.create_sampler(es);

    rhi::TextureDesc td;
    td.size = {desc_.shadow_size, desc_.shadow_size};
    td.format = rhi::TextureFormat::D32_Float;
    td.usage = rhi::TextureUsage::DepthTarget;
    td.dimension = rhi::TextureDimension::Tex2DArray;
    td.array_layers = static_cast<u16>(kMaxCascades + desc_.local_shadow_layers);
    td.debug_name = "mce.3d.shadow_cascades";
    shadow_map_ = device.create_texture(td);
    rhi::TextureDesc od;
    od.size = {1, 1};
    od.usage = rhi::TextureUsage::SampledColorTarget;
    od.debug_name = "mce.3d.ssao_off";
    ao_off_ = device.create_texture(od);
    return ok();
}

ResultV Forward3D::create_pipelines() {
    using namespace shaders3d;
    static constexpr rhi::VertexAttribute kAttrs[] = {
        {0, rhi::VertexFormat::SNorm16x4, offsetof(asset::MeshVertex, position)},
        {1, rhi::VertexFormat::SNorm16x2, offsetof(asset::MeshVertex, normal)},
        {2, rhi::VertexFormat::UNorm16x2, offsetof(asset::MeshVertex, uv)},
    };
    const rhi::VertexLayout layout{kAttrs, sizeof(asset::MeshVertex)};
    static constexpr rhi::VertexAttribute kSkinnedAttrs[] = {
        {0, rhi::VertexFormat::SNorm16x4, offsetof(SkinnedMeshVertex, base) + offsetof(asset::MeshVertex, position)},
        {1, rhi::VertexFormat::SNorm16x2, offsetof(SkinnedMeshVertex, base) + offsetof(asset::MeshVertex, normal)},
        {2, rhi::VertexFormat::UNorm16x2, offsetof(SkinnedMeshVertex, base) + offsetof(asset::MeshVertex, uv)},
        {3, rhi::VertexFormat::UNorm8x4, offsetof(SkinnedMeshVertex, skin) + offsetof(asset::MeshSkinVertex, joints)},
        {4, rhi::VertexFormat::UNorm8x4, offsetof(SkinnedMeshVertex, skin) + offsetof(asset::MeshSkinVertex, weights)},
    };
    const rhi::VertexLayout skinned_layout{kSkinnedAttrs, sizeof(SkinnedMeshVertex)};

    rhi::GraphicsPipelineDesc pd;
    pd.vertex_shader = {kMeshVS, "pbr.vert"};
    pd.fragment_shader = {kMeshFS, "pbr.frag"};
    pd.vertex = layout;
    pd.state.depth = {true, true, rhi::CompareOp::LessEqual};
    pd.state.raster.cull = rhi::CullMode::Back;
    pd.debug_name = "pbr.opaque";
    MCE_TRY_ASSIGN(mesh_cull_, device_->create_pipeline(pd));
    pd.state.raster.cull = rhi::CullMode::None;
    pd.debug_name = "pbr.two_sided";
    MCE_TRY_ASSIGN(mesh_two_sided_, device_->create_pipeline(pd));
    pd.state.depth.write = false;
    pd.state.blend.mode = rhi::BlendMode::Alpha;
    pd.debug_name = "pbr.blend";
    MCE_TRY_ASSIGN(mesh_blend_, device_->create_pipeline(pd));

    // Skinned twins (FR-R3D-060): the same states over the SkinnedMeshVertex stream.
    rhi::GraphicsPipelineDesc kd;
    kd.vertex_shader = {kSkinnedMeshVS, "pbr_skinned.vert"};
    kd.fragment_shader = {kMeshFS, "pbr.frag"};
    kd.vertex = skinned_layout;
    kd.state.depth = {true, true, rhi::CompareOp::LessEqual};
    kd.state.raster.cull = rhi::CullMode::Back;
    kd.debug_name = "pbr.skinned.opaque";
    MCE_TRY_ASSIGN(skinned_cull_, device_->create_pipeline(kd));
    kd.state.raster.cull = rhi::CullMode::None;
    kd.debug_name = "pbr.skinned.two_sided";
    MCE_TRY_ASSIGN(skinned_two_sided_, device_->create_pipeline(kd));
    kd.state.depth.write = false;
    kd.state.blend.mode = rhi::BlendMode::Alpha;
    kd.debug_name = "pbr.skinned.blend";
    MCE_TRY_ASSIGN(skinned_blend_, device_->create_pipeline(kd));

    rhi::GraphicsPipelineDesc sp;
    sp.vertex_shader = {kShadowVS, "shadow.vert"};
    sp.fragment_shader = {kShadowFS, "shadow.frag"};
    sp.vertex = layout;
    sp.state.depth = {true, true, rhi::CompareOp::LessEqual};
    sp.state.raster.cull = rhi::CullMode::None;  // kit meshes are often single-sided shells
    sp.debug_name = "mesh.shadow";
    MCE_TRY_ASSIGN(shadow_pipe_, device_->create_pipeline(sp));
    sp.vertex_shader = {kSkinnedShadowVS, "shadow_skinned.vert"};
    sp.vertex = skinned_layout;
    sp.debug_name = "mesh.shadow.skinned";
    MCE_TRY_ASSIGN(skinned_shadow_pipe_, device_->create_pipeline(sp));

    rhi::GraphicsPipelineDesc sky;
    sky.vertex_shader = {kSkyVS, "sky.vert"};
    sky.fragment_shader = {kSkyFS, "sky.frag"};
    sky.state.depth = {true, false, rhi::CompareOp::LessEqual};
    sky.debug_name = "sky";
    MCE_TRY_ASSIGN(sky_, device_->create_pipeline(sky));

    rhi::GraphicsPipelineDesc fs;
    fs.vertex_shader = {kFullscreenVS, "fullscreen.vert"};
    fs.state.depth = {};
    fs.fragment_shader = {kBloomDownFS, "bloom_down.frag"};
    fs.debug_name = "bloom.down";
    MCE_TRY_ASSIGN(bloom_down_, device_->create_pipeline(fs));
    fs.fragment_shader = {kBloomUpFS, "bloom_up.frag"};
    fs.state.blend.mode = rhi::BlendMode::Additive;
    fs.debug_name = "bloom.up";
    MCE_TRY_ASSIGN(bloom_up_, device_->create_pipeline(fs));
    fs.state.blend.mode = rhi::BlendMode::Opaque;
    fs.fragment_shader = {kTonemapFS, "tonemap.frag"};
    fs.debug_name = "tonemap";
    MCE_TRY_ASSIGN(tonemap_, device_->create_pipeline(fs));
    fs.fragment_shader = {kFxaaFS, "fxaa.frag"};
    fs.debug_name = "fxaa";
    MCE_TRY_ASSIGN(fxaa_, device_->create_pipeline(fs));
    if (desc_.gpu_light_clusters && cv_gpu_light_clusters.get_bool() && device_->caps().compute) {
        rhi::ComputePipelineDesc cd;
        cd.compute_shader = {kClusterBuildCS, "mce.3d.cluster_build.comp"};
        cd.debug_name = "mce.3d.cluster_build";
        // A driver that refuses the shader keeps the CPU assignment rather than failing init.
        if (auto p = device_->create_compute_pipeline(cd)) cluster_build_ = p.value();
        else MCE_LOG_WARN("render3d: GPU light assignment unavailable, using the CPU path");
    }
    if (desc_.ssao && cv_ssao.get_bool()) {
        rhi::GraphicsPipelineDesc ap = pd;  // the mesh vertex stage, depth-tested, both faces
        ap.fragment_shader = {kAoPrepassFS, "ssao_prepass.frag"};
        ap.state = {};
        ap.state.depth = {true, true, rhi::CompareOp::LessEqual};
        ap.state.raster.cull = rhi::CullMode::None;
        ap.debug_name = "ssao.prepass";
        MCE_TRY_ASSIGN(ao_prepass_, device_->create_pipeline(ap));
        ap.vertex_shader = kd.vertex_shader;
        ap.vertex = skinned_layout;
        ap.debug_name = "ssao.prepass.skinned";
        MCE_TRY_ASSIGN(ao_prepass_skinned_, device_->create_pipeline(ap));
        fs.state.blend.mode = rhi::BlendMode::Opaque;
        fs.fragment_shader = {kAoFS, "ssao.frag"};
        fs.debug_name = "ssao";
        MCE_TRY_ASSIGN(ao_pipe_, device_->create_pipeline(fs));
        fs.fragment_shader = {kAoBlurFS, "ssao_blur.frag"};
        fs.debug_name = "ssao.blur";
        MCE_TRY_ASSIGN(ao_blur_, device_->create_pipeline(fs));
    }
    const bool taa = desc_.taa && cv_taa.get_bool(), post_fx = desc_.post_fx && cv_post_fx.get_bool();
    if (taa || post_fx) {  // the velocity prepass: TAA, motion blur and depth of field
        rhi::GraphicsPipelineDesc vp = sp;  // the shadow vertex streams, depth-tested, both faces
        vp.vertex_shader = {kVelocityVS, "velocity.vert"};
        vp.fragment_shader = {kVelocityFS, "velocity.frag"};
        vp.vertex = layout;
        vp.debug_name = "taa.velocity";
        MCE_TRY_ASSIGN(velocity_pipe_, device_->create_pipeline(vp));
        vp.vertex_shader = {kSkinnedVelocityVS, "velocity_skinned.vert"};
        vp.vertex = skinned_layout;
        vp.debug_name = "taa.velocity.skinned";
        MCE_TRY_ASSIGN(velocity_skinned_, device_->create_pipeline(vp));
    }
    MCE_TRY(create_scale_pipelines());
    if (taa) {
        fs.state.blend.mode = rhi::BlendMode::Opaque;
        fs.fragment_shader = {kTaaFS, "taa.frag"};
        fs.debug_name = "taa";
        MCE_TRY_ASSIGN(taa_pipe_, device_->create_pipeline(fs));
    }
    // The exposure buffer is bound by the bloom and tonemap passes whether or not auto exposure runs.
    {
        ExposureData init{};
        init.exposure = 1.0f;
        exposure_buf_ = device_->create_buffer({rhi::BufferUsage::Storage, sizeof(ExposureData), "mce.3d.exposure"},
                                               std::as_bytes(Span<const ExposureData>(&init, 1)));
        exposure_reset_ = true;
    }
    if (post_fx) {
        fs.state.blend.mode = rhi::BlendMode::Opaque;
        fs.fragment_shader = {kDofFS, "dof.frag"};
        fs.debug_name = "dof";
        MCE_TRY_ASSIGN(dof_pipe_, device_->create_pipeline(fs));
        fs.fragment_shader = {kMotionBlurFS, "motion_blur.frag"};
        fs.debug_name = "motion_blur";
        MCE_TRY_ASSIGN(motion_blur_pipe_, device_->create_pipeline(fs));
        if (device_->caps().compute) {
            rhi::ComputePipelineDesc cd;
            cd.compute_shader = {kExposureHistCS, "mce.3d.exposure_hist.comp"};
            cd.debug_name = "mce.3d.exposure_hist";
            if (auto p = device_->create_compute_pipeline(cd)) exposure_hist_ = p.value();
            cd.compute_shader = {kExposureReduceCS, "mce.3d.exposure_reduce.comp"};
            cd.debug_name = "mce.3d.exposure_reduce";
            if (auto p = device_->create_compute_pipeline(cd)) exposure_reduce_ = p.value();
            if (exposure_hist_.is_null() || exposure_reduce_.is_null())
                MCE_LOG_WARN("render3d: auto exposure unavailable (its compute shaders failed)");
        }
    }
    if (desc_.gi && cv_gi.get_bool()) {
        fs.fragment_shader = {kGiProjectFS, "gi_project.frag"};
        fs.state.blend.mode = rhi::BlendMode::Alpha;  // alpha = the new value's weight (hysteresis)
        fs.debug_name = "gi.project";
        MCE_TRY_ASSIGN(gi_project_, device_->create_pipeline(fs));
        fs.state.blend.mode = rhi::BlendMode::Opaque;
        fs.fragment_shader = {kGiEncodeFS, "gi_encode.frag"};
        fs.debug_name = "gi.encode";
        MCE_TRY_ASSIGN(gi_encode_, device_->create_pipeline(fs));
        fs.fragment_shader = {kProbeDecodeFS, "probe_decode.frag"};
        fs.debug_name = "gi.decode";
        MCE_TRY_ASSIGN(gi_decode_, device_->create_pipeline(fs));
    }
    if (desc_.volumetrics && cv_volumetrics.get_bool()) {
        fs.fragment_shader = {kVolInjectFS, "vol_inject.frag"};
        fs.debug_name = "vol.inject";
        MCE_TRY_ASSIGN(vol_inject_, device_->create_pipeline(fs));
        fs.fragment_shader = {kVolIntegrateFS, "vol_integrate.frag"};
        fs.debug_name = "vol.integrate";
        MCE_TRY_ASSIGN(vol_integrate_, device_->create_pipeline(fs));
    }
    if (desc_.ibl && cv_ibl.get_bool()) {
        fs.fragment_shader = {kIblPrefilterFS, "ibl_prefilter.frag"};
        fs.debug_name = "ibl.prefilter";
        MCE_TRY_ASSIGN(ibl_prefilter_, device_->create_pipeline(fs));
        fs.fragment_shader = {kIblShFS, "ibl_sh.frag"};
        fs.debug_name = "ibl.sh9";
        MCE_TRY_ASSIGN(ibl_sh_pipe_, device_->create_pipeline(fs));
        fs.fragment_shader = {kIblBrdfFS, "ibl_brdf.frag"};
        fs.debug_name = "ibl.brdf_lut";
        MCE_TRY_ASSIGN(ibl_brdf_, device_->create_pipeline(fs));
        fs.fragment_shader = {kProbeReprojectFS, "probe_reproject.frag"};
        fs.debug_name = "probe.reproject";
        MCE_TRY_ASSIGN(probe_reproject_, device_->create_pipeline(fs));
        fs.fragment_shader = {kProbePrefilterFS, "probe_prefilter.frag"};
        fs.debug_name = "probe.prefilter";
        MCE_TRY_ASSIGN(probe_prefilter_, device_->create_pipeline(fs));
        fs.fragment_shader = {kProbeEncodeFS, "probe_encode.frag"};
        fs.debug_name = "probe.encode";
        MCE_TRY_ASSIGN(probe_encode_, device_->create_pipeline(fs));
        fs.fragment_shader = {kProbeDecodeFS, "probe_decode.frag"};
        fs.debug_name = "probe.decode";
        MCE_TRY_ASSIGN(probe_decode_, device_->create_pipeline(fs));
    }
    return ok();
}

void Forward3D::destroy_targets() {
    if (!device_) return;
    for (rhi::TextureHandle* t : {&hdr_msaa_, &depth_, &hdr_, &ldr_, &ao_gbuf_, &ao_depth_, &ao_a_, &ao_b_, &velocity_,
                                  &velocity_depth_, &history_[0], &history_[1]})
        destroy(*device_, *t);
    history_size_ = {};
    history_valid_ = false;
    for (rhi::TextureHandle& t : bloom_) destroy(*device_, t);
    bloom_levels_ = 0;
    size_ = {};
}

void Forward3D::ensure_targets(rhi::Extent2D size) {
    if (size == size_ && !hdr_.is_null()) return;
    destroy_targets();
    size_ = size;
    rhi::TextureDesc d;
    d.size = size;
    d.format = rhi::TextureFormat::R11G11B10F;
    d.usage = rhi::TextureUsage::SampledColorTarget;
    d.debug_name = "mce.3d.hdr";
    hdr_ = device_->create_texture(d);
    if (desc_.msaa > 1) {
        rhi::TextureDesc m = d;
        m.samples = static_cast<u8>(desc_.msaa);
        m.usage = rhi::TextureUsage::ColorTarget;
        m.debug_name = "mce.3d.hdr_msaa";
        hdr_msaa_ = device_->create_texture(m);
    }
    rhi::TextureDesc z;
    z.size = size;
    z.format = rhi::TextureFormat::D32_Float;
    z.usage = rhi::TextureUsage::DepthTarget;
    z.samples = static_cast<u8>(desc_.msaa);
    z.debug_name = "mce.3d.depth";
    depth_ = device_->create_texture(z);
    rhi::TextureDesc l;
    l.size = size;
    l.format = rhi::TextureFormat::RGBA8_UNorm;
    l.usage = rhi::TextureUsage::SampledColorTarget;
    l.debug_name = "mce.3d.ldr";
    ldr_ = device_->create_texture(l);

    // Bloom chain: half resolution down to ~16 px on the short side.
    static constexpr const char* kBloomNames[kMaxBloomLevels] = {"mce.3d.bloom0", "mce.3d.bloom1", "mce.3d.bloom2",
                                                                 "mce.3d.bloom3", "mce.3d.bloom4", "mce.3d.bloom5"};
    rhi::Extent2D bs{size.width, size.height};
    for (u32 i = 0; i < kMaxBloomLevels; ++i) {
        bs = {max(1u, bs.width / 2), max(1u, bs.height / 2)};
        if (i > 0 && min(bs.width, bs.height) < 8) break;
        rhi::TextureDesc b = d;
        b.size = bs;
        b.debug_name = kBloomNames[i];
        bloom_[i] = device_->create_texture(b);
        bloom_size_[i] = bs;
        bloom_levels_ = i + 1;
    }
}

void Forward3D::shutdown() {
    if (!device_) return;
    destroy_targets();
    for (rhi::PipelineHandle* p :
         {&shadow_pipe_, &skinned_shadow_pipe_, &mesh_cull_, &mesh_two_sided_, &mesh_blend_, &skinned_cull_,
          &skinned_two_sided_, &skinned_blend_, &sky_, &bloom_down_, &bloom_up_, &tonemap_, &fxaa_, &cluster_build_,
          &ibl_prefilter_, &ibl_sh_pipe_, &ibl_brdf_, &probe_reproject_, &probe_prefilter_, &probe_encode_,
          &probe_decode_, &ao_prepass_, &ao_prepass_skinned_, &ao_pipe_, &ao_blur_, &velocity_pipe_, &velocity_skinned_,
          &taa_pipe_, &gi_project_, &gi_encode_, &gi_decode_, &vol_inject_, &vol_integrate_, &exposure_hist_,
          &exposure_reduce_, &dof_pipe_, &motion_blur_pipe_}) {
        if (!p->is_null()) device_->destroy_pipeline(*p);
        *p = {};
    }
    for (rhi::TextureHandle* t : {&ibl_env_, &ibl_lut_, &ibl_sh_, &ibl_off_env_, &ibl_off_2d_, &probe_faces_, &probe_depth_, &probe_capture_, &probe_export_, &ao_off_,
                                  &gi_volume_, &gi_faces_, &gi_faces_depth_, &gi_export_, &atmo_tex_, &vol_media_, &vol_light_,
                                  &fx_[0], &fx_[1]})
        destroy(*device_, *t);
    atmo_key_ = 0;
    gi_size_ = {};
    gi_face_layers_ = gi_shadow_layer_ = gi_cursor_ = gi_filled_ = 0;
    gi_key_ = 0;
    ibl_key_ = 0;
    ibl_env_layers_ = 0;
    for (u64& k : probe_keys_) k = 0;
    for (rhi::SamplerHandle* s : {&material_sampler_, &shadow_sampler_, &point_sampler_, &linear_clamp_, &env_sampler_}) {
        if (!s->is_null()) device_->destroy_sampler(*s);
        *s = {};
    }
    destroy(*device_, shadow_map_);
    fx_size_ = {};
    for (rhi::BufferHandle* b : {&cluster_ranges_, &cluster_indices_, &exposure_buf_}) {
        if (!b->is_null()) device_->destroy_buffer(*b);
        *b = {};
    }
    cluster_ranges_cap_ = cluster_indices_cap_ = 0;
    destroy_scale();
    device_ = nullptr;
    textures_ = nullptr;
}

void Forward3D::timestamp(rhi::CommandBuffer& cmd, u32 stage) {
    if (desc_.timestamps) cmd.write_timestamp(desc_.timestamp_base + stage);
}

Forward3DGpuTimes Forward3D::gpu_times(const rhi::GpuTimestamps& ts) const {
    Forward3DGpuTimes t;
    if (!desc_.timestamps) return t;
    const u32 b = desc_.timestamp_base;
    t.frame_index = ts.frame_index;
    t.clusters = ts.ms(b + kTsBegin, b + kTsClusters);
    t.scale = ts.ms(b + kTsClusters, b + kTsScale);
    t.ssao = ts.ms(b + kTsScale, b + kTsSsao);
    t.shadow = ts.ms(b + kTsSsao, b + kTsShadow);
    t.gi = ts.ms(b + kTsShadow, b + kTsGi);
    t.volumetrics = ts.ms(b + kTsGi, b + kTsVol);
    t.main = ts.ms(b + kTsVol, b + kTsMain);
    t.resolve = ts.ms(b + kTsMain, b + kTsResolve);
    t.taa = ts.ms(b + kTsResolve, b + kTsTaa);
    t.post_fx = ts.ms(b + kTsTaa, b + kTsPostFx);
    t.bloom = ts.ms(b + kTsPostFx, b + kTsBloom);
    t.tonemap = ts.ms(b + kTsBloom, b + kTsTonemap);
    t.fxaa = ts.ms(b + kTsTonemap, b + kTsFxaa);
    t.total = ts.ms(b + kTsBegin, b + kTsFxaa);
    return t;
}

void Forward3D::record(rhi::CommandBuffer& cmd, const Scene3D& scene, rhi::TextureHandle target) {
    MCE_ASSERT_MSG(device_, "Forward3D::record before init");
    MCE_ASSERT_MSG(scene.built(), "Forward3D::record: scene not built");
    MCE_ASSERT_MSG(!cmd.in_pass(), "Forward3D::record inside a pass");
    stats_ = {};
    const rhi::Extent2D view = scene.view().viewport;
    if (view.width == 0 || view.height == 0) return;
    // Render scale (quality.h): the 3D targets shrink; the tonemap/FXAA pass writes the full target.
    const f32 scale = clamp(desc_.render_scale, 0.5f, 1.0f);
    const rhi::Extent2D size = scale >= 1.0f ? view
                                              : rhi::Extent2D{max(1u, static_cast<u32>(std::lround(f32(view.width) * scale))),
                                                              max(1u, static_cast<u32>(std::lround(f32(view.height) * scale)))};
    ensure_targets(size);
    // TAA: a sub-pixel shift of this frame's projection, Halton(2, 3) over 8 frames.
    taa_on_ = scene.post().taa && !taa_pipe_.is_null();
    jitter_ = mat4::identity();
    jitter_uv_ = {0.0f, 0.0f};
    if (taa_on_) {
        const u32 i = taa_frame_++ % 8 + 1;
        const f32 jx = (halton(i, 2) - 0.5f) * 2.0f / f32(size.width), jy = (halton(i, 3) - 0.5f) * 2.0f / f32(size.height);
        jitter_.c[3][0] = jx;  // clip.xy += j * clip.w: NDC moves by j
        jitter_.c[3][1] = jy;
        jitter_uv_ = {jx * 0.5f, jy * 0.5f};
    } else {
        history_valid_ = false;  // a frame without TAA breaks the history
    }
    post_source_ = hdr_;
    post_size_ = size;
    // The joint palette stays bound for the shadow and main passes (one copy per frame).
    const Span<const mat4> palette = scene.joint_palette();
    if (!palette.empty()) cmd.set_storage_data(rhi::bank::kJointPalette, std::as_bytes(palette));
    stats_.joints = static_cast<u32>(palette.size());
    update_atmosphere(scene);   // CPU: re-bakes the sky texture when the sun or the air changed
    record_ibl(cmd, scene);     // rare (sky changes), so outside the per-stage budget
    record_probes(cmd, scene);  // rare too (probe placed, moved or version bumped)
    // The volume's sun fit takes one more shadow-map layer, added once a scene has a volume.
    if (scene.gi_volume().enabled && !gi_project_.is_null() && gi_shadow_layer_ == 0) {
        destroy(*device_, shadow_map_);
        rhi::TextureDesc td;
        td.size = {desc_.shadow_size, desc_.shadow_size};
        td.format = rhi::TextureFormat::D32_Float;
        td.usage = rhi::TextureUsage::DepthTarget;
        td.dimension = rhi::TextureDimension::Tex2DArray;
        gi_shadow_layer_ = kMaxCascades + desc_.local_shadow_layers;
        td.array_layers = static_cast<u16>(gi_shadow_layer_ + 1);
        td.debug_name = "mce.3d.shadow_cascades";
        shadow_map_ = device_->create_texture(td);
    }
    timestamp(cmd, kTsBegin);
    record_clusters(cmd, scene);
    timestamp(cmd, kTsClusters);
    record_scale(cmd, scene);
    timestamp(cmd, kTsScale);
    record_ssao(cmd, scene);
    timestamp(cmd, kTsSsao);
    record_shadows(cmd, scene);
    record_local_shadows(cmd, scene);
    timestamp(cmd, kTsShadow);
    record_gi(cmd, scene);
    timestamp(cmd, kTsGi);
    record_volumetrics(cmd, scene);
    timestamp(cmd, kTsVol);
    record_main(cmd, scene);
    timestamp(cmd, kTsMain);
    if (desc_.msaa > 1) cmd.resolve(hdr_msaa_, hdr_);
    timestamp(cmd, kTsResolve);
    velocity_on_ = scene.post().needs_velocity() && !velocity_pipe_.is_null();
    if (velocity_on_) record_velocity(cmd, scene);
    else velocity_prev_valid_ = false;  // a frame without the prepass breaks the previous transforms
    if (taa_on_) record_taa(cmd, scene);
    timestamp(cmd, kTsTaa);
    record_post_fx(cmd, scene);
    timestamp(cmd, kTsPostFx);
    record_post(cmd, scene, target);
}

// ---- 0. Forward+ light assignment on the GPU (M-VISUAL 1): fills banks 4 and 6 for the main pass
void Forward3D::record_clusters(rhi::CommandBuffer& cmd, const Scene3D& scene) {
    if (cluster_build_.is_null()) return;
    const ClusterLights& cl = scene.cluster_setup();
    const u32 clusters = cl.cluster_count();
    if (clusters > cluster_ranges_cap_) {
        if (!cluster_ranges_.is_null()) device_->destroy_buffer(cluster_ranges_);
        cluster_ranges_ = device_->create_buffer({rhi::BufferUsage::Storage, usize(clusters) * sizeof(ClusterRange), "mce.3d.cluster_ranges"});
        cluster_ranges_cap_ = clusters;
    }
    const u32 capacity = max(cl.grid().max_indices, 1u);
    if (capacity > cluster_indices_cap_) {
        if (!cluster_indices_.is_null()) device_->destroy_buffer(cluster_indices_);
        cluster_indices_ = device_->create_buffer({rhi::BufferUsage::Storage, usize(capacity) * sizeof(u32), "mce.3d.cluster_indices"});
        cluster_indices_cap_ = capacity;
    }
    struct ClusterBuildU {
        u32 grid[4];
        u32 layout[4];
    } u{{cl.grid().x, cl.grid().y, cl.grid().z, static_cast<u32>(cl.culled().size())},
        {capacity, cl.tile_bounds_y_offset(), cl.edges_offset(), 0}};
    static constexpr ClusterCullLight kNoLight{};
    static constexpr u32 kZero[2] = {0, 0};
    cmd.push_debug_scope("clusters");
    cmd.bind_pipeline(cluster_build_);
    cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(u));
    cmd.set_storage_data(rhi::bank::kClusterCull, cl.culled().empty() ? rhi::as_bytes(kNoLight) : std::as_bytes(cl.culled()));
    cmd.set_storage_data(rhi::bank::kClusterBounds, std::as_bytes(cl.tile_bounds()));
    cmd.set_storage_data(rhi::bank::kClusterCounter, rhi::as_bytes(kZero));
    cmd.bind_storage_buffer(rhi::bank::kClusters, cluster_ranges_, 0, 0);
    cmd.bind_storage_buffer(rhi::bank::kLightIndices, cluster_indices_, 0, 0);
    cmd.dispatch((clusters + 63) / 64);
    cmd.barrier(rhi::barrier::kStorageBuffer);
    cmd.pop_debug_scope();
    stats_.gpu_clusters = true;
}

// ---- image-based lighting (M-VISUAL 2): bakes the sky's IBL textures when the sky changed
void Forward3D::update_atmosphere(const Scene3D& scene) {
    const Atmosphere& a = scene.atmosphere();
    atmo_on_ = a.enabled;
    if (!a.enabled) return;
    stats_.atmosphere = true;
    // What the bake depends on: the air, the (quantized) sun and its irradiance above the air.
    const vec3 sun = scene.atmosphere_sun(), e = scene.sun_irradiance();
    const f32 key_f[16] = {a.intensity, a.rayleigh, a.mie, a.mie_anisotropy, a.ozone, a.ground_albedo.x, a.ground_albedo.y,
                           a.ground_albedo.z, a.altitude, sun.x, sun.y, sun.z, e.x, e.y, e.z, 0.0f};
    u64 key = 1469598103934665603ull;
    const auto* b = reinterpret_cast<const u8*>(key_f);
    for (usize i = 0; i < sizeof(key_f); ++i) key = (key ^ b[i]) * 1099511628211ull;
    key |= 1u;
    if (key == atmo_key_ && !atmo_tex_.is_null()) return;
    constexpr u32 kW = 256, kH = 128, kMips = 8;  // 256×128 down to 2×1
    Array<f32> img(mem::heap());
    img.resize(usize(kW) * kH * 4);
    bake_atmosphere_sky(a, sun, kW, kH, img.span());
    for (usize i = 0; i < img.size(); i += 4) {  // radiance per unit irradiance -> radiance
        img[i] *= e.x;
        img[i + 1] *= e.y;
        img[i + 2] *= e.z;
    }
    // A new texture per bake: the IBL key follows the handle, so the sky's lighting re-bakes too.
    destroy(*device_, atmo_tex_);
    rhi::TextureDesc d;
    d.format = rhi::TextureFormat::RGBA16F;
    d.usage = rhi::TextureUsage::Sampled;
    d.size = {kW, kH};
    d.mip_levels = kMips;
    d.dimension = rhi::TextureDimension::Tex2DArray;
    d.array_layers = 1;
    d.debug_name = "mce.3d.atmosphere_sky";
    atmo_tex_ = device_->create_texture(d);
    Array<u16> half(mem::heap());
    u32 w = kW, h = kH;
    for (u32 mip = 0; mip < kMips; ++mip) {
        if (mip > 0) {  // 2×2 box down from the level above, in place
            const u32 pw = w;
            w = max(w / 2, 1u);
            h = max(h / 2, 1u);
            for (u32 y = 0; y < h; ++y)
                for (u32 x = 0; x < w; ++x)
                    for (u32 c = 0; c < 4; ++c)
                        img[(usize(y) * w + x) * 4 + c] =
                            0.25f * (img[(usize(2 * y) * pw + 2 * x) * 4 + c] + img[(usize(2 * y) * pw + 2 * x + 1) * 4 + c] +
                                     img[(usize(2 * y + 1) * pw + 2 * x) * 4 + c] + img[(usize(2 * y + 1) * pw + 2 * x + 1) * 4 + c]);
        }
        half.resize(usize(w) * h * 4);
        for (usize i = 0; i < half.size(); ++i) half[i] = to_half(img[i]);
        rhi::TextureRegion r;
        r.mip = mip;
        device_->update_texture(atmo_tex_, r, std::as_bytes(half.span()));
    }
    atmo_key_ = key;
    stats_.atmosphere_baked = true;
}

void Forward3D::record_ibl(rhi::CommandBuffer& cmd, const Scene3D& scene) {
    const Sky& sky = scene.sky();
    if (!sky.ibl || ibl_prefilter_.is_null()) return;
    stats_.ibl = true;
    auto pass_into = [&](rhi::TextureHandle t, u16 layer, const char* name) {
        rhi::PassAttachment out;
        out.texture = t;
        out.layer = layer;
        out.load = rhi::LoadOp::DontCare;
        rhi::RenderPassDesc p;
        p.colors = Span<const rhi::PassAttachment>(&out, 1);
        p.debug_name = name;
        cmd.begin_pass(p);
    };
    cmd.push_debug_scope("ibl");
    if (ibl_lut_.is_null()) {
        rhi::TextureDesc d;
        d.format = rhi::TextureFormat::RGBA16F;
        d.usage = rhi::TextureUsage::SampledColorTarget;
        d.size = {kIblLutSize, kIblLutSize};
        d.debug_name = "mce.3d.ibl_brdf_lut";
        ibl_lut_ = device_->create_texture(d);
        d.size = {9, 1};
        d.debug_name = "mce.3d.ibl_sh9";
        ibl_sh_ = device_->create_texture(d);
        pass_into(ibl_lut_, 0, "ibl.brdf_lut");  // depends on nothing: once
        cmd.bind_pipeline(ibl_brdf_);
        cmd.draw_non_indexed(3);
        cmd.end_pass();
    }
    // The sky's slot, then the probe slots once a scene has probes (grown once, never shrunk).
    const u32 layers = kProbeLayers + (scene.reflection_probes().empty() ? 0u : kMaxReflectionProbes * kProbeLayers);
    if (layers > ibl_env_layers_) {
        destroy(*device_, ibl_env_);
        rhi::TextureDesc d;
        d.format = rhi::TextureFormat::RGBA16F;
        d.usage = rhi::TextureUsage::SampledColorTarget;
        d.size = {kIblEnvWidth, kIblEnvWidth / 2};
        d.dimension = rhi::TextureDimension::Tex2DArray;
        d.array_layers = static_cast<u16>(layers);
        d.debug_name = "mce.3d.ibl_env";
        ibl_env_ = device_->create_texture(d);
        ibl_env_layers_ = layers;
        ibl_key_ = 0;
        for (u64& k : probe_keys_) k = 0;
    }
    // The bake reads sky_radiance(): the visible sky, or Sky::ibl_texture alone in its place.
    Sky source = sky;
    ibl_own_source_ = sky.ibl_texture != ~0u && textures_->contains(sky.ibl_texture);
    if (ibl_own_source_) {
        source.texture = sky.ibl_texture;
        source.texture_intensity = sky.ibl_texture_intensity;
        source.texture_additive = false;
    }
    PassU pu{};
    const rhi::TextureHandle sky_tex = sky_params(*textures_, *device_, source, pu, ibl_own_source_ ? rhi::TextureHandle{} : sky_texture_override());
    const u64 key = sky_radiance_key(source, sky_tex, pu.sky_ground.w);
    if (key != ibl_key_) {
        cmd.bind_texture(rhi::bank::kSky, sky_tex, material_sampler_);
        for (u32 i = 0; i < kProbeLayers; ++i) {  // sharp, then roughness 0.2 ... 1.0
            pass_into(ibl_env_, static_cast<u16>(i), "ibl.prefilter");
            cmd.bind_pipeline(ibl_prefilter_);
            pu.ibl = {0.0f, 0.0f, f32(kIblLayers), f32(i) / f32(kIblLayers)};
            cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(pu));
            cmd.draw_non_indexed(3);
            cmd.end_pass();
        }
        pass_into(ibl_sh_, 0, "ibl.sh9");
        cmd.bind_pipeline(ibl_sh_pipe_);
        cmd.draw_non_indexed(3);
        cmd.end_pass();
        ibl_key_ = key;
        stats_.ibl_baked = true;
    }
    cmd.pop_debug_scope();
}

// ---- reflection probes (M-VISUAL 2): the scene from each changed probe, into its ibl_env layers.
// Captures see the sun and the sky IBL but no shadows and no point lights.
void Forward3D::record_probes(rhi::CommandBuffer& cmd, const Scene3D& scene) {
    const Span<const ReflectionProbe> probes = scene.reflection_probes();
    if (!stats_.ibl || probes.empty() || probe_reproject_.is_null()) return;
    stats_.probes = static_cast<u32>(probes.size());
    u64 keys[kMaxReflectionProbes] = {};
    bool any = false;
    rhi::TextureHandle baked[kMaxReflectionProbes] = {};
    for (u32 i = 0; i < probes.size(); ++i) {
        if (probes[i].baked != ~0u && textures_->contains(probes[i].baked)) {
            const rhi::TextureHandle t = textures_->get(probes[i].baked);
            const rhi::TextureDesc& d = device_->texture_desc(t);
            if (d.size.width == kIblEnvWidth && d.size.height == kIblEnvWidth / 2 && d.array_layers == kProbeLayers)
                baked[i] = t;
            else
                MCE_LOG_WARN("render3d: reflection probe {} bake is {}x{}x{}, not {}x{}x{}: captured instead", i,
                             d.size.width, d.size.height, d.array_layers, kIblEnvWidth, kIblEnvWidth / 2, kProbeLayers);
        }
        // A bake depends on nothing but its texture.
        keys[i] = baked[i].is_null() ? probe_key(probes[i], ibl_key_)
                                     : ((u64(baked[i].index) << 32 | baked[i].generation) * 1099511628211ull) | 1u;
        any |= keys[i] != probe_keys_[i];
    }
    if (!any) return;
    if (probe_faces_.is_null()) {
        rhi::TextureDesc d;
        d.format = rhi::TextureFormat::RGBA16F;
        d.usage = rhi::TextureUsage::SampledColorTarget;
        d.size = {kProbeFaceSize, kProbeFaceSize};
        d.dimension = rhi::TextureDimension::Tex2DArray;
        d.array_layers = 6;
        d.debug_name = "mce.3d.probe_faces";
        probe_faces_ = device_->create_texture(d);
        d.format = rhi::TextureFormat::D32_Float;
        d.usage = rhi::TextureUsage::DepthTarget;
        d.debug_name = "mce.3d.probe_depth";
        probe_depth_ = device_->create_texture(d);
        rhi::TextureDesc c;
        c.format = rhi::TextureFormat::RGBA16F;
        c.usage = rhi::TextureUsage::SampledColorTarget;
        c.size = {kIblEnvWidth, kIblEnvWidth / 2};
        c.debug_name = "mce.3d.probe_capture";
        probe_capture_ = device_->create_texture(c);
    }
    auto pass_into = [&](rhi::TextureHandle t, u16 layer, const char* name) {
        rhi::PassAttachment out;
        out.texture = t;
        out.layer = layer;
        out.load = rhi::LoadOp::DontCare;
        rhi::RenderPassDesc p;
        p.colors = Span<const rhi::PassAttachment>(&out, 1);
        p.debug_name = name;
        cmd.begin_pass(p);
    };

    const Fog& fog = scene.fog();
    const Sky& sky = scene.sky();
    const View3D& view = scene.view();
    const DirectionalLight& light = scene.light();
    PassU pu{};
    vec3 dir = normalize(light.direction);
    if (!(length_sq(dir) > 0.0f)) dir = {0, -1, 0};
    pu.light_dir = vec4(dir, 0.0f);  // no cascades
    pu.light_color = vec4(light.color * light.intensity, 0.0f);
    pu.amb_sky = vec4(light.ambient_sky, 0.0f);
    pu.amb_ground = vec4(light.ambient_ground, 0.0f);
    pu.fog = vec4(fog.color, fog.density);
    for (mat4& c : pu.cascade) c = mat4::identity();
    const rhi::TextureHandle sky_tex = sky_params(*textures_, *device_, sky, pu, sky_texture_override());
    pu.ibl = {1.0f, sky.diffuse_ambient, f32(kIblLayers), ibl_own_source_ ? 1.0f : 0.0f};  // the sky's IBL, no probes
    static constexpr ClusterRange kNoRange{0, 0};
    static constexpr GpuPointLight kNoLight{};
    static constexpr u32 kNoIndex = 0;
    const Span<const Scene3D::Draw> draws = scene.probe_draws();

    cmd.push_debug_scope("probes");
    for (u32 i = 0; i < probes.size(); ++i) {
        if (keys[i] == probe_keys_[i]) continue;
        if (!baked[i].is_null()) {  // the editor's bake: decode its layers into the slot
            cmd.bind_texture(rhi::bank::kFirstSampler, baked[i], point_sampler_);
            for (u32 l = 0; l < kProbeLayers; ++l) {
                pass_into(ibl_env_, static_cast<u16>(probe_base_layer(i) + l), "probe.decode");
                cmd.bind_pipeline(probe_decode_);
                const vec4 du{f32(l), 0.0f, kProbeRgbmRange, 0.0f};
                cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(du));
                cmd.draw_non_indexed(3);
                cmd.end_pass();
            }
            probe_keys_[i] = keys[i];
            ++stats_.probes_loaded;
            continue;
        }
        const ReflectionProbe& probe = probes[i];
        mat4 face_view[6], face_vp[6];
        probe_face_views(probe.position, view.near_z, view.far_z, face_view, face_vp);
        for (u32 f = 0; f < 6; ++f) {
            rhi::PassAttachment color;
            color.texture = probe_faces_;
            color.layer = static_cast<u16>(f);
            color.clear.color[0] = fog.color.x;
            color.clear.color[1] = fog.color.y;
            color.clear.color[2] = fog.color.z;
            color.clear.color[3] = 1.0f;
            rhi::PassAttachment depth;
            depth.texture = probe_depth_;
            depth.layer = static_cast<u16>(f);
            depth.clear.depth = 1.0f;
            depth.store = rhi::StoreOp::DontCare;
            rhi::RenderPassDesc fp;
            fp.colors = Span<const rhi::PassAttachment>(&color, 1);
            fp.depth = depth;
            fp.debug_name = "probe.face";
            cmd.begin_pass(fp);
            FrameU fu{};
            fu.view_proj = face_vp[f];
            fu.view = face_view[f];
            fu.inv_view_proj = inverse(face_vp[f]);
            fu.camera_pos = vec4(probe.position, 0.0f);
            fu.params = {scene.exposure(), 0.0f, 0.0f, 0.0f};
            const f32 px = f32(kProbeFaceSize);
            fu.viewport = {px, px, 1.0f / px, 1.0f / px};
            fu.cluster = {1.0f, 1.0f, 1.0f, 0.0f};
            fu.cluster_z = {0.0f, 0.0f, view.near_z, view.far_z};
            cmd.set_uniforms(rhi::bank::kFrame, rhi::as_bytes(fu));
            cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(pu));
            cmd.bind_texture(rhi::bank::kShadowMap, shadow_map_, shadow_sampler_);
            cmd.bind_texture(rhi::bank::kSky, sky_tex, material_sampler_);
            cmd.bind_texture(rhi::bank::kIblEnv, ibl_env_, env_sampler_);
            cmd.bind_texture(rhi::bank::kIblBrdfLut, ibl_lut_, linear_clamp_);
            cmd.bind_texture(rhi::bank::kIblSh, ibl_sh_, point_sampler_);
            cmd.bind_texture(rhi::bank::kAo, ao_off_, linear_clamp_);  // captures take no SSAO (PassU::ao.x = 0)
            cmd.bind_texture(rhi::bank::kGiVolume, ao_off_, point_sampler_);  // nor the volume (PassU::gi.x = 0)
            cmd.set_storage_data(rhi::bank::kClusters, rhi::as_bytes(kNoRange));
            cmd.set_storage_data(rhi::bank::kLightIndices, rhi::as_bytes(kNoIndex));
            cmd.set_storage_data(rhi::bank::kLightPool, rhi::as_bytes(kNoLight));
            static constexpr LocalShadowRecord kNoShadow{};
            cmd.set_storage_data(rhi::bank::kLocalShadows, rhi::as_bytes(kNoShadow));
            bind_decals(cmd, scene, false);
            DrawCache cache;
            draw_meshes(cmd, scene, draws, 0, static_cast<u32>(draws.size()), static_cast<u32>(draws.size()), cache, false);
            stats_.probe_draws += static_cast<u32>(draws.size());
            if (sky.enabled) {
                cmd.bind_pipeline(sky_);
                cmd.draw_non_indexed(3);
            }
            cmd.end_pass();
        }
        // Faces -> the sharp equirect, then the GGX layers into this probe's ibl_env slot.
        ProbeU qu;
        for (u32 f = 0; f < 6; ++f) qu.face_vp[f] = face_vp[f];
        qu.pos = vec4(probe.position, 0.0f);
        pass_into(probe_capture_, 0, "probe.reproject");
        cmd.bind_pipeline(probe_reproject_);
        cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(qu));
        cmd.bind_texture(rhi::bank::kFirstSampler, probe_faces_, linear_clamp_);
        cmd.draw_non_indexed(3);
        cmd.end_pass();
        const u32 base = probe_base_layer(i);
        cmd.bind_texture(rhi::bank::kFirstSampler, probe_capture_, env_sampler_);
        for (u32 l = 0; l < kProbeLayers; ++l) {
            pass_into(ibl_env_, static_cast<u16>(base + l), "probe.prefilter");
            cmd.bind_pipeline(probe_prefilter_);
            const PrefilterU fu{{0.0f, l == 0 ? 1.0f : 256.0f, 0.0f, f32(l) / f32(kIblLayers)}};
            cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(fu));
            cmd.draw_non_indexed(3);
            cmd.end_pass();
        }
        probe_keys_[i] = keys[i];
        ++stats_.probes_captured;
    }
    cmd.pop_debug_scope();
}

bool Forward3D::record_probe_export(rhi::CommandBuffer& cmd, u32 probe) {
    MCE_ASSERT_MSG(!cmd.in_pass(), "Forward3D::record_probe_export inside a pass");
    if (probe >= kMaxReflectionProbes || probe_keys_[probe] == 0 || probe_encode_.is_null()) return false;
    if (probe_export_.is_null()) {
        rhi::TextureDesc d;
        d.format = rhi::TextureFormat::RGBA8_UNorm;
        d.usage = rhi::TextureUsage::SampledColorTarget;
        d.size = probe_export_size();
        d.debug_name = "mce.3d.probe_export";
        probe_export_ = device_->create_texture(d);
    }
    rhi::PassAttachment out;
    out.texture = probe_export_;
    out.load = rhi::LoadOp::DontCare;
    rhi::RenderPassDesc p;
    p.colors = Span<const rhi::PassAttachment>(&out, 1);
    p.debug_name = "probe.export";
    cmd.begin_pass(p);
    cmd.bind_pipeline(probe_encode_);
    const vec4 eu{f32(probe_base_layer(probe)), f32(kProbeLayers), kProbeRgbmRange, 0.0f};
    cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(eu));
    cmd.bind_texture(rhi::bank::kFirstSampler, ibl_env_, point_sampler_);
    cmd.draw_non_indexed(3);
    cmd.end_pass();
    return true;
}

// ---- 0. screen-space ambient occlusion (M-VISUAL 3): prepass -> occlusion -> blur H -> blur V, half size
void Forward3D::record_ssao(rhi::CommandBuffer& cmd, const Scene3D& scene) {
    const AmbientOcclusion& ao = scene.ambient_occlusion();
    if (!ao.enabled || ao_pipe_.is_null()) return;
    const rhi::Extent2D half{max(1u, size_.width / 2), max(1u, size_.height / 2)};
    if (ao_gbuf_.is_null()) {
        rhi::TextureDesc d;
        d.size = half;
        d.format = rhi::TextureFormat::RGBA16F;
        d.usage = rhi::TextureUsage::SampledColorTarget;
        d.debug_name = "mce.3d.ssao_gbuf";
        ao_gbuf_ = device_->create_texture(d);
        d.format = rhi::TextureFormat::RGBA8_UNorm;
        d.debug_name = "mce.3d.ssao_a";
        ao_a_ = device_->create_texture(d);
        d.debug_name = "mce.3d.ssao_b";
        ao_b_ = device_->create_texture(d);
        rhi::TextureDesc z;
        z.size = half;
        z.format = rhi::TextureFormat::D32_Float;
        z.usage = rhi::TextureUsage::DepthTarget;
        z.debug_name = "mce.3d.ssao_depth";
        ao_depth_ = device_->create_texture(z);
    }
    cmd.push_debug_scope("ssao");
    const View3D& view = scene.view();
    rhi::PassAttachment color;
    color.texture = ao_gbuf_;
    color.clear.color[3] = 0.0f;  // view depth 0: nothing drawn
    rhi::PassAttachment depth;
    depth.texture = ao_depth_;
    depth.clear.depth = 1.0f;
    depth.store = rhi::StoreOp::DontCare;
    rhi::RenderPassDesc pp;
    pp.colors = Span<const rhi::PassAttachment>(&color, 1);
    pp.depth = depth;
    pp.debug_name = "ssao.prepass";
    cmd.begin_pass(pp);
    FrameU fu{};
    fu.view_proj = jitter_ * view.view_proj;
    fu.view = view.view;
    fu.inv_view_proj = inverse(fu.view_proj);
    fu.camera_pos = vec4(view.position, 0.0f);
    fu.viewport = {f32(half.width), f32(half.height), 1.0f / f32(half.width), 1.0f / f32(half.height)};
    cmd.set_uniforms(rhi::bank::kFrame, rhi::as_bytes(fu));
    DrawCache cache;
    const Span<const Scene3D::Draw> draws = scene.draws();
    draw_meshes(cmd, scene, draws, 0, scene.opaque_count(), scene.opaque_count(), cache, false, true);
    stats_.ssao_draws = scene.opaque_count();
    draw_batches(cmd, scene, cache, true);
    cmd.end_pass();

    auto fullscreen = [&](rhi::TextureHandle dst, const char* name) {
        rhi::PassAttachment out;
        out.texture = dst;
        out.load = rhi::LoadOp::DontCare;
        rhi::RenderPassDesc p;
        p.colors = Span<const rhi::PassAttachment>(&out, 1);
        p.debug_name = name;
        cmd.begin_pass(p);
    };
    AoU au{view.proj, {max(ao.radius, 1e-3f), max(ao.intensity, 0.0f), max(ao.power, 1e-3f), clamp(ao.bias, 0.0f, 0.99f)}, {}};
    fullscreen(ao_a_, "ssao.occlusion");
    cmd.bind_pipeline(ao_pipe_);
    cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(au));
    cmd.bind_texture(rhi::bank::kFirstSampler, ao_gbuf_, point_sampler_);
    cmd.draw_non_indexed(3);
    cmd.end_pass();
    for (u32 axis = 0; axis < 2; ++axis) {  // horizontal into b, vertical back into a
        fullscreen(axis == 0 ? ao_b_ : ao_a_, "ssao.blur");
        cmd.bind_pipeline(ao_blur_);
        au.step = axis == 0 ? vec4(1.0f / f32(half.width), 0.0f, 0.0f, 0.0f) : vec4(0.0f, 1.0f / f32(half.height), 0.0f, 0.0f);
        cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(au));
        cmd.bind_texture(rhi::bank::kFirstSampler, axis == 0 ? ao_a_ : ao_b_, point_sampler_);
        cmd.bind_texture(rhi::bank::kFirstSampler + 1, ao_gbuf_, point_sampler_);
        cmd.draw_non_indexed(3);
        cmd.end_pass();
    }
    cmd.pop_debug_scope();
    stats_.ssao = true;
}

// ---- 1. cascades: one depth-only pass per layer, each with its own caster list
void Forward3D::record_shadows(rhi::CommandBuffer& cmd, const Scene3D& scene) {
    const Span<const Cascade> cascades = scene.cascades();
    if (cascades.empty()) return;
    cmd.push_debug_scope("shadow");
    const Span<const Scene3D::Draw> casters = scene.shadow_draws();
    const Span<const u32> lists = scene.cascade_casters();
    for (u32 ci = 0; ci < cascades.size(); ++ci) {
        const Cascade& c = cascades[ci];
        rhi::RenderPassDesc sp;
        rhi::PassAttachment depth;
        depth.texture = shadow_map_;
        depth.layer = static_cast<u16>(ci);
        depth.clear.depth = 1.0f;
        sp.depth = depth;
        sp.debug_name = "shadow.cascade";
        cmd.begin_pass(sp);
        rhi::PipelineHandle bound_pipe;
        const GpuMesh* bound = nullptr;
        for (u32 k = 0; k < c.caster_count; ++k) shadow_draw(cmd, c.view_proj, casters[lists[c.first_caster + k]], bound_pipe, bound);
        draw_batches_depth(cmd, scene, 1 + ci, c.view_proj, false);
        cmd.end_pass();
    }
    stats_.cascades = static_cast<u32>(cascades.size());
    cmd.pop_debug_scope();
}

void Forward3D::shadow_draw(rhi::CommandBuffer& cmd, const mat4& view_proj, const Scene3D::Draw& d, rhi::PipelineHandle& bound_pipe,
                            const GpuMesh*& bound) {
    if (d.mesh->vertices.is_null()) return;
    const rhi::PipelineHandle pipe = d.mesh->skinned() ? skinned_shadow_pipe_ : shadow_pipe_;
    if (!(pipe == bound_pipe)) {
        cmd.bind_pipeline(pipe);
        bound_pipe = pipe;
    }
    if (d.mesh != bound) {
        cmd.bind_vertex_buffer(d.mesh->vertices);
        cmd.bind_index_buffer(d.mesh->indices, d.mesh->index_format);
        bound = d.mesh;
    }
    const ShadowU su{view_proj * d.model, d.dequant, {palette_index(d.palette), 0.0f, 0.0f, 0.0f}};
    cmd.set_uniforms(rhi::bank::kObject, rhi::as_bytes(su));
    cmd.draw(d.index_count, 1, d.first_index, 0, static_cast<i32>(d.base_vertex));
    ++stats_.shadow_draws;
}

// ---- 1b. local-light shadows (M-VISUAL 4): one depth pass per atlas tile, its viewport the tile.
// The first pass on a layer clears the whole layer; the rest load it.
void Forward3D::record_local_shadows(rhi::CommandBuffer& cmd, const Scene3D& scene) {
    local_shadows_ = false;
    const Span<const LocalShadowTile> tiles = scene.local_shadow_tiles();
    if (tiles.empty() || desc_.local_shadow_layers == 0 || !cv_local_shadows.get_bool()) return;
    const LocalShadowSettings& ls = scene.local_shadow_settings();
    if (ls.layers != desc_.local_shadow_layers || scene.shadow_settings().resolution != desc_.shadow_size) {
        if (!local_mismatch_logged_)
            MCE_LOG_WARN("render3d: local shadows need LocalShadowSettings::layers = {} and ShadowSettings::resolution = {} "
                         "(got {} and {}): drawn unshadowed",
                         desc_.local_shadow_layers, desc_.shadow_size, ls.layers, scene.shadow_settings().resolution);
        local_mismatch_logged_ = true;
        return;
    }
    cmd.push_debug_scope("shadow.local");
    const Span<const Scene3D::Draw> casters = scene.shadow_draws();
    const Span<const u32> lists = scene.local_shadow_casters();
    u32 cleared = 0;  // layers cleared so far (tiles fill the layers in order)
    for (const LocalShadowTile& t : tiles) {
        rhi::RenderPassDesc sp;
        rhi::PassAttachment depth;
        depth.texture = shadow_map_;
        depth.layer = static_cast<u16>(kMaxCascades + t.layer);
        depth.clear.depth = 1.0f;
        depth.load = t.layer < cleared ? rhi::LoadOp::Load : rhi::LoadOp::Clear;
        cleared = max(cleared, t.layer + 1);
        sp.depth = depth;
        sp.viewport = t.rect;
        sp.debug_name = "shadow.local";
        cmd.begin_pass(sp);
        rhi::PipelineHandle bound_pipe;
        const GpuMesh* bound = nullptr;
        for (u32 k = 0; k < t.caster_count; ++k) shadow_draw(cmd, t.view_proj, casters[lists[t.first_caster + k]], bound_pipe, bound);
        cmd.end_pass();
    }
    stats_.local_shadow_tiles = static_cast<u32>(tiles.size());
    local_shadows_ = true;
    cmd.pop_debug_scope();
}

// ---- 2. Forward+ PBR, sky, blended
// The froxel grid of this frame's 3D size (columns × rows of `tile` pixels, `slices` deep) in an
// atlas of a tile per slice, kVolAtlasCols across.
void Forward3D::vol_params(const Scene3D& scene, PassU& pu) const {
    const Volumetrics& v = scene.volumetrics();
    const f32 far = max(v.max_distance, 1.0f);
    pu.vol = {1.0f, f32(vol_slices_), far, std::log2(1.0f + far / 1.0f)};  // kVolD0 = 1 m (shaders3d.h)
    const u32 atlas_rows = (vol_slices_ + kVolAtlasCols - 1) / kVolAtlasCols;
    pu.vol_grid = {f32(vol_grid_.width), f32(vol_grid_.height), f32(kVolAtlasCols), f32(atlas_rows)};
    pu.vol_medium = {max(v.density, 0.0f), max(v.height_falloff, 0.0f), v.base_height, clamp(v.anisotropy, -0.95f, 0.95f)};
    pu.vol_albedo = vec4(v.albedo, max(v.sun_intensity, 0.0f));
    pu.vol_scale = {max(v.local_intensity, 0.0f), max(v.ambient_intensity, 0.0f), 0.0f, 0.0f};
}

// ---- 1c. volumetric fog (M-VISUAL 7): the medium and its light at every froxel (the sun through
// its cascades, the clusters' lights, the ambient), then integrated front to back per column.
void Forward3D::record_volumetrics(rhi::CommandBuffer& cmd, const Scene3D& scene) {
    const Volumetrics& v = scene.volumetrics();
    if (!v.enabled || vol_inject_.is_null()) return;
    const u32 tile = clamp(v.tile, 4u, 64u);
    vol_slices_ = clamp(v.slices, 8u, 128u);
    vol_grid_ = {(size_.width + tile - 1) / tile, (size_.height + tile - 1) / tile};
    const rhi::Extent2D atlas{vol_grid_.width * kVolAtlasCols, vol_grid_.height * ((vol_slices_ + kVolAtlasCols - 1) / kVolAtlasCols)};
    if (vol_media_.is_null() || !(device_->texture_size(vol_media_) == atlas)) {
        destroy(*device_, vol_media_);
        destroy(*device_, vol_light_);
        rhi::TextureDesc d;
        d.format = rhi::TextureFormat::RGBA16F;
        d.usage = rhi::TextureUsage::SampledColorTarget;
        d.size = atlas;
        d.debug_name = "mce.3d.vol_media";
        vol_media_ = device_->create_texture(d);
        d.debug_name = "mce.3d.vol_light";
        vol_light_ = device_->create_texture(d);
    }
    stats_.volumetrics = true;
    stats_.froxels = vol_grid_.width * vol_grid_.height * vol_slices_;
    cmd.push_debug_scope("volumetrics");
    rhi::PassAttachment out;
    out.texture = vol_media_;
    out.load = rhi::LoadOp::DontCare;
    rhi::RenderPassDesc p;
    p.colors = Span<const rhi::PassAttachment>(&out, 1);
    p.debug_name = "vol.inject";
    cmd.begin_pass(p);
    bind_main_state(cmd, scene, false);  // the main pass's view, light, cascades and lists
    cmd.bind_pipeline(vol_inject_);
    cmd.draw_non_indexed(3);
    cmd.end_pass();
    out.texture = vol_light_;
    p.debug_name = "vol.integrate";
    cmd.begin_pass(p);
    FrameU fu{};  // the view's rays (the integration reads nothing else of the main pass's state)
    fu.view_proj = jitter_ * scene.view().view_proj;
    fu.view = scene.view().view;
    fu.inv_view_proj = inverse(fu.view_proj);
    fu.camera_pos = vec4(scene.view().position, 0.0f);
    PassU pu{};
    vol_params(scene, pu);
    cmd.set_uniforms(rhi::bank::kFrame, rhi::as_bytes(fu));
    cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(pu));
    cmd.bind_pipeline(vol_integrate_);
    cmd.bind_texture(rhi::bank::kFirstSampler, vol_media_, point_sampler_);
    cmd.draw_non_indexed(3);
    cmd.end_pass();
    cmd.pop_debug_scope();
}

// The main pass's FrameU, PassU, textures and Forward+ lists (the volumetric fog's injection reads the
// same, `apply_fog` = false while the fog is still being made).
void Forward3D::bind_main_state(rhi::CommandBuffer& cmd, const Scene3D& scene, bool apply_fog) {
    const Fog& fog = scene.fog();
    const View3D& view = scene.view();
    // The GPU path never runs the CPU assignment: the setup (pool + grid) is all the frame reads.
    const ClusterLights& cl = stats_.gpu_clusters ? scene.cluster_setup() : scene.clusters();
    const PostFx& post = scene.post();
    FrameU fu;
    fu.view_proj = jitter_ * view.view_proj;
    fu.view = view.view;
    fu.inv_view_proj = inverse(fu.view_proj);
    fu.camera_pos = vec4(view.position, 0.0f);
    fu.params = {scene.exposure(), desc_.encode_srgb_output ? 1.0f : 0.0f, post.bloom_intensity / f32(max(bloom_levels_, 1u)),
                 post.bloom && bloom_levels_ > 0 ? 1.0f : 0.0f};
    fu.viewport = {f32(size_.width), f32(size_.height), 1.0f / f32(size_.width), 1.0f / f32(size_.height)};
    fu.cluster = {f32(cl.grid().x), f32(cl.grid().y), f32(cl.grid().z), f32(cl.lights().size())};
    fu.cluster_z = {cl.slice_scale(), cl.slice_bias(), view.near_z, view.far_z};
    cmd.set_uniforms(rhi::bank::kFrame, rhi::as_bytes(fu));

    const DirectionalLight& light = scene.light();
    const Span<const Cascade> cascades = scene.cascades();
    PassU pu{};
    vec3 dir = normalize(light.direction);
    if (!(length_sq(dir) > 0.0f)) dir = {0, -1, 0};
    pu.light_dir = vec4(dir, f32(cascades.size()));
    pu.light_color = vec4(light.color * light.intensity, 0.0f);
    pu.amb_sky = vec4(light.ambient_sky, 0.0f);
    pu.amb_ground = vec4(light.ambient_ground, 0.0f);
    pu.fog = vec4(fog.color, fog.density);
    for (u32 i = 0; i < kMaxCascades; ++i) {
        if (i < cascades.size()) {
            const Cascade& c = cascades[i];
            pu.cascade[i] = c.view_proj;
            pu.cascade_far[i] = c.far_depth;
            pu.cascade_texel[i] = 2.0f * c.radius / f32(desc_.shadow_size);  // this map's texels, not the fit's
            pu.cascade_blend[i] = scene.shadow_settings().blend * (c.far_depth - c.near_depth);
        } else {
            pu.cascade[i] = mat4::identity();
        }
    }
    const rhi::TextureHandle sky_tex = bind_ambient(cmd, scene, true, stats_.gi, pu);
    // Volumetric fog: the integrated froxels (record_volumetrics() filled them), or a stand-in.
    if (stats_.volumetrics) {
        vol_params(scene, pu);
        pu.vol.x = apply_fog ? 1.0f : 0.0f;  // the injection reads the grid, not the fog it is making
    }
    cmd.bind_texture(rhi::bank::kVolumetrics, stats_.volumetrics && apply_fog ? vol_light_ : ao_off_, linear_clamp_);
    // SSAO: the blurred half-size visibility, upsampled bilinearly; a stand-in fills the slot when off.
    pu.ao = {stats_.ssao ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
    cmd.bind_texture(rhi::bank::kAo, stats_.ssao ? ao_a_ : ao_off_, linear_clamp_);
    bind_local_shadows(cmd, scene, pu);
    // Decals (M-VISUAL 9): the main pass only; the fog injection takes the stand-ins.
    pu.decals.x = f32(bind_decals(cmd, scene, apply_fog));
    if (apply_fog) stats_.decals = static_cast<u32>(pu.decals.x);
    cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(pu));
    cmd.bind_texture(rhi::bank::kShadowMap, shadow_map_, shadow_sampler_);
    cmd.bind_texture(rhi::bank::kSky, sky_tex, material_sampler_);

    // Forward+ lists (banks 4-6). Never empty: an unbound SSBO is undefined even when unread.
    static constexpr ClusterRange kNoRange{0, 0};
    static constexpr GpuPointLight kNoLight{};
    static constexpr u32 kNoIndex = 0;
    if (stats_.gpu_clusters) {  // record_clusters() filled them
        cmd.bind_storage_buffer(rhi::bank::kClusters, cluster_ranges_, 0, 0);
        cmd.bind_storage_buffer(rhi::bank::kLightIndices, cluster_indices_, 0, 0);
    } else {
        cmd.set_storage_data(rhi::bank::kClusters, cl.ranges().empty() ? rhi::as_bytes(kNoRange) : std::as_bytes(cl.ranges()));
        cmd.set_storage_data(rhi::bank::kLightIndices, cl.indices().empty() ? rhi::as_bytes(kNoIndex) : std::as_bytes(cl.indices()));
    }
    cmd.set_storage_data(rhi::bank::kLightPool, cl.lights().empty() ? rhi::as_bytes(kNoLight) : std::as_bytes(cl.lights()));
    stats_.point_lights = static_cast<u32>(cl.lights().size());
    stats_.light_indices = static_cast<u32>(cl.indices().size());

}

void Forward3D::record_main(rhi::CommandBuffer& cmd, const Scene3D& scene) {
    cmd.push_debug_scope("forward");
    const Fog& fog = scene.fog();
    const Sky& sky = scene.sky();
    rhi::PassAttachment color;
    color.texture = desc_.msaa > 1 ? hdr_msaa_ : hdr_;
    color.clear.color[0] = fog.color.x;
    color.clear.color[1] = fog.color.y;
    color.clear.color[2] = fog.color.z;
    color.clear.color[3] = 1.0f;
    rhi::PassAttachment depth;
    depth.texture = depth_;
    depth.clear.depth = 1.0f;
    depth.store = rhi::StoreOp::DontCare;
    rhi::RenderPassDesc mp;
    mp.colors = Span<const rhi::PassAttachment>(&color, 1);
    mp.depth = depth;
    mp.debug_name = "forward";
    cmd.begin_pass(mp);

    bind_main_state(cmd, scene, true);

    DrawCache cache;
    const Span<const Scene3D::Draw> draws = scene.draws();
    draw_meshes(cmd, scene, draws, 0, scene.opaque_count(), scene.opaque_count(), cache, true);
    draw_batches(cmd, scene, cache, false);
    if (sky.enabled) {  // behind the opaques, before the blended draws composite over it
        cmd.bind_pipeline(sky_);
        cache.pipe = sky_;
        ++stats_.pipeline_binds;
        cmd.draw_non_indexed(3);
    }
    draw_meshes(cmd, scene, draws, scene.opaque_count(), static_cast<u32>(draws.size()), scene.opaque_count(), cache, true);
    draw_particles(cmd, scene);
    cmd.end_pass();
    cmd.pop_debug_scope();
}

// ---- 1b. the irradiance volume (M-VISUAL 6): filled (or loaded from its bake) when it appears or
// changes, then `probes_per_frame` probes re-captured every frame and blended in by the hysteresis.
void Forward3D::record_gi(rhi::CommandBuffer& cmd, const Scene3D& scene) {
    gi_shadowed_ = false;
    const GiVolume& gi = scene.gi_volume();
    if (!gi.enabled || gi_project_.is_null()) return;
    const rhi::Extent2D vs = gi_volume_size(gi);
    rhi::TextureHandle baked;
    if (gi.baked != ~0u && textures_->contains(gi.baked)) {
        const rhi::TextureHandle t = textures_->get(gi.baked);
        const rhi::TextureDesc& d = device_->texture_desc(t);
        if (d.size == vs && d.array_layers == 1)
            baked = t;
        else if (gi_key_ == 0)
            MCE_LOG_WARN("render3d: the GI volume bake is {}x{}x{}, not {}x{}x1: filled at runtime", d.size.width,
                         d.size.height, d.array_layers, vs.width, vs.height);
    }
    if (!gi.updates && baked.is_null()) return;  // the Low tier without a bake: the sky's ambient
    if (gi_volume_.is_null() || !(gi_size_ == vs)) {
        destroy(*device_, gi_volume_);
        rhi::TextureDesc d;
        d.format = rhi::TextureFormat::RGBA16F;
        d.usage = rhi::TextureUsage::SampledColorTarget;
        d.size = vs;
        d.debug_name = "mce.3d.gi_volume";
        gi_volume_ = device_->create_texture(d);
        gi_size_ = vs;
        gi_key_ = 0;
    }
    const u32 total = gi.counts[0] * gi.counts[1] * gi.counts[2];
    const u64 key = gi_key(gi, baked);
    cmd.push_debug_scope("gi");
    if (key != gi_key_) {
        gi_key_ = key;
        gi_cursor_ = 0;
        gi_filled_ = baked.is_null() ? 0 : total;
        rhi::PassAttachment out;
        out.texture = gi_volume_;
        out.load = baked.is_null() ? rhi::LoadOp::Clear : rhi::LoadOp::DontCare;  // the fill's captures read zeros
        out.clear.color[0] = out.clear.color[1] = out.clear.color[2] = out.clear.color[3] = 0.0f;
        rhi::RenderPassDesc p;
        p.colors = Span<const rhi::PassAttachment>(&out, 1);
        p.debug_name = baked.is_null() ? "gi.clear" : "gi.decode";
        cmd.begin_pass(p);
        if (!baked.is_null()) {
            cmd.bind_pipeline(gi_decode_);
            const vec4 du{0.0f, 0.0f, kGiRgbmRange, 0.0f};
            cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(du));
            cmd.bind_texture(rhi::bank::kFirstSampler, baked, point_sampler_);
            cmd.draw_non_indexed(3);
        }
        cmd.end_pass();
        if (!baked.is_null()) stats_.gi_loaded = true;
    }
    if (gi_filled_ < total) {
        // The fill: kGiMaxBatchProbes a frame (or what the uniform budget allows); the volume lights
        // the frame once every probe has been captured.
        gi_filled_ += gi_capture(cmd, scene, gi_filled_, min(kGiMaxBatchProbes, total - gi_filled_), 1.0f);
        stats_.gi_filled = gi_filled_ == total;
    } else if (!stats_.gi_loaded && gi.updates && gi.probes_per_frame > 0) {
        const u32 n = min(min(gi.probes_per_frame, kGiMaxBatchProbes), total);
        gi_cursor_ %= total;
        gi_cursor_ = (gi_cursor_ + gi_capture(cmd, scene, gi_cursor_, n, clamp(1.0f - gi.hysteresis, 0.0f, 1.0f))) % total;
    }
    stats_.gi = gi_filled_ == total;
    cmd.pop_debug_scope();
}

// Probes first, first + 1, ... (x-major, wrapping past the last) of the volume, up to `count`: six
// faces each, then their projection, a pass per volume row touched. Stops early at the probe that
// spends kGiCaptureBudget uniform uploads; returns the probes captured.
u32 Forward3D::gi_capture(rhi::CommandBuffer& cmd, const Scene3D& scene, u32 first, u32 count, f32 weight) {
    const GiVolume& gi = scene.gi_volume();
    const u32 cx = gi.counts[0], cy = gi.counts[1], total = cx * cy * gi.counts[2];
    const u32 layers = 6 * kGiMaxBatchProbes;
    u32 budget = kGiCaptureBudget;
    if (gi_faces_.is_null() || gi_face_layers_ != layers) {
        destroy(*device_, gi_faces_);
        destroy(*device_, gi_faces_depth_);
        rhi::TextureDesc d;
        d.format = rhi::TextureFormat::RGBA16F;
        d.usage = rhi::TextureUsage::SampledColorTarget;
        d.size = {kGiFaceSize, kGiFaceSize};
        d.dimension = rhi::TextureDimension::Tex2DArray;
        d.array_layers = static_cast<u16>(layers);
        d.debug_name = "mce.3d.gi_faces";
        gi_faces_ = device_->create_texture(d);
        d.format = rhi::TextureFormat::D32_Float;
        d.usage = rhi::TextureUsage::DepthTarget;
        d.debug_name = "mce.3d.gi_faces_depth";
        gi_faces_depth_ = device_->create_texture(d);
        gi_face_layers_ = layers;
    }
    const DirectionalLight& light = scene.light();
    vec3 dir = normalize(light.direction);
    if (!(length_sq(dir) > 0.0f)) dir = {0, -1, 0};
    // The sun's shadow over the whole volume, once per frame: an orthographic box around the volume's
    // bounding sphere, stretched toward the light over every caster.
    const Span<const Scene3D::Draw> casters = scene.shadow_draws();
    if (!gi_shadowed_ && light.shadows && !casters.empty() && gi_shadow_layer_ > 0) {
        const vec3 c = gi.bounds.center();
        const f32 r = max(length(gi.bounds.extents()), 0.01f);
        const aabb3& sb = scene.scene_bounds();
        const f32 reach = r + length(sb.center() - c) + length(sb.extents());
        const vec3 up = std::fabs(dir.y) > 0.99f ? vec3{1, 0, 0} : vec3{0, 1, 0};
        const mat4 lv = look_at_rh(c - dir * reach, c, up);
        gi_shadow_vp_ = ortho_rh_no(-r, r, -r, r, 0.0f, reach + r) * lv;
        gi_shadow_texel_ = 2.0f * r / f32(desc_.shadow_size);
        rhi::RenderPassDesc sp;
        rhi::PassAttachment depth;
        depth.texture = shadow_map_;
        depth.layer = static_cast<u16>(gi_shadow_layer_);
        depth.clear.depth = 1.0f;
        sp.depth = depth;
        sp.debug_name = "gi.shadow";
        cmd.begin_pass(sp);
        rhi::PipelineHandle bound_pipe;
        const GpuMesh* bound = nullptr;
        for (const Scene3D::Draw& d : casters) shadow_draw(cmd, gi_shadow_vp_, d, bound_pipe, bound);
        cmd.end_pass();
        gi_shadowed_ = true;
        budget -= min(budget, static_cast<u32>(casters.size()));
    }

    // A lit pass's state, as the main pass's but for one cluster holding every light.
    const Fog& fog = scene.fog();
    const Sky& sky = scene.sky();
    const View3D& view = scene.view();
    PassU pu{};
    pu.light_dir = vec4(dir, gi_shadowed_ ? 1.0f : 0.0f);
    pu.light_color = vec4(light.color * light.intensity, 0.0f);
    pu.amb_sky = vec4(light.ambient_sky, 0.0f);
    pu.amb_ground = vec4(light.ambient_ground, 0.0f);
    pu.fog = vec4(fog.color, fog.density);
    for (mat4& m : pu.cascade) m = mat4::identity();
    pu.cascade[0] = gi_shadow_vp_;
    pu.cascade_far = {3.0e38f, 0.0f, 0.0f, 0.0f};
    pu.cascade_texel = {gi_shadow_texel_, 0.0f, 0.0f, 0.0f};
    // The captures read the volume as it stands (zeros while it fills): each update adds a bounce.
    const rhi::TextureHandle sky_tex = bind_ambient(cmd, scene, false, true, pu);
    bind_local_shadows(cmd, scene, pu);
    pu.local.y = f32(gi_shadow_layer_);  // cascade 0 is the volume's fit
    cmd.bind_texture(rhi::bank::kAo, ao_off_, linear_clamp_);
    // Every light in the pool; each probe's single cluster lists the kGiMaxCaptureLights that matter
    // most to it (brightness × how much of its range the probe sits in). Storage bindings outlive passes.
    const ClusterLights& cl = scene.cluster_setup();
    const Span<const GpuPointLight> pool = cl.lights();
    static constexpr GpuPointLight kNoLight{};
    cmd.set_storage_data(rhi::bank::kLightPool, pool.empty() ? rhi::as_bytes(kNoLight) : std::as_bytes(pool));
    struct Ranked {
        f32 score;
        u32 light;
    };
    Array<Ranked> ranked(mem::heap());
    ranked.reserve(pool.size());
    u32 picked[kGiMaxCaptureLights] = {};
    const Span<const Scene3D::Draw> draws = scene.probe_draws();
    Array<Scene3D::Draw> visible(mem::heap());
    visible.reserve(draws.size());

    // A draw narrower than a face texel (2r / d below 90° / kGiFaceSize) adds nothing a 16² face can hold.
    constexpr f32 kGiMinTexelAngle = 0.5f * 1.5707963f / f32(kGiFaceSize);
    struct Sized {
        f32 size;  // r / d
        u32 draw;
    };
    Array<Sized> sized(mem::heap());
    sized.reserve(draws.size());
    // A probe goes in when the budget still holds one as costly as the last (the first always does).
    u32 done = 0, probe_cost = 0;
    for (; done < count && (done == 0 || (budget > 0 && budget >= probe_cost)); ++done) {
        const u32 probe = (first + done) % total, ix = probe % cx, vrow = probe / cx, iy = vrow % cy, iz = vrow / cy;
        const u32 before = budget;
        {
            const vec3 t{cx > 1 ? f32(ix) / f32(cx - 1) : 0.5f, f32(iy) / f32(max(cy - 1, 1u)),
                         f32(iz) / f32(max(gi.counts[2] - 1, 1u))};
            const vec3 pos = gi.bounds.min + (gi.bounds.max - gi.bounds.min) * t;
            mat4 face_view[6], face_vp[6];
            probe_face_views(pos, view.near_z, view.far_z, face_view, face_vp);
            ranked.clear();
            for (u32 i = 0; i < pool.size(); ++i) {
                const GpuPointLight& l = pool[i];
                const vec3 c{l.color.x, l.color.y, l.color.z};
                const f32 r2 = l.position_radius.w * l.position_radius.w;
                const f32 d2 = length_sq(l.position_radius.xyz() - pos);
                ranked.push_back({(0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z) * r2 / (d2 + r2), i});
            }
            const u32 k = min(static_cast<u32>(ranked.size()), kGiMaxCaptureLights);
            std::partial_sort(ranked.begin(), ranked.begin() + k, ranked.end(),
                              [](const Ranked& a, const Ranked& b) { return a.score > b.score || (a.score == b.score && a.light < b.light); });
            for (u32 i = 0; i < k; ++i) picked[i] = ranked[i].light;
            const ClusterRange range{0, k};
            cmd.set_storage_data(rhi::bank::kClusters, rhi::as_bytes(range));
            cmd.set_storage_data(rhi::bank::kLightIndices, Span<const byte>(reinterpret_cast<const byte*>(picked), sizeof(u32) * max(k, 1u)));
            stats_.gi_lights = max(stats_.gi_lights, k);
            for (u32 f = 0; f < 6; ++f) {
                const u32 layer = done * 6 + f;
                rhi::PassAttachment color;
                color.texture = gi_faces_;
                color.layer = static_cast<u16>(layer);
                color.clear.color[0] = fog.color.x;
                color.clear.color[1] = fog.color.y;
                color.clear.color[2] = fog.color.z;
                color.clear.color[3] = 1.0f;
                rhi::PassAttachment depth;
                depth.texture = gi_faces_depth_;
                depth.layer = static_cast<u16>(layer);
                depth.clear.depth = 1.0f;
                depth.store = rhi::StoreOp::DontCare;
                rhi::RenderPassDesc fp;
                fp.colors = Span<const rhi::PassAttachment>(&color, 1);
                fp.depth = depth;
                fp.debug_name = "gi.face";
                cmd.begin_pass(fp);
                FrameU fu{};
                fu.view_proj = face_vp[f];
                fu.view = face_view[f];
                fu.inv_view_proj = inverse(face_vp[f]);
                fu.camera_pos = vec4(pos, 0.0f);
                fu.params = {scene.exposure(), 0.0f, 0.0f, 0.0f};
                const f32 px = f32(kGiFaceSize);
                fu.viewport = {px, px, 1.0f / px, 1.0f / px};
                fu.cluster = {1.0f, 1.0f, 1.0f, f32(k)};
                fu.cluster_z = {0.0f, 0.0f, view.near_z, view.far_z};
                cmd.set_uniforms(rhi::bank::kFrame, rhi::as_bytes(fu));
                cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(pu));
                cmd.bind_texture(rhi::bank::kShadowMap, shadow_map_, shadow_sampler_);
                cmd.bind_texture(rhi::bank::kSky, sky_tex, material_sampler_);
                vec4 planes[6];
                frustum_planes(face_vp[f], planes);
                sized.clear();
                for (u32 i = 0; i < draws.size(); ++i) {
                    const Scene3D::Draw& d = draws[i];
                    const f32 size = d.radius / max(length(d.center - pos), 1e-4f);
                    if (size >= kGiMinTexelAngle && sphere_in(planes, d.center, d.radius)) sized.push_back({size, i});
                }
                if (sized.size() > kGiMaxFaceDraws) {  // the widest, then back in draw (sort-key) order
                    std::nth_element(sized.begin(), sized.begin() + kGiMaxFaceDraws, sized.end(),
                                     [](const Sized& a, const Sized& b) { return a.size > b.size || (a.size == b.size && a.draw < b.draw); });
                    sized.resize(kGiMaxFaceDraws);
                    std::sort(sized.begin(), sized.end(), [](const Sized& a, const Sized& b) { return a.draw < b.draw; });
                }
                visible.clear();
                for (const Sized& z : sized) visible.push_back(draws[z.draw]);
                DrawCache cache;
                const u32 nv = static_cast<u32>(visible.size());
                draw_meshes(cmd, scene, visible.span(), 0, nv, nv, cache, false);
                stats_.gi_draws += nv;
                budget -= min(budget, nv + 3);
                if (sky.enabled) {
                    cmd.bind_pipeline(sky_);
                    cmd.draw_non_indexed(3);
                }
                cmd.end_pass();
            }
        }
        probe_cost = before - budget;
    }
    // The faces into the volume, a pass per run of probes along one row (the pass's viewport;
    // Rect2D is top-left based, row 0 the volume's v 0).
    for (u32 j = 0; j < done;) {
        const u32 probe = (first + j) % total, ix = probe % cx, vrow = probe / cx;
        const u32 n = min(done - j, cx - ix);
        rhi::PassAttachment out;
        out.texture = gi_volume_;
        out.load = rhi::LoadOp::Load;
        rhi::RenderPassDesc p;
        p.colors = Span<const rhi::PassAttachment>(&out, 1);
        p.viewport = rhi::Rect2D{i32(ix * 4), i32(gi_size_.height - vrow - 1), n * 4, 1};
        p.debug_name = "gi.project";
        cmd.begin_pass(p);
        cmd.bind_pipeline(gi_project_);
        const GiProjectU gu{{f32(n * 4), 1.0f, f32(kGiFaceSize), 0.0f}, {f32(n), weight, f32(j), 0.0f}};
        cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(gu));
        cmd.bind_texture(rhi::bank::kFirstSampler, gi_faces_, point_sampler_);
        cmd.draw_non_indexed(3);
        cmd.end_pass();
        j += n;
    }
    stats_.gi_probes += done;
    return done;
}

bool Forward3D::record_gi_export(rhi::CommandBuffer& cmd) {
    MCE_ASSERT_MSG(!cmd.in_pass(), "Forward3D::record_gi_export inside a pass");
    if (gi_volume_.is_null() || gi_key_ == 0 || gi_encode_.is_null()) return false;
    if (gi_export_.is_null() || !(device_->texture_desc(gi_export_).size == gi_size_)) {
        destroy(*device_, gi_export_);
        rhi::TextureDesc d;
        d.format = rhi::TextureFormat::RGBA8_UNorm;
        d.usage = rhi::TextureUsage::SampledColorTarget;
        d.size = gi_size_;
        d.debug_name = "mce.3d.gi_export";
        gi_export_ = device_->create_texture(d);
    }
    rhi::PassAttachment out;
    out.texture = gi_export_;
    out.load = rhi::LoadOp::DontCare;
    rhi::RenderPassDesc p;
    p.colors = Span<const rhi::PassAttachment>(&out, 1);
    p.debug_name = "gi.export";
    cmd.begin_pass(p);
    cmd.bind_pipeline(gi_encode_);
    const vec4 eu{0.0f, 0.0f, kGiRgbmRange, 0.0f};
    cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(eu));
    cmd.bind_texture(rhi::bank::kFirstSampler, gi_volume_, point_sampler_);
    cmd.draw_non_indexed(3);
    cmd.end_pass();
    return true;
}

rhi::TextureHandle Forward3D::bind_ambient(rhi::CommandBuffer& cmd, const Scene3D& scene, bool probes, bool gi_on, PassU& pu) {
    const Sky& sky = scene.sky();
    const rhi::TextureHandle sky_tex = sky_params(*textures_, *device_, sky, pu, sky_texture_override());
    if (stats_.ibl) {
        pu.ibl = {1.0f, sky.diffuse_ambient, f32(kIblLayers), ibl_own_source_ ? 1.0f : 0.0f};
        cmd.bind_texture(rhi::bank::kIblEnv, ibl_env_, env_sampler_);
        cmd.bind_texture(rhi::bank::kIblBrdfLut, ibl_lut_, linear_clamp_);
        cmd.bind_texture(rhi::bank::kIblSh, ibl_sh_, point_sampler_);
        const Span<const ReflectionProbe> rp = scene.reflection_probes();
        if (probes && stats_.probes > 0) {
            pu.probes.x = f32(rp.size());
            for (u32 i = 0; i < rp.size(); ++i) {
                const ReflectionProbe& p = rp[i];
                pu.probe_pos[i] = vec4(p.position, p.intensity);
                pu.probe_min[i] = vec4(p.bounds.min, max(p.blend_distance, 1e-3f));
                pu.probe_max[i] = vec4(p.bounds.max, f32(probe_base_layer(i)));
            }
        }
    } else {
        // IBL off: the shader branches around these samplers, but Metal's validation layer still wants
        // every declared one bound (and of the declared type), so stand-ins fill the slots unread.
        if (ibl_off_env_.is_null()) {
            rhi::TextureDesc d;
            d.format = rhi::TextureFormat::RGBA16F;
            d.usage = rhi::TextureUsage::SampledColorTarget;
            d.size = {1, 1};
            d.dimension = rhi::TextureDimension::Tex2DArray;
            d.array_layers = 1;
            d.debug_name = "mce.3d.ibl_off_env";
            ibl_off_env_ = device_->create_texture(d);
            d.dimension = rhi::TextureDimension::Tex2D;
            d.debug_name = "mce.3d.ibl_off_2d";
            ibl_off_2d_ = device_->create_texture(d);
        }
        cmd.bind_texture(rhi::bank::kIblEnv, ibl_off_env_, linear_clamp_);
        cmd.bind_texture(rhi::bank::kIblBrdfLut, ibl_off_2d_, linear_clamp_);
        cmd.bind_texture(rhi::bank::kIblSh, ibl_off_2d_, point_sampler_);
    }
    cmd.bind_texture(rhi::bank::kVolumetrics, ao_off_, linear_clamp_);  // the main pass binds the fog over it
    // The irradiance volume (record_gi() filled it this frame or before), or a stand-in.
    if (gi_on) {
        const GiVolume& gi = scene.gi_volume();
        pu.gi = {1.0f, gi.intensity, max(gi.normal_bias, 0.0f), max(gi.fade_distance, 1e-3f)};
        pu.gi_min = vec4(gi.bounds.min, 0.0f);
        pu.gi_max = vec4(gi.bounds.max, 0.0f);
        pu.gi_counts = {f32(gi.counts[0]), f32(gi.counts[1]), f32(gi.counts[2]), 0.0f};
        cmd.bind_texture(rhi::bank::kGiVolume, gi_volume_, point_sampler_);
    } else {
        cmd.bind_texture(rhi::bank::kGiVolume, ao_off_, point_sampler_);
    }
    return sky_tex;
}

void Forward3D::bind_local_shadows(rhi::CommandBuffer& cmd, const Scene3D& scene, PassU& pu) {
    // Local-light shadow records, one per atlas tile (GpuPointLight::shadow points into them).
    static constexpr LocalShadowRecord kNoShadow{};
    if (!local_shadows_) {
        cmd.set_storage_data(rhi::bank::kLocalShadows, rhi::as_bytes(kNoShadow));
        return;
    }
    const Span<const LocalShadowTile> tiles = scene.local_shadow_tiles();
    Array<LocalShadowRecord> records(mem::heap());
    records.resize(tiles.size());
    const f32 inv = 1.0f / f32(desc_.shadow_size);
    for (u32 i = 0; i < tiles.size(); ++i) {
        const LocalShadowTile& t = tiles[i];
        // Rect2D is top-left based; the layer holds GL's layout, whose row 0 is the bottom.
        const f32 v0 = f32(i32(desc_.shadow_size) - t.rect.y - i32(t.rect.height)) * inv;
        records[i] = {t.view_proj, vec4(f32(t.rect.x) * inv, v0, f32(t.rect.width) * inv, f32(t.rect.height) * inv),
                      vec4(f32(kMaxCascades + t.layer), t.texel_scale, 0.0f, 0.0f)};
    }
    cmd.set_storage_data(rhi::bank::kLocalShadows, std::as_bytes(records.span()));
    pu.local.x = 1.0f;
}

void Forward3D::draw_meshes(rhi::CommandBuffer& cmd, const Scene3D& scene, Span<const Scene3D::Draw> draws, u32 begin,
                            u32 end, u32 opaque_end, DrawCache& cache, bool main, bool prepass) {
    const rhi::TextureHandle white = textures_->white();
    static constexpr u32 kSlots[4] = {rhi::bank::kAlbedo, rhi::bank::kNormal, rhi::bank::kOrm, rhi::bank::kEmissive};
    auto bind_map = [&](DrawCache& c, u32 i, u32 key) {
        if (key == c.tex[i]) return;
        const rhi::TextureHandle tex = key == ~0u ? white : textures_->get(key);
        cmd.bind_texture(kSlots[i], tex, material_sampler_);
        c.tex[i] = key;
        if (main) ++stats_.texture_binds;
    };
    for (u32 i = begin; i < end; ++i) {
        const Scene3D::Draw& d = draws[i];
        if (d.mesh->vertices.is_null()) continue;
        const GpuSubmesh& sm = d.mesh->submeshes[d.submesh];
        const MeshInstance& inst = scene.instances()[d.instance];
        const asset::MaterialChunk& mat = material_of(inst, sm);
        const u32 albedo = albedo_key_of(inst, sm), normal = normal_key_of(inst, sm), orm = orm_key_of(inst, sm),
                  emissive = emissive_key_of(inst, sm);
        const bool skinned = d.mesh->skinned();
        const rhi::PipelineHandle pipe = prepass                                   ? (skinned ? ao_prepass_skinned_ : ao_prepass_)
                                         : i >= opaque_end                         ? (skinned ? skinned_blend_ : mesh_blend_)
                                         : (mat.flags & asset::MaterialFlags::DoubleSided) ? (skinned ? skinned_two_sided_ : mesh_two_sided_)
                                                                                           : (skinned ? skinned_cull_ : mesh_cull_);
        if (!(pipe == cache.pipe)) {
            cmd.bind_pipeline(pipe);
            cache.pipe = pipe;
            if (main) ++stats_.pipeline_binds;
        }
        if (d.mesh != cache.mesh) {
            cmd.bind_vertex_buffer(d.mesh->vertices);
            cmd.bind_index_buffer(d.mesh->indices, d.mesh->index_format);
            cache.mesh = d.mesh;
        }
        bind_map(cache, 0, albedo);
        bind_map(cache, 1, normal);
        bind_map(cache, 2, orm);
        bind_map(cache, 3, emissive);
        ObjectU ou;
        ou.model = d.model;
        ou.normal = d.normal;
        ou.uv_rect = sm.uv_rect;
        ou.base_color = {mat.base_color[0] * inst.tint.x, mat.base_color[1] * inst.tint.y,
                         mat.base_color[2] * inst.tint.z, mat.base_color[3] * inst.tint.w};
        ou.emissive = {mat.emissive[0] * inst.emissive_tint.x, mat.emissive[1] * inst.emissive_tint.y,
                       mat.emissive[2] * inst.emissive_tint.z, emissive != ~0u ? 1.0f : 0.0f};
        ou.params = {(mat.flags & asset::MaterialFlags::AlphaMask) ? mat.alpha_cutoff : 0.0f,
                     (mat.flags & asset::MaterialFlags::Unlit) ? 1.0f : 0.0f, mat.roughness, mat.metallic};
        ou.maps = {normal != ~0u ? 1.0f : 0.0f, orm != ~0u ? 1.0f : 0.0f,
                   (mat.flags & asset::MaterialFlags::OcclusionInMR) ? 1.0f : 0.0f,
                   (mat.flags & asset::MaterialFlags::OcclusionOnly) ? 1.0f : 0.0f};
        ou.dequant = d.dequant;
        ou.skin = {palette_index(d.palette), 0.0f, 0.0f, 0.0f};
        cmd.set_uniforms(rhi::bank::kObject, rhi::as_bytes(ou));
        cmd.draw(d.index_count, 1, d.first_index, 0, static_cast<i32>(d.base_vertex));
        if (main) ++stats_.main_draws;
    }
}

// ---- instanced batches (M-VISUAL 9) of the main view: the main pass or the SSAO prepass. Each
// (submesh, level) is one draw of its visible list; o_skin.y names the list, o_inst the wind.
void Forward3D::draw_batches(rhi::CommandBuffer& cmd, const Scene3D& scene, DrawCache& cache, bool prepass) {
    if (batch_view_count_ == 0 || (prepass ? inst_prepass_.is_null() : inst_cull_.is_null())) return;
    const rhi::TextureHandle white = textures_->white();
    static constexpr u32 kSlots[4] = {rhi::bank::kAlbedo, rhi::bank::kNormal, rhi::bank::kOrm, rhi::bank::kEmissive};
    const Span<const InstanceBatch> batches = scene.batches();
    for (u32 bi = 0; bi < batch_count_; ++bi) {
        if (!batch_views_[bi].culled) continue;
        const InstanceBatch& b = batches[bi];
        const GpuMesh& mesh = *scene.meshes().get(b.mesh);
        if (mesh.vertices.is_null()) continue;
        bind_batch(cmd, mesh, bi);
        cache.mesh = nullptr;  // the batch's buffers replaced the cached mesh's
        const BatchView& bv = batch_views_[bi];
        const u32 levels = max(mesh.lod_levels, 1u);
        for (u32 s = 0; s < mesh.submeshes.size(); ++s) {
            const GpuSubmesh& sm = mesh.submeshes[s];
            const asset::MaterialChunk& mat = b.material ? b.material->params : sm.material;
            const u32 keys[4] = {b.material ? b.material->albedo_key : sm.albedo_key, b.material ? b.material->normal_key : sm.normal_key,
                                 b.material ? b.material->orm_key : sm.orm_key, b.material ? b.material->emissive_key : sm.emissive_key};
            const rhi::PipelineHandle pipe =
                prepass ? inst_prepass_ : (mat.flags & asset::MaterialFlags::DoubleSided) ? inst_two_sided_ : inst_cull_;
            if (!(pipe == cache.pipe)) {
                cmd.bind_pipeline(pipe);
                cache.pipe = pipe;
                if (!prepass) ++stats_.pipeline_binds;
            }
            for (u32 t = 0; t < 4; ++t) {
                if (keys[t] == cache.tex[t]) continue;
                cmd.bind_texture(kSlots[t], keys[t] == ~0u ? white : textures_->get(keys[t]), material_sampler_);
                cache.tex[t] = keys[t];
                if (!prepass) ++stats_.texture_binds;
            }
            const mat4& node = mesh.rest_pose[sm.node];
            ObjectU ou;
            ou.model = node * sm.dequant;
            ou.normal = transpose(inverse(node));
            ou.uv_rect = sm.uv_rect;
            ou.base_color = {mat.base_color[0] * b.tint.x, mat.base_color[1] * b.tint.y, mat.base_color[2] * b.tint.z,
                             mat.base_color[3] * b.tint.w};
            ou.emissive = {mat.emissive[0], mat.emissive[1], mat.emissive[2], keys[3] != ~0u ? 1.0f : 0.0f};
            ou.params = {(mat.flags & asset::MaterialFlags::AlphaMask) ? mat.alpha_cutoff : 0.0f,
                         (mat.flags & asset::MaterialFlags::Unlit) ? 1.0f : 0.0f, mat.roughness, mat.metallic};
            ou.maps = {keys[1] != ~0u ? 1.0f : 0.0f, keys[2] != ~0u ? 1.0f : 0.0f,
                       (mat.flags & asset::MaterialFlags::OcclusionInMR) ? 1.0f : 0.0f,
                       (mat.flags & asset::MaterialFlags::OcclusionOnly) ? 1.0f : 0.0f};
            ou.dequant = mat4::identity();
            ou.inst = batch_wind(b);
            for (u32 l = 0; l < levels; ++l) {
                ou.skin = {-1.0f, f32(bv.slot[l]), scene.time(), 0.0f};
                cmd.set_uniforms(rhi::bank::kObject, rhi::as_bytes(ou));
                if (batch_draw(cmd, mesh, 0, bi, l, s) && !prepass) ++stats_.main_draws;
            }
        }
    }
}

// ---- 4-6. bloom, tonemap, FXAA
// ---- 3. TAA (M-VISUAL 5): the velocity prepass, then the history resolve into the next history slot.
// The output is the full viewport, so a render_scale < 1 frame comes out upsampled.
void Forward3D::record_velocity(rhi::CommandBuffer& cmd, const Scene3D& scene) {
    const View3D& view = scene.view();
    if (velocity_.is_null()) {
        rhi::TextureDesc d;
        d.size = size_;
        d.format = rhi::TextureFormat::RGBA16F;
        d.usage = rhi::TextureUsage::SampledColorTarget;
        d.debug_name = "mce.3d.velocity";
        velocity_ = device_->create_texture(d);
        rhi::TextureDesc z;
        z.size = size_;
        z.format = rhi::TextureFormat::D32_Float;
        z.usage = rhi::TextureUsage::DepthTarget;
        z.debug_name = "mce.3d.velocity_depth";
        velocity_depth_ = device_->create_texture(z);
    }
    cmd.push_debug_scope("velocity");
    // The opaques, unjittered positions now and last frame, rasterized with the jitter (if any).
    rhi::PassAttachment color;
    color.texture = velocity_;
    rhi::PassAttachment depth;
    depth.texture = velocity_depth_;
    depth.clear.depth = 1.0f;
    depth.store = rhi::StoreOp::DontCare;
    rhi::RenderPassDesc vp;
    vp.colors = Span<const rhi::PassAttachment>(&color, 1);
    vp.depth = depth;
    vp.debug_name = "taa.velocity";
    cmd.begin_pass(vp);
    const mat4 previous = velocity_prev_valid_ ? prev_view_proj_ : view.view_proj;
    const VelPassU pu{jitter_ * view.view_proj, view.view_proj, previous};
    cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(pu));
    rhi::PipelineHandle bound_pipe;
    const GpuMesh* bound = nullptr;
    const Span<const Scene3D::Draw> draws = scene.draws();
    for (u32 i = 0; i < scene.opaque_count(); ++i) {
        const Scene3D::Draw& d = draws[i];
        if (d.mesh->vertices.is_null()) continue;
        const rhi::PipelineHandle pipe = d.mesh->skinned() ? velocity_skinned_ : velocity_pipe_;
        if (!(pipe == bound_pipe)) {
            cmd.bind_pipeline(pipe);
            bound_pipe = pipe;
        }
        if (d.mesh != bound) {
            cmd.bind_vertex_buffer(d.mesh->vertices);
            cmd.bind_index_buffer(d.mesh->indices, d.mesh->index_format);
            bound = d.mesh;
        }
        const VelObjU ou{d.model, d.prev_model, d.dequant, {palette_index(d.palette), palette_index(d.prev_palette), 0.0f, 0.0f}};
        cmd.set_uniforms(rhi::bank::kObject, rhi::as_bytes(ou));
        cmd.draw(d.index_count, 1, d.first_index, 0, static_cast<i32>(d.base_vertex));
        ++stats_.velocity_draws;
    }
    draw_batches_depth(cmd, scene, 0, view.view_proj, true);
    cmd.end_pass();
    cmd.pop_debug_scope();
    // The camera reprojection of this frame (TAA's sky, motion blur's) uses the previous matrix.
    velocity_prev_vp_used_ = previous;
    prev_view_proj_ = view.view_proj;
    velocity_prev_valid_ = true;
}

void Forward3D::record_taa(rhi::CommandBuffer& cmd, const Scene3D& scene) {
    const View3D& view = scene.view();
    const rhi::Extent2D out = view.viewport;
    if (!(history_size_ == out)) {
        for (rhi::TextureHandle& h : history_) destroy(*device_, h);
        rhi::TextureDesc d;
        d.size = out;
        d.format = rhi::TextureFormat::RGBA16F;
        d.usage = rhi::TextureUsage::SampledColorTarget;
        d.debug_name = "mce.3d.taa_history0";
        history_[0] = device_->create_texture(d);
        d.debug_name = "mce.3d.taa_history1";
        history_[1] = device_->create_texture(d);
        history_size_ = out;
        history_valid_ = false;
    }

    cmd.push_debug_scope("taa");
    const mat4 previous = velocity_prev_vp_used_;
    // Resolve: history_[next] = clip(history_[current] reprojected) blended with this frame.
    const u32 next = history_index_ ^ 1u;
    rhi::PassAttachment o;
    o.texture = history_[next];
    o.load = rhi::LoadOp::DontCare;
    rhi::RenderPassDesc rp;
    rp.colors = Span<const rhi::PassAttachment>(&o, 1);
    rp.debug_name = "taa.resolve";
    cmd.begin_pass(rp);
    cmd.bind_pipeline(taa_pipe_);
    TaaU tu;
    tu.reproject = previous * inverse(view.view_proj);
    tu.in = {1.0f / f32(size_.width), 1.0f / f32(size_.height), f32(size_.width), f32(size_.height)};
    tu.out = {1.0f / f32(out.width), 1.0f / f32(out.height), f32(out.width), f32(out.height)};
    tu.jitter = {jitter_uv_.x, jitter_uv_.y, history_valid_ ? 1.0f : 0.0f, clamp(scene.post().taa_blend, 0.01f, 1.0f)};
    cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(tu));
    cmd.bind_texture(rhi::bank::kAlbedo, hdr_, point_sampler_);
    cmd.bind_texture(rhi::bank::kNormal, history_[history_index_], linear_clamp_);
    cmd.bind_texture(rhi::bank::kOrm, velocity_, point_sampler_);
    cmd.draw_non_indexed(3);
    cmd.end_pass();
    cmd.pop_debug_scope();

    stats_.taa = true;
    stats_.taa_history = history_valid_;
    history_index_ = next;
    history_valid_ = true;
    post_source_ = history_[next];
    post_size_ = out;
}

rhi::TextureHandle Forward3D::post_fx_target(rhi::Extent2D size) {
    if (!(fx_size_ == size)) {
        for (rhi::TextureHandle& t : fx_) destroy(*device_, t);
        rhi::TextureDesc d;
        d.size = size;
        d.format = rhi::TextureFormat::RGBA16F;
        d.usage = rhi::TextureUsage::SampledColorTarget;
        d.debug_name = "mce.3d.post_fx0";
        fx_[0] = device_->create_texture(d);
        d.debug_name = "mce.3d.post_fx1";
        fx_[1] = device_->create_texture(d);
        fx_size_ = size;
    }
    const rhi::TextureHandle t = fx_[fx_next_];
    fx_next_ ^= 1u;
    return t;
}

// ---- post effects (M-VISUAL 8): depth of field, motion blur (on post_source_, the HDR frame after TAA),
// then the auto-exposure histogram of the result
void Forward3D::record_post_fx(rhi::CommandBuffer& cmd, const Scene3D& scene) {
    const PostFx& post = scene.post();
    const View3D& view = scene.view();
    const rhi::Extent2D out = post_size_;
    const vec4 texel{1.0f / f32(out.width), 1.0f / f32(out.height), f32(out.width), f32(out.height)};
    auto fullscreen = [&](rhi::TextureHandle dst, const char* name) {
        rhi::PassAttachment o;
        o.texture = dst;
        o.load = rhi::LoadOp::DontCare;
        rhi::RenderPassDesc p;
        p.colors = Span<const rhi::PassAttachment>(&o, 1);
        p.debug_name = name;
        cmd.begin_pass(p);
    };
    if (post.dof.enabled && velocity_on_ && !dof_pipe_.is_null() && post.dof.max_radius > 0.0f) {
        const rhi::TextureHandle dst = post_fx_target(out);
        cmd.push_debug_scope("dof");
        fullscreen(dst, "dof");
        cmd.bind_pipeline(dof_pipe_);
        const f32 radius = min(post.dof.max_radius, 0.05f) * f32(out.height);  // a fraction of the view height, in px
        const DofU du{texel, {max(post.dof.focus_distance, 0.0f), max(post.dof.focus_range, 0.0f), post.dof.blur_range, radius}};
        cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(du));
        cmd.bind_texture(rhi::bank::kAlbedo, post_source_, linear_clamp_);
        cmd.bind_texture(rhi::bank::kNormal, velocity_, point_sampler_);
        cmd.draw_non_indexed(3);
        cmd.end_pass();
        cmd.pop_debug_scope();
        post_source_ = dst;
        stats_.dof = true;
    }
    if (post.motion_blur.enabled && velocity_on_ && !motion_blur_pipe_.is_null() && post.motion_blur.intensity > 0.0f) {
        const rhi::TextureHandle dst = post_fx_target(out);
        cmd.push_debug_scope("motion_blur");
        fullscreen(dst, "motion_blur");
        cmd.bind_pipeline(motion_blur_pipe_);
        MbU mu;
        mu.reproject = velocity_prev_vp_used_ * inverse(view.view_proj);
        mu.texel = texel;
        mu.params = {post.motion_blur.intensity, max(post.motion_blur.max_length, 0.0f),
                     f32(clamp(post.motion_blur.samples, 4u, 32u)), 0.0f};
        cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(mu));
        cmd.bind_texture(rhi::bank::kAlbedo, post_source_, linear_clamp_);
        cmd.bind_texture(rhi::bank::kNormal, velocity_, point_sampler_);
        cmd.draw_non_indexed(3);
        cmd.end_pass();
        cmd.pop_debug_scope();
        post_source_ = dst;
        stats_.motion_blur = true;
    }
    const PostFx::AutoExposure& ae = post.auto_exposure;
    if (ae.enabled && !exposure_hist_.is_null() && !exposure_reduce_.is_null()) {
        cmd.push_debug_scope("exposure");
        constexpr u32 kStep = 2;  // every other texel each way
        ExpU eu;
        eu.src = {f32(out.width), f32(out.height), f32(kStep), 0.0f};
        eu.range = {kExposureMinLog2, f32(kExposureBins - 1) / (kExposureMaxLog2 - kExposureMinLog2), max(ae.speed_up, 0.0f),
                    max(ae.speed_down, 0.0f)};
        eu.target = {max(ae.key, 1e-4f), ae.compensation, clamp(ae.low_percent, 0.0f, 1.0f), clamp(ae.high_percent, 0.0f, 1.0f)};
        eu.adapt = {max(ae.dt, 0.0f), exposure_reset_ ? 1.0f : 0.0f, max(ae.min_exposure, 1e-4f),
                    max(ae.max_exposure, max(ae.min_exposure, 1e-4f))};
        cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(eu));
        cmd.bind_storage_buffer(rhi::bank::kExposure, exposure_buf_, 0, 0);
        cmd.bind_pipeline(exposure_hist_);
        cmd.bind_texture(rhi::bank::kAlbedo, post_source_, point_sampler_);
        const u32 gx = (out.width + 16 * kStep - 1) / (16 * kStep), gy = (out.height + 16 * kStep - 1) / (16 * kStep);
        cmd.dispatch(gx, gy, 1);
        cmd.barrier(rhi::barrier::kStorageBuffer);
        cmd.bind_pipeline(exposure_reduce_);
        cmd.dispatch(1, 1, 1);
        cmd.barrier(rhi::barrier::kStorageBuffer);
        cmd.pop_debug_scope();
        exposure_reset_ = false;
        stats_.auto_exposure = true;
    } else {
        exposure_reset_ = true;  // the next auto-exposure frame snaps
    }
}

void Forward3D::record_post(rhi::CommandBuffer& cmd, const Scene3D& scene, rhi::TextureHandle target) {
    const PostFx& post = scene.post();
    auto fullscreen = [&](rhi::TextureHandle dst, rhi::LoadOp load, const char* name) {
        rhi::PassAttachment out;
        out.texture = dst;
        out.load = load;
        rhi::RenderPassDesc p;
        p.colors = Span<const rhi::PassAttachment>(&out, 1);
        p.debug_name = name;
        cmd.begin_pass(p);
    };

    const bool bloom = post.bloom && bloom_levels_ > 0;
    if (bloom) {
        cmd.push_debug_scope("bloom");
        PostU pu;
        for (u32 i = 0; i < bloom_levels_; ++i) {
            const rhi::TextureHandle src = i == 0 ? post_source_ : bloom_[i - 1];
            const rhi::Extent2D ss = i == 0 ? post_size_ : bloom_size_[i - 1];
            fullscreen(bloom_[i], rhi::LoadOp::DontCare, "bloom.down");
            cmd.bind_pipeline(bloom_down_);
            pu.texel = {1.0f / f32(ss.width), 1.0f / f32(ss.height), stats_.auto_exposure ? 1.0f : 0.0f, 0.0f};
            pu.params = {max(post.bloom_threshold, 0.0f), max(post.bloom_threshold, 0.0f) * 0.5f, i == 0 ? 1.0f : 0.0f,
                         scene.exposure()};
            cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(pu));
            cmd.bind_storage_buffer(rhi::bank::kExposure, exposure_buf_, 0, 0);
            cmd.bind_texture(rhi::bank::kAlbedo, src, linear_clamp_);
            cmd.draw_non_indexed(3);
            cmd.end_pass();
        }
        for (u32 i = bloom_levels_ - 1; i > 0; --i) {
            fullscreen(bloom_[i - 1], rhi::LoadOp::Load, "bloom.up");
            cmd.bind_pipeline(bloom_up_);
            pu.texel = {1.0f / f32(bloom_size_[i].width), 1.0f / f32(bloom_size_[i].height), 0.0f, 0.0f};
            pu.params = {};
            cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(pu));
            cmd.bind_texture(rhi::bank::kAlbedo, bloom_[i], linear_clamp_);
            cmd.draw_non_indexed(3);
            cmd.end_pass();
        }
        cmd.pop_debug_scope();
        stats_.bloom_levels = bloom_levels_;
    }
    timestamp(cmd, kTsBloom);

    const bool fxaa = post.fxaa && !taa_on_;  // TAA replaces FXAA
    const View3D& view = scene.view();
    FrameU fu;
    fu.view_proj = view.view_proj;
    fu.view = view.view;
    fu.inv_view_proj = mat4::identity();
    fu.camera_pos = vec4(view.position, 0.0f);
    fu.params = {scene.exposure(), desc_.encode_srgb_output ? 1.0f : 0.0f,
                 post.bloom_intensity / f32(max(bloom_levels_, 1u)), bloom ? 1.0f : 0.0f};
    fu.viewport = {f32(size_.width), f32(size_.height), 1.0f / f32(size_.width), 1.0f / f32(size_.height)};
    // The tonemap pass's u_cluster: x auto exposure, y color-grade strength (0 = off), z color-vision
    // mode (0 = off), w its strength.
    const rhi::TextureHandle lut = post.lut_key != 0 && textures_->contains(post.lut_key) ? textures_->get(post.lut_key) : rhi::TextureHandle{};
    const f32 grade = lut.is_null() ? 0.0f : clamp(post.lut_strength, 0.0f, 1.0f);
    const f32 cvd_strength = clamp(post.color_vision_strength, 0.0f, 1.0f);
    const f32 cvd = cvd_strength > 0.0f ? f32(post.color_vision) : 0.0f;
    fu.cluster = {stats_.auto_exposure ? 1.0f : 0.0f, grade, cvd, cvd_strength};
    stats_.color_grade = grade > 0.0f;
    stats_.color_vision = cvd > 0.0f;
    fullscreen(fxaa ? ldr_ : target, rhi::LoadOp::DontCare, "tonemap");
    const rhi::Extent2D out = scene.view().viewport;
    cmd.bind_pipeline(tonemap_);
    cmd.set_uniforms(rhi::bank::kFrame, rhi::as_bytes(fu));
    // 1:1 reads texel-exact; a scaled frame is upsampled bilinearly (TAA upsamples it itself).
    cmd.bind_texture(rhi::bank::kAlbedo, post_source_, post_size_ == out ? point_sampler_ : linear_clamp_);
    cmd.bind_texture(rhi::bank::kNormal, bloom ? bloom_[0] : post_source_, linear_clamp_);
    cmd.bind_texture(rhi::bank::kOrm, lut.is_null() || grade <= 0.0f ? textures_->white() : lut, linear_clamp_);
    cmd.bind_storage_buffer(rhi::bank::kExposure, exposure_buf_, 0, 0);
    cmd.draw_non_indexed(3);
    cmd.end_pass();
    timestamp(cmd, kTsTonemap);

    if (fxaa) {
        fullscreen(target, rhi::LoadOp::DontCare, "fxaa");
        cmd.bind_pipeline(fxaa_);
        cmd.set_uniforms(rhi::bank::kFrame, rhi::as_bytes(fu));
        cmd.bind_texture(rhi::bank::kAlbedo, ldr_, linear_clamp_);
        cmd.draw_non_indexed(3);
        cmd.end_pass();
        stats_.fxaa = true;
    }
    timestamp(cmd, kTsFxaa);
}

}  // namespace mce::render
