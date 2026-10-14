#include "mce/render/forward3d.h"

#include <cmath>

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

struct PassU {
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
};

struct ProbeU {  // kProbeReprojectFS
    mat4 face_vp[6];
    vec4 pos;
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
    vec4 skin;      // x: first palette joint, -1 = rigid
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

enum Stage : u32 { kTsBegin, kTsClusters, kTsShadow, kTsMain, kTsResolve, kTsBloom, kTsTonemap, kTsFxaa };

// The sky's PassU members; returns the texture to bind at bank::kSky.
rhi::TextureHandle sky_params(const TextureStore& textures, rhi::RenderDevice& device, const Sky& sky, PassU& pu) {
    rhi::TextureHandle sky_tex = textures.white();
    f32 sky_lod = -1.0f;
    if (sky.texture != ~0u && textures.contains(sky.texture)) {
        sky_tex = textures.get(sky.texture);
        sky_lod = f32(max<u32>(device.texture_desc(sky_tex).mip_levels, 1u) - 1u);
    }
    pu.sky_zenith = vec4(sky.zenith, sky.enabled ? 1.0f : 0.0f);
    pu.sky_horizon = vec4(sky.horizon, sky.specular_ambient);
    pu.sky_ground = vec4(sky.ground, sky_lod);
    pu.sun = {sky.sun_size, sky.sun_intensity, sky.texture_intensity, sky.texture_additive ? 1.0f : 0.0f};
    return sky_tex;
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
    td.array_layers = kMaxCascades;
    td.debug_name = "mce.3d.shadow_cascades";
    shadow_map_ = device.create_texture(td);
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
    }
    return ok();
}

void Forward3D::destroy_targets() {
    if (!device_) return;
    for (rhi::TextureHandle* t : {&hdr_msaa_, &depth_, &hdr_, &ldr_}) destroy(*device_, *t);
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
          &ibl_prefilter_, &ibl_sh_pipe_, &ibl_brdf_, &probe_reproject_, &probe_prefilter_}) {
        if (!p->is_null()) device_->destroy_pipeline(*p);
        *p = {};
    }
    for (rhi::TextureHandle* t : {&ibl_env_, &ibl_lut_, &ibl_sh_, &probe_faces_, &probe_depth_, &probe_capture_})
        destroy(*device_, *t);
    ibl_key_ = 0;
    ibl_env_layers_ = 0;
    for (u64& k : probe_keys_) k = 0;
    for (rhi::SamplerHandle* s : {&material_sampler_, &shadow_sampler_, &point_sampler_, &linear_clamp_, &env_sampler_}) {
        if (!s->is_null()) device_->destroy_sampler(*s);
        *s = {};
    }
    destroy(*device_, shadow_map_);
    for (rhi::BufferHandle* b : {&cluster_ranges_, &cluster_indices_}) {
        if (!b->is_null()) device_->destroy_buffer(*b);
        *b = {};
    }
    cluster_ranges_cap_ = cluster_indices_cap_ = 0;
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
    t.shadow = ts.ms(b + kTsClusters, b + kTsShadow);
    t.main = ts.ms(b + kTsShadow, b + kTsMain);
    t.resolve = ts.ms(b + kTsMain, b + kTsResolve);
    t.bloom = ts.ms(b + kTsResolve, b + kTsBloom);
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
    // The joint palette stays bound for the shadow and main passes (one copy per frame).
    const Span<const mat4> palette = scene.joint_palette();
    if (!palette.empty()) cmd.set_storage_data(rhi::bank::kJointPalette, std::as_bytes(palette));
    stats_.joints = static_cast<u32>(palette.size());
    record_ibl(cmd, scene);     // rare (sky changes), so outside the per-stage budget
    record_probes(cmd, scene);  // rare too (probe placed, moved or version bumped)
    timestamp(cmd, kTsBegin);
    record_clusters(cmd, scene);
    timestamp(cmd, kTsClusters);
    record_shadows(cmd, scene);
    timestamp(cmd, kTsShadow);
    record_main(cmd, scene);
    timestamp(cmd, kTsMain);
    if (desc_.msaa > 1) cmd.resolve(hdr_msaa_, hdr_);
    timestamp(cmd, kTsResolve);
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
    // The sky's layers, then the probe slots once a scene has probes (grown once, never shrunk).
    const u32 layers = kIblLayers + (scene.reflection_probes().empty() ? 0u : kMaxReflectionProbes * kProbeLayers);
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
    PassU pu{};
    const rhi::TextureHandle sky_tex = sky_params(*textures_, *device_, sky, pu);
    const u64 key = sky_radiance_key(sky, sky_tex, pu.sky_ground.w);
    if (key != ibl_key_) {
        cmd.bind_texture(rhi::bank::kSky, sky_tex, material_sampler_);
        for (u32 i = 0; i < kIblLayers; ++i) {
            pass_into(ibl_env_, static_cast<u16>(i), "ibl.prefilter");
            cmd.bind_pipeline(ibl_prefilter_);
            pu.ibl = {0.0f, 0.0f, f32(kIblLayers), f32(i + 1) / f32(kIblLayers)};
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
    for (u32 i = 0; i < probes.size(); ++i) {
        keys[i] = probe_key(probes[i], ibl_key_);
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
    const rhi::TextureHandle sky_tex = sky_params(*textures_, *device_, sky, pu);
    pu.ibl = {1.0f, sky.diffuse_ambient, f32(kIblLayers), 0.0f};  // the sky's IBL, no probes
    static constexpr ClusterRange kNoRange{0, 0};
    static constexpr GpuPointLight kNoLight{};
    static constexpr u32 kNoIndex = 0;
    const Span<const Scene3D::Draw> draws = scene.probe_draws();

    cmd.push_debug_scope("probes");
    for (u32 i = 0; i < probes.size(); ++i) {
        if (keys[i] == probe_keys_[i]) continue;
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
            cmd.set_storage_data(rhi::bank::kClusters, rhi::as_bytes(kNoRange));
            cmd.set_storage_data(rhi::bank::kLightIndices, rhi::as_bytes(kNoIndex));
            cmd.set_storage_data(rhi::bank::kLightPool, rhi::as_bytes(kNoLight));
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
        for (u32 k = 0; k < c.caster_count; ++k) {
            const Scene3D::Draw& d = casters[lists[c.first_caster + k]];
            if (d.mesh->vertices.is_null()) continue;
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
            const ShadowU su{c.view_proj * d.model, d.dequant, {palette_index(d.palette), 0.0f, 0.0f, 0.0f}};
            cmd.set_uniforms(rhi::bank::kObject, rhi::as_bytes(su));
            cmd.draw(d.index_count, 1, d.first_index, 0, static_cast<i32>(d.base_vertex));
            ++stats_.shadow_draws;
        }
        cmd.end_pass();
    }
    stats_.cascades = static_cast<u32>(cascades.size());
    cmd.pop_debug_scope();
}

// ---- 2. Forward+ PBR, sky, blended
void Forward3D::record_main(rhi::CommandBuffer& cmd, const Scene3D& scene) {
    cmd.push_debug_scope("forward");
    const Fog& fog = scene.fog();
    const Sky& sky = scene.sky();
    const View3D& view = scene.view();
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

    // The GPU path never runs the CPU assignment: the setup (pool + grid) is all the frame reads.
    const ClusterLights& cl = stats_.gpu_clusters ? scene.cluster_setup() : scene.clusters();
    const PostFx& post = scene.post();
    FrameU fu;
    fu.view_proj = view.view_proj;
    fu.view = view.view;
    fu.inv_view_proj = inverse(view.view_proj);
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
    const rhi::TextureHandle sky_tex = sky_params(*textures_, *device_, sky, pu);
    if (stats_.ibl) {
        pu.ibl = {1.0f, sky.diffuse_ambient, f32(kIblLayers), 0.0f};
        cmd.bind_texture(rhi::bank::kIblEnv, ibl_env_, env_sampler_);
        cmd.bind_texture(rhi::bank::kIblBrdfLut, ibl_lut_, linear_clamp_);
        cmd.bind_texture(rhi::bank::kIblSh, ibl_sh_, point_sampler_);
        const Span<const ReflectionProbe> probes = scene.reflection_probes();
        if (stats_.probes > 0) {
            pu.probes.x = f32(probes.size());
            for (u32 i = 0; i < probes.size(); ++i) {
                const ReflectionProbe& p = probes[i];
                pu.probe_pos[i] = vec4(p.position, p.intensity);
                pu.probe_min[i] = vec4(p.bounds.min, max(p.blend_distance, 1e-3f));
                pu.probe_max[i] = vec4(p.bounds.max, f32(probe_base_layer(i)));
            }
        }
    }
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

    DrawCache cache;
    const Span<const Scene3D::Draw> draws = scene.draws();
    draw_meshes(cmd, scene, draws, 0, scene.opaque_count(), scene.opaque_count(), cache, true);
    if (sky.enabled) {  // behind the opaques, before the blended draws composite over it
        cmd.bind_pipeline(sky_);
        cache.pipe = sky_;
        ++stats_.pipeline_binds;
        cmd.draw_non_indexed(3);
    }
    draw_meshes(cmd, scene, draws, scene.opaque_count(), static_cast<u32>(draws.size()), scene.opaque_count(), cache, true);
    cmd.end_pass();
    cmd.pop_debug_scope();
}

void Forward3D::draw_meshes(rhi::CommandBuffer& cmd, const Scene3D& scene, Span<const Scene3D::Draw> draws, u32 begin,
                            u32 end, u32 opaque_end, DrawCache& cache, bool main) {
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
        const asset::MaterialChunk& mat = sm.material;
        const bool skinned = d.mesh->skinned();
        const rhi::PipelineHandle pipe = i >= opaque_end                         ? (skinned ? skinned_blend_ : mesh_blend_)
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
        bind_map(cache, 0, sm.albedo_key);
        bind_map(cache, 1, sm.normal_key);
        bind_map(cache, 2, sm.orm_key);
        bind_map(cache, 3, sm.emissive_key);
        const MeshInstance& inst = scene.instances()[d.instance];
        ObjectU ou;
        ou.model = d.model;
        ou.normal = d.normal;
        ou.uv_rect = sm.uv_rect;
        ou.base_color = {mat.base_color[0] * inst.tint.x, mat.base_color[1] * inst.tint.y,
                         mat.base_color[2] * inst.tint.z, mat.base_color[3] * inst.tint.w};
        ou.emissive = {mat.emissive[0] * inst.emissive_tint.x, mat.emissive[1] * inst.emissive_tint.y,
                       mat.emissive[2] * inst.emissive_tint.z, sm.emissive_key != ~0u ? 1.0f : 0.0f};
        ou.params = {(mat.flags & asset::MaterialFlags::AlphaMask) ? mat.alpha_cutoff : 0.0f,
                     (mat.flags & asset::MaterialFlags::Unlit) ? 1.0f : 0.0f, mat.roughness, mat.metallic};
        ou.maps = {sm.normal_key != ~0u ? 1.0f : 0.0f, sm.orm_key != ~0u ? 1.0f : 0.0f,
                   (mat.flags & asset::MaterialFlags::OcclusionInMR) ? 1.0f : 0.0f,
                   (mat.flags & asset::MaterialFlags::OcclusionOnly) ? 1.0f : 0.0f};
        ou.dequant = d.dequant;
        ou.skin = {palette_index(d.palette), 0.0f, 0.0f, 0.0f};
        cmd.set_uniforms(rhi::bank::kObject, rhi::as_bytes(ou));
        cmd.draw(d.index_count, 1, d.first_index, 0, static_cast<i32>(d.base_vertex));
        if (main) ++stats_.main_draws;
    }
}

// ---- 4-6. bloom, tonemap, FXAA
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
            const rhi::TextureHandle src = i == 0 ? hdr_ : bloom_[i - 1];
            const rhi::Extent2D ss = i == 0 ? size_ : bloom_size_[i - 1];
            fullscreen(bloom_[i], rhi::LoadOp::DontCare, "bloom.down");
            cmd.bind_pipeline(bloom_down_);
            pu.texel = {1.0f / f32(ss.width), 1.0f / f32(ss.height), 0.0f, 0.0f};
            pu.params = {max(post.bloom_threshold, 0.0f), max(post.bloom_threshold, 0.0f) * 0.5f, i == 0 ? 1.0f : 0.0f,
                         scene.exposure()};
            cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(pu));
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

    const bool fxaa = post.fxaa;
    const View3D& view = scene.view();
    FrameU fu;
    fu.view_proj = view.view_proj;
    fu.view = view.view;
    fu.inv_view_proj = mat4::identity();
    fu.camera_pos = vec4(view.position, 0.0f);
    fu.params = {scene.exposure(), desc_.encode_srgb_output ? 1.0f : 0.0f,
                 post.bloom_intensity / f32(max(bloom_levels_, 1u)), bloom ? 1.0f : 0.0f};
    fu.viewport = {f32(size_.width), f32(size_.height), 1.0f / f32(size_.width), 1.0f / f32(size_.height)};
    fullscreen(fxaa ? ldr_ : target, rhi::LoadOp::DontCare, "tonemap");
    cmd.bind_pipeline(tonemap_);
    cmd.set_uniforms(rhi::bank::kFrame, rhi::as_bytes(fu));
    // 1:1 reads texel-exact; a scaled frame is upsampled bilinearly.
    cmd.bind_texture(rhi::bank::kAlbedo, hdr_, desc_.render_scale >= 1.0f ? point_sampler_ : linear_clamp_);
    cmd.bind_texture(rhi::bank::kNormal, bloom ? bloom_[0] : hdr_, linear_clamp_);
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
