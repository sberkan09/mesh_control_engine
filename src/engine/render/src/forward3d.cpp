#include "mce/render/forward3d.h"

#include <cmath>

#include "mce/log.h"
#include "shaders3d.h"

namespace mce::render {

using namespace mce::math;

namespace {

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
    vec4 sun;          // x: cos disk, y: disk intensity, z: texture intensity
};

struct ObjectU {
    mat4 model;
    mat4 normal;
    vec4 uv_rect;
    vec4 base_color;
    vec4 emissive;  // w: emissive map
    vec4 params;    // x: alpha cutoff (0 = no mask), y: unlit, z: roughness, w: metallic
    vec4 maps;      // x: normal map, y: ORM map, z: occlusion in ORM.r, w: occlusion-only
};

struct ShadowU {
    mat4 mvp;
};

struct PostU {
    vec4 texel;
    vec4 params;  // x: threshold, y: knee, z: first pass, w: exposure
};

enum Stage : u32 { kTsBegin, kTsShadow, kTsMain, kTsResolve, kTsBloom, kTsTonemap, kTsFxaa };

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

    rhi::GraphicsPipelineDesc sp;
    sp.vertex_shader = {kShadowVS, "shadow.vert"};
    sp.fragment_shader = {kShadowFS, "shadow.frag"};
    sp.vertex = layout;
    sp.state.depth = {true, true, rhi::CompareOp::LessEqual};
    sp.state.raster.cull = rhi::CullMode::None;  // kit meshes are often single-sided shells
    sp.debug_name = "mesh.shadow";
    MCE_TRY_ASSIGN(shadow_pipe_, device_->create_pipeline(sp));

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
         {&shadow_pipe_, &mesh_cull_, &mesh_two_sided_, &mesh_blend_, &sky_, &bloom_down_, &bloom_up_, &tonemap_, &fxaa_}) {
        if (!p->is_null()) device_->destroy_pipeline(*p);
        *p = {};
    }
    for (rhi::SamplerHandle* s : {&material_sampler_, &shadow_sampler_, &point_sampler_, &linear_clamp_}) {
        if (!s->is_null()) device_->destroy_sampler(*s);
        *s = {};
    }
    destroy(*device_, shadow_map_);
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
    t.shadow = ts.ms(b + kTsBegin, b + kTsShadow);
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
    const rhi::Extent2D size = scene.view().viewport;
    if (size.width == 0 || size.height == 0) return;
    ensure_targets(size);
    timestamp(cmd, kTsBegin);
    record_shadows(cmd, scene);
    timestamp(cmd, kTsShadow);
    record_main(cmd, scene);
    timestamp(cmd, kTsMain);
    if (desc_.msaa > 1) cmd.resolve(hdr_msaa_, hdr_);
    timestamp(cmd, kTsResolve);
    record_post(cmd, scene, target);
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
        cmd.bind_pipeline(shadow_pipe_);
        const GpuMesh* bound = nullptr;
        for (u32 k = 0; k < c.caster_count; ++k) {
            const Scene3D::Draw& d = casters[lists[c.first_caster + k]];
            if (d.mesh->vertices.is_null()) continue;
            if (d.mesh != bound) {
                cmd.bind_vertex_buffer(d.mesh->vertices);
                cmd.bind_index_buffer(d.mesh->indices, d.mesh->index_format);
                bound = d.mesh;
            }
            const GpuSubmesh& sm = d.mesh->submeshes[d.submesh];
            const ShadowU su{c.view_proj * d.model};
            cmd.set_uniforms(rhi::bank::kObject, rhi::as_bytes(su));
            cmd.draw(sm.index_count, 1, sm.first_index, 0, static_cast<i32>(sm.base_vertex));
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

    const ClusterLights& cl = scene.clusters();
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
    u32 sky_key = sky.texture;
    rhi::TextureHandle sky_tex = textures_->white();
    f32 sky_lod = -1.0f;
    if (sky_key != ~0u && textures_->contains(sky_key)) {
        sky_tex = textures_->get(sky_key);
        sky_lod = f32(max<u32>(device_->texture_desc(sky_tex).mip_levels, 1u) - 1u);
    }
    pu.sky_zenith = vec4(sky.zenith, sky.enabled ? 1.0f : 0.0f);
    pu.sky_horizon = vec4(sky.horizon, sky.specular_ambient);
    pu.sky_ground = vec4(sky.ground, sky_lod);
    pu.sun = {sky.sun_size, sky.sun_intensity, sky.texture_intensity, 0.0f};
    cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(pu));
    cmd.bind_texture(rhi::bank::kShadowMap, shadow_map_, shadow_sampler_);
    cmd.bind_texture(rhi::bank::kSky, sky_tex, material_sampler_);

    // Forward+ lists (banks 4-6). Never empty: an unbound SSBO is undefined even when unread.
    static constexpr ClusterRange kNoRange{0, 0};
    static constexpr GpuPointLight kNoLight{};
    static constexpr u32 kNoIndex = 0;
    cmd.set_storage_data(rhi::bank::kClusters, cl.ranges().empty() ? rhi::as_bytes(kNoRange) : std::as_bytes(cl.ranges()));
    cmd.set_storage_data(rhi::bank::kLightPool, cl.lights().empty() ? rhi::as_bytes(kNoLight) : std::as_bytes(cl.lights()));
    cmd.set_storage_data(rhi::bank::kLightIndices, cl.indices().empty() ? rhi::as_bytes(kNoIndex) : std::as_bytes(cl.indices()));
    stats_.point_lights = static_cast<u32>(cl.lights().size());
    stats_.light_indices = static_cast<u32>(cl.indices().size());

    const rhi::TextureHandle white = textures_->white();
    rhi::PipelineHandle bound_pipe;
    const GpuMesh* bound_mesh = nullptr;
    u32 bound_tex[4] = {~0u - 1, ~0u - 1, ~0u - 1, ~0u - 1};
    static constexpr u32 kSlots[4] = {rhi::bank::kAlbedo, rhi::bank::kNormal, rhi::bank::kOrm, rhi::bank::kEmissive};
    auto bind_map = [&](u32 i, u32 key) {
        if (key == bound_tex[i]) return;
        const rhi::TextureHandle tex = key == ~0u ? white : textures_->get(key);
        cmd.bind_texture(kSlots[i], tex, material_sampler_);
        bound_tex[i] = key;
        ++stats_.texture_binds;
    };
    const Span<const Scene3D::Draw> draws = scene.draws();
    auto draw_range = [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            const Scene3D::Draw& d = draws[i];
            if (d.mesh->vertices.is_null()) continue;
            const GpuSubmesh& sm = d.mesh->submeshes[d.submesh];
            const asset::MaterialChunk& mat = sm.material;
            const rhi::PipelineHandle pipe = i >= scene.opaque_count()                         ? mesh_blend_
                                             : (mat.flags & asset::MaterialFlags::DoubleSided) ? mesh_two_sided_
                                                                                               : mesh_cull_;
            if (!(pipe == bound_pipe)) {
                cmd.bind_pipeline(pipe);
                bound_pipe = pipe;
                ++stats_.pipeline_binds;
            }
            if (d.mesh != bound_mesh) {
                cmd.bind_vertex_buffer(d.mesh->vertices);
                cmd.bind_index_buffer(d.mesh->indices, d.mesh->index_format);
                bound_mesh = d.mesh;
            }
            bind_map(0, sm.albedo_key);
            bind_map(1, sm.normal_key);
            bind_map(2, sm.orm_key);
            bind_map(3, sm.emissive_key);
            const MeshInstance& inst = scene.instances()[d.instance];
            ObjectU ou;
            ou.model = d.model;
            ou.normal = d.normal;
            ou.uv_rect = sm.uv_rect;
            ou.base_color = {mat.base_color[0] * inst.tint.x, mat.base_color[1] * inst.tint.y,
                             mat.base_color[2] * inst.tint.z, mat.base_color[3] * inst.tint.w};
            ou.emissive = {mat.emissive[0], mat.emissive[1], mat.emissive[2], sm.emissive_key != ~0u ? 1.0f : 0.0f};
            ou.params = {(mat.flags & asset::MaterialFlags::AlphaMask) ? mat.alpha_cutoff : 0.0f,
                         (mat.flags & asset::MaterialFlags::Unlit) ? 1.0f : 0.0f, mat.roughness, mat.metallic};
            ou.maps = {sm.normal_key != ~0u ? 1.0f : 0.0f, sm.orm_key != ~0u ? 1.0f : 0.0f,
                       (mat.flags & asset::MaterialFlags::OcclusionInMR) ? 1.0f : 0.0f,
                       (mat.flags & asset::MaterialFlags::OcclusionOnly) ? 1.0f : 0.0f};
            cmd.set_uniforms(rhi::bank::kObject, rhi::as_bytes(ou));
            cmd.draw(sm.index_count, 1, sm.first_index, 0, static_cast<i32>(sm.base_vertex));
            ++stats_.main_draws;
        }
    };
    draw_range(0, scene.opaque_count());
    if (sky.enabled) {  // behind the opaques, before the blended draws composite over it
        cmd.bind_pipeline(sky_);
        bound_pipe = sky_;
        ++stats_.pipeline_binds;
        cmd.draw_non_indexed(3);
    }
    draw_range(scene.opaque_count(), static_cast<u32>(draws.size()));
    cmd.end_pass();
    cmd.pop_debug_scope();
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
    cmd.bind_texture(rhi::bank::kAlbedo, hdr_, point_sampler_);
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
