// Forward3D, M-VISUAL 9 (scale): instance batches culled on the GPU (frustum, cull distance, a depth
// pyramid of the frame's other opaques, LOD) into indirect draws, their CPU fallback, projected
// decals' records, and GPU particles. The batches' main-pass and SSAO draws (ObjectU) live in
// forward3d.cpp next to draw_meshes.

#include <cmath>
#include <cstring>

#include "mce/convar.h"
#include "mce/log.h"
#include "mce/render/forward3d.h"
#include "shaders3d.h"

namespace mce::render {

using namespace mce::math;

namespace {

MCE_CVAR(cv_gpu_culling, "r.gpu_culling", true,
         "Cull instance batches in a compute pass with indirect draws (off = CPU frustum culling)", CVarFlags::None);
MCE_CVAR(cv_occlusion, "r.occlusion", true, "Occlusion-cull instance batches against the frame's depth pyramid",
         CVarFlags::None);

// std140 mirrors (shaders3d.h, scale section): vec4/mat4 members only.
struct InstDrawU {
    mat4 view_proj;
    mat4 local;
    vec4 info;  // x: first visible slot, y: time, z: previous time, w: alpha cutoff
    vec4 wind;
    vec4 uv_rect;
    vec4 base;  // x: base color alpha
};
struct OccU {
    mat4 mvp;
    mat4 mv;
};
struct HizU {
    vec4 size;  // source w, h, this level's w, h
};
struct InitU {
    u32 count[4];
};
struct CullU {
    mat4 view_proj;
    vec4 planes[6];
    vec4 eye;
    vec4 forward;
    vec4 sphere;
    vec4 batch;
    vec4 lod;
    vec4 lod_error;
    vec4 lod_px;
    vec4 hiz;
};
struct SimU {
    vec4 spawn;
    vec4 step;
    vec4 origin;
    vec4 box;
    vec4 dir;
    vec4 ranges;
    vec4 gravity;
};
struct SpriteU {
    vec4 size;
    vec4 start;
    vec4 end;
    vec4 info;
};
struct DecalRecord {  // std430 row of bank kDecals (kMeshFS DecalRec)
    mat4 inv_world;
    vec4 uv_rect;
    vec4 color;
    vec4 params;  // roughness, metallic, material weight, emissive
    vec4 fade;    // angle fade, edge fade, texture slot (-1 = none)
    vec4 axis;
};
static_assert(sizeof(DecalRecord) == 144);

constexpr u32 kCmdWords = sizeof(rhi::DrawIndexedIndirectCommand) / sizeof(u32);
constexpr f32 kOccluderFar = 1e9f;  // the occluder target's clear: nothing in front

u32 groups(u32 n, u32 size) { return (n + size - 1) / size; }

f32 uint_bits(u32 v) {
    f32 f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}

void destroy_buffer(rhi::RenderDevice& d, rhi::BufferHandle& b) {
    if (!b.is_null()) d.destroy_buffer(b);
    b = {};
}

void destroy_texture(rhi::RenderDevice& d, rhi::TextureHandle& t) {
    if (!t.is_null()) d.destroy_texture(t);
    t = {};
}

}  // namespace

ResultV Forward3D::create_scale_pipelines() {
    using namespace shaders3d;
    static constexpr rhi::VertexAttribute kAttrs[] = {
        {0, rhi::VertexFormat::SNorm16x4, offsetof(asset::MeshVertex, position)},
        {1, rhi::VertexFormat::SNorm16x2, offsetof(asset::MeshVertex, normal)},
        {2, rhi::VertexFormat::UNorm16x2, offsetof(asset::MeshVertex, uv)},
    };
    const rhi::VertexLayout layout{kAttrs, sizeof(asset::MeshVertex)};
    rhi::GraphicsPipelineDesc pd;
    pd.vertex_shader = {kInstancedMeshVS, "pbr_instanced.vert"};
    pd.fragment_shader = {kMeshFS, "pbr.frag"};
    pd.vertex = layout;
    pd.state.depth = {true, true, rhi::CompareOp::LessEqual};
    pd.state.raster.cull = rhi::CullMode::Back;
    pd.debug_name = "pbr.instanced.opaque";
    MCE_TRY_ASSIGN(inst_cull_, device_->create_pipeline(pd));
    pd.state.raster.cull = rhi::CullMode::None;
    pd.debug_name = "pbr.instanced.two_sided";
    MCE_TRY_ASSIGN(inst_two_sided_, device_->create_pipeline(pd));
    if (!ao_prepass_.is_null()) {
        pd.fragment_shader = {kAoPrepassFS, "ssao_prepass.frag"};
        pd.debug_name = "ssao.prepass.instanced";
        MCE_TRY_ASSIGN(inst_prepass_, device_->create_pipeline(pd));
    }
    rhi::GraphicsPipelineDesc sp;
    sp.vertex_shader = {kInstancedShadowVS, "shadow_instanced.vert"};
    sp.fragment_shader = {kInstancedShadowFS, "shadow_instanced.frag"};
    sp.vertex = layout;
    sp.state.depth = {true, true, rhi::CompareOp::LessEqual};
    sp.state.raster.cull = rhi::CullMode::None;
    sp.debug_name = "mesh.shadow.instanced";
    MCE_TRY_ASSIGN(inst_shadow_, device_->create_pipeline(sp));
    if (!velocity_pipe_.is_null()) {
        sp.vertex_shader = {kInstancedVelocityVS, "velocity_instanced.vert"};
        sp.fragment_shader = {kInstancedVelocityFS, "velocity_instanced.frag"};
        sp.debug_name = "taa.velocity.instanced";
        MCE_TRY_ASSIGN(inst_velocity_, device_->create_pipeline(sp));
    }
    if (!device_->caps().compute) return {};
    // The GPU path; a driver that refuses one of its shaders keeps the CPU culling (and no particles).
    rhi::GraphicsPipelineDesc op;
    op.vertex_shader = {kOccluderVS, "occluder.vert"};
    op.fragment_shader = {kOccluderFS, "occluder.frag"};
    op.vertex = layout;
    op.state.depth = {true, true, rhi::CompareOp::LessEqual};
    op.state.raster.cull = rhi::CullMode::None;
    op.debug_name = "scale.occluders";
    auto compute = [&](const char* src, const char* name, rhi::PipelineHandle& out) {
        rhi::ComputePipelineDesc cd;
        cd.compute_shader = {src, name};
        cd.debug_name = name;
        if (auto p = device_->create_compute_pipeline(cd)) out = p.value();
        else {
            FixedString<512> msg;
            format_error(p.error(), msg);
            MCE_LOG_WARN("render3d: {} unavailable: {}", name, msg.c_str());
        }
        return !out.is_null();
    };
    if (auto p = device_->create_pipeline(op)) occluder_pipe_ = p.value();
    const bool culling = !occluder_pipe_.is_null() && compute(kHizReduceCS, "mce.3d.hiz_reduce", hiz_reduce_) &&
                         compute(kInstanceArgsInitCS, "mce.3d.instance_args", args_init_) &&
                         compute(kInstanceCullCS, "mce.3d.instance_cull", cull_cs_);
    if (!culling) MCE_LOG_WARN("render3d: GPU instance culling unavailable, using the CPU path");
    if (compute(kParticleSimCS, "mce.3d.particle_sim", particle_sim_)) {
        rhi::GraphicsPipelineDesc pp;
        pp.vertex_shader = {kParticleVS, "particles.vert"};
        pp.fragment_shader = {kParticleFS, "particles.frag"};
        pp.state.depth = {true, false, rhi::CompareOp::LessEqual};
        pp.state.blend.mode = rhi::BlendMode::Additive;
        pp.debug_name = "particles.additive";
        MCE_TRY_ASSIGN(particle_add_, device_->create_pipeline(pp));
        pp.state.blend.mode = rhi::BlendMode::PremultipliedAlpha;
        pp.debug_name = "particles.alpha";
        MCE_TRY_ASSIGN(particle_alpha_, device_->create_pipeline(pp));
    }
    return {};
}

void Forward3D::destroy_scale() {
    for (rhi::PipelineHandle* p : {&inst_cull_, &inst_two_sided_, &inst_prepass_, &inst_shadow_, &inst_velocity_, &occluder_pipe_,
                                   &hiz_reduce_, &args_init_, &cull_cs_, &particle_sim_, &particle_add_, &particle_alpha_}) {
        if (!p->is_null()) device_->destroy_pipeline(*p);
        *p = {};
    }
    for (BatchGpu& g : batch_gpu_) destroy_buffer(*device_, g.transforms);
    batch_gpu_.clear();
    for (EmitterGpu& e : emitters_gpu_) destroy_buffer(*device_, e.particles);
    emitters_gpu_.clear();
    destroy_buffer(*device_, inst_args_);
    destroy_buffer(*device_, inst_visible_);
    inst_args_cap_ = inst_visible_cap_ = 0;
    destroy_texture(*device_, occ_color_);
    destroy_texture(*device_, occ_depth_);
    for (rhi::TextureHandle& t : hiz_) destroy_texture(*device_, t);
    for (rhi::Extent2D& e : hiz_size_) e = {};
    batch_views_.clear();
    batch_gpu_index_.clear();
    batch_view_count_ = batch_count_ = 0;
}

u32 Forward3D::batch_command(u32 view, u32 batch, u32 level, u32 submesh) const {
    if (!batches_gpu_ || view >= batch_view_count_ || batch >= batch_count_) return ~0u;
    const BatchView& bv = batch_views_[view * batch_count_ + batch];
    if (!bv.culled || level >= asset::kMaxMeshLods || submesh >= bv.submeshes) return ~0u;
    return bv.first_command + level * bv.submeshes + submesh;
}

rhi::BufferHandle Forward3D::particle_buffer(u32 emitter_id) const {
    for (const EmitterGpu& e : emitters_gpu_)
        if (e.id == emitter_id) return e.particles;
    return {};
}

// ---- the frame's batches: matrices to the GPU, then culling for the view and each cascade
void Forward3D::record_scale(rhi::CommandBuffer& cmd, const Scene3D& scene) {
    const Span<const InstanceBatch> batches = scene.batches();
    batch_count_ = static_cast<u32>(batches.size());
    batch_views_.clear();
    batch_gpu_index_.clear();
    batch_view_count_ = 0;
    batches_gpu_ = false;
    // 1. Matrices: kept per batch id, uploaded when the batch changed; batches no view drew last
    // frame freed by the frame's first view (M-VIEWS: a batch another view draws stays resident).
    if (out_.first) ++batch_frame_;
    for (BatchGpu& g : batch_gpu_) g.seen = false;
    for (const InstanceBatch& b : batches) {
        BatchGpu* g = nullptr;
        for (BatchGpu& e : batch_gpu_)
            if (e.id == b.id && !e.seen) g = &e;
        if (!g) {
            batch_gpu_.push_back({});
            g = &batch_gpu_.back();
            g->id = b.id;
        }
        g->seen = true;
        g->frame = batch_frame_;
    }
    for (u32 i = 0; i < batch_gpu_.size();) {
        if (batch_gpu_[i].seen || !out_.first || batch_gpu_[i].frame + 1 >= batch_frame_) {
            ++i;
            continue;
        }
        destroy_buffer(*device_, batch_gpu_[i].transforms);
        batch_gpu_[i] = batch_gpu_.back();
        batch_gpu_.pop_back();
    }
    for (BatchGpu& g : batch_gpu_) g.seen = false;
    for (const InstanceBatch& b : batches) {
        u32 gi = 0;
        while (batch_gpu_[gi].id != b.id || batch_gpu_[gi].seen) ++gi;  // present: added above
        BatchGpu& g = batch_gpu_[gi];
        g.seen = true;
        batch_gpu_index_.push_back(gi);
        const GpuMesh* mesh = scene.meshes().get(b.mesh);
        const u32 n = static_cast<u32>(b.transforms.size());
        bool upload = g.transforms.is_null() || g.version != b.version || g.count != n || g.mesh != mesh ||
                      g.mesh_generation != mesh->generation;
        if (n > g.capacity) {
            destroy_buffer(*device_, g.transforms);
            g.capacity = max(n, 64u);
            g.transforms = device_->create_buffer({rhi::BufferUsage::Storage, usize(g.capacity) * sizeof(mat4), "mce.3d.batch"});
            upload = true;
        }
        if (upload) device_->update_buffer(g.transforms, 0, std::as_bytes(b.transforms));
        g.version = b.version;
        g.count = n;
        g.mesh = mesh;
        g.mesh_generation = mesh->generation;
    }
    stats_.batches = batch_count_;
    if (batch_count_ > 0) {
        // 2. Views: the main view, then each cascade when a batch casts shadows.
        const Span<const Cascade> cascades = scene.cascades();
        bool casters = false;
        for (const InstanceBatch& b : batches) casters = casters || b.cast_shadows;
        batch_view_count_ = 1 + (casters ? static_cast<u32>(cascades.size()) : 0u);
        batch_views_.resize(usize(batch_view_count_) * batch_count_);
        stats_.batch_views = batch_view_count_;
        const InstanceCulling& culling = scene.instance_culling();
        batches_gpu_ = culling.gpu && cv_gpu_culling.get_bool() && !cull_cs_.is_null();
        stats_.batches_gpu = batches_gpu_;
        const View3D& view = scene.view();
        const LodSettings& lod = scene.lod();
        auto view_vp = [&](u32 v) -> const mat4& { return v == 0 ? view.view_proj : cascades[v - 1].view_proj; };
        if (batches_gpu_) {
            // Layout: per (view, batch) the commands of every level × submesh and a visible list per level.
            u32 commands = 0, slots = 0;
            for (u32 v = 0; v < batch_view_count_; ++v) {
                for (u32 bi = 0; bi < batch_count_; ++bi) {
                    const InstanceBatch& b = batches[bi];
                    BatchView& bv = batch_views_[v * batch_count_ + bi];
                    const GpuMesh& mesh = *scene.meshes().get(b.mesh);
                    bv.submeshes = static_cast<u32>(mesh.submeshes.size());
                    if (v > 0 && !b.cast_shadows) continue;
                    const u32 levels = max(mesh.lod_levels, 1u), n = static_cast<u32>(b.transforms.size());
                    bv.culled = true;
                    bv.first_command = commands;
                    commands += levels * bv.submeshes;
                    for (u32 l = 0; l < levels; ++l) bv.slot[l] = slots + l * n;
                    slots += levels * n;
                }
            }
            if (commands > inst_args_cap_) {
                destroy_buffer(*device_, inst_args_);
                inst_args_cap_ = max(commands * 2, 64u);
                inst_args_ = device_->create_buffer(
                    {rhi::BufferUsage::IndirectArgs, usize(inst_args_cap_) * sizeof(rhi::DrawIndexedIndirectCommand), "mce.3d.batch_args"});
            }
            if (slots > inst_visible_cap_) {
                destroy_buffer(*device_, inst_visible_);
                inst_visible_cap_ = max(slots + slots / 2, 1024u);
                inst_visible_ = device_->create_buffer({rhi::BufferUsage::Storage, usize(inst_visible_cap_) * sizeof(u32), "mce.3d.batch_visible"});
            }
            Array<rhi::DrawIndexedIndirectCommand> templ(mem::heap());
            templ.resize(commands);
            for (u32 v = 0; v < batch_view_count_; ++v) {
                for (u32 bi = 0; bi < batch_count_; ++bi) {
                    const BatchView& bv = batch_views_[v * batch_count_ + bi];
                    if (!bv.culled) continue;
                    const GpuMesh& mesh = *scene.meshes().get(batches[bi].mesh);
                    const u32 levels = max(mesh.lod_levels, 1u);
                    for (u32 l = 0; l < levels; ++l) {
                        for (u32 s = 0; s < bv.submeshes; ++s) {
                            const GpuLod& r = mesh.lod(s, l);
                            templ[bv.first_command + l * bv.submeshes + s] = {r.index_count, 0, r.first_index, i32(r.base_vertex), 0};
                        }
                    }
                }
            }
            cmd.push_debug_scope("batch_cull");
            record_occluders(cmd, scene);
            const bool hiz = stats_.occlusion;
            // The commands start from the template (zero instances), then every (view, batch) culls.
            cmd.bind_pipeline(args_init_);
            const InitU iu{{commands * kCmdWords, 0, 0, 0}};
            cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(iu));
            cmd.set_storage_data(rhi::bank::kInstances, std::as_bytes(templ.span()));
            cmd.bind_storage_buffer(rhi::bank::kInstanceArgs, inst_args_, 0, 0);
            cmd.dispatch(groups(commands * kCmdWords, 64));
            cmd.barrier(rhi::barrier::kStorageBuffer);
            cmd.bind_pipeline(cull_cs_);
            cmd.bind_storage_buffer(rhi::bank::kInstanceVisible, inst_visible_, 0, 0);
            cmd.bind_storage_buffer(rhi::bank::kInstanceArgs, inst_args_, 0, 0);
            for (u32 k = 0; k < kHizLevels; ++k) {
                const rhi::TextureHandle t = hiz ? hiz_level(k) : ao_off_;
                cmd.bind_texture(k, t, point_sampler_);
            }
            for (u32 v = 0; v < batch_view_count_; ++v) {
                vec4 planes[6];
                frustum_planes(view_vp(v), planes);
                for (u32 bi = 0; bi < batch_count_; ++bi) {
                    const BatchView& bv = batch_views_[v * batch_count_ + bi];
                    if (!bv.culled) continue;
                    const InstanceBatch& b = batches[bi];
                    const GpuMesh& mesh = *scene.meshes().get(b.mesh);
                    CullU cu{};
                    cu.view_proj = view_vp(v);
                    for (u32 p = 0; p < 6; ++p) cu.planes[p] = planes[p];
                    cu.eye = vec4(view.position, b.cull_distance);
                    cu.forward = vec4(view.forward, view.near_z);
                    cu.sphere = vec4(mesh.bounds.center(), length(mesh.bounds.extents()));
                    cu.batch = {f32(b.transforms.size()), f32(bv.slot[0]), f32(bv.first_command), f32(bv.submeshes)};
                    const u32 levels = max(mesh.lod_levels, 1u);
                    const f32 forced = lod.force_level != ~0u ? f32(min(lod.force_level, levels - 1)) : (lod.enabled ? -1.0f : 0.0f);
                    cu.lod = {f32(levels), view.proj.c[1][1] * 0.5f * f32(view.viewport.height), v > 0 ? f32(lod.shadow_bias) : 0.0f, forced};
                    for (u32 l = 0; l < asset::kMaxMeshLods; ++l) cu.lod_error[l] = mesh.lod_error[l];
                    cu.lod_px = {lod.pixel_error, 0.0f, 0.0f, 0.0f};
                    cu.hiz = {v == 0 && hiz ? 1.0f : 0.0f, f32(hiz_size_[0].width), f32(hiz_size_[0].height), f32(kHizLevels)};
                    cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(cu));
                    cmd.bind_storage_buffer(rhi::bank::kInstances, batch_gpu_[batch_gpu_index_[bi]].transforms, 0, 0);
                    cmd.dispatch(groups(static_cast<u32>(b.transforms.size()), 64));
                }
            }
            cmd.barrier(rhi::barrier::kIndirect | rhi::barrier::kStorageBuffer);
            cmd.pop_debug_scope();
        } else {
            // The CPU path: the same tests (without the pyramid), one list per (view, batch, level).
            Array<u32> visible(mem::heap());
            Array<BatchVisible> found(mem::heap());
            for (u32 v = 0; v < batch_view_count_; ++v) {
                vec4 planes[6];
                frustum_planes(view_vp(v), planes);
                for (u32 bi = 0; bi < batch_count_; ++bi) {
                    const InstanceBatch& b = batches[bi];
                    BatchView& bv = batch_views_[v * batch_count_ + bi];
                    const GpuMesh& mesh = *scene.meshes().get(b.mesh);
                    bv.submeshes = static_cast<u32>(mesh.submeshes.size());
                    if (v > 0 && !b.cast_shadows) continue;
                    bv.culled = true;
                    found.clear();
                    cull_batch(b, mesh, planes, view, lod, v > 0 ? lod.shadow_bias : 0u, found);
                    for (u32 l = 0; l < asset::kMaxMeshLods; ++l) {
                        bv.slot[l] = static_cast<u32>(visible.size());
                        for (const BatchVisible& f : found)
                            if (f.level == l) visible.push_back(f.instance);
                        bv.count[l] = static_cast<u32>(visible.size()) - bv.slot[l];
                        if (v == 0) stats_.batch_visible += bv.count[l];
                    }
                }
            }
            if (visible.empty()) visible.push_back(0);  // never an empty binding
            cmd.set_storage_data(rhi::bank::kInstanceVisible, std::as_bytes(visible.span()));
        }
    }
    if (out_.first) record_particles_sim(cmd, scene);  // later views of the frame draw the rings as they are
}

// The occluders' view depth, then the pyramid: each level the farthest depth under its texels.
void Forward3D::record_occluders(rhi::CommandBuffer& cmd, const Scene3D& scene) {
    stats_.occlusion = false;
    const InstanceCulling& culling = scene.instance_culling();
    if (!culling.occlusion || !cv_occlusion.get_bool() || hiz_reduce_.is_null()) return;
    const Span<const Scene3D::Draw> draws = scene.draws();
    u32 occluders = 0;
    auto occluder = [&](const Scene3D::Draw& d) {
        if (d.mesh->vertices.is_null() || d.mesh->skinned()) return false;
        const asset::MaterialChunk& mat = material_of(scene.instances()[d.instance], d.mesh->submeshes[d.submesh]);
        return !(mat.flags & (asset::MaterialFlags::AlphaMask | asset::MaterialFlags::AlphaBlend));
    };
    for (u32 i = 0; i < scene.opaque_count(); ++i) occluders += occluder(draws[i]) ? 1u : 0u;
    if (occluders == 0) return;
    const u32 div = clamp(culling.occluder_divisor, 1u, 8u);
    const rhi::Extent2D size{max(1u, size_.width / div), max(1u, size_.height / div)};
    if (!(size == hiz_size_[0]) || occ_color_.is_null()) {
        destroy_texture(*device_, occ_color_);
        destroy_texture(*device_, occ_depth_);
        for (rhi::TextureHandle& t : hiz_) destroy_texture(*device_, t);
        hiz_size_[0] = size;
        rhi::TextureDesc cd;
        cd.size = size;
        cd.format = rhi::TextureFormat::RGBA32F;
        cd.usage = rhi::TextureUsage::SampledColorTarget;
        cd.debug_name = "mce.3d.occluders";
        occ_color_ = device_->create_texture(cd);
        rhi::TextureDesc dd;
        dd.size = size;
        dd.format = rhi::TextureFormat::D32_Float;
        dd.usage = rhi::TextureUsage::DepthTarget;
        dd.debug_name = "mce.3d.occluders_depth";
        occ_depth_ = device_->create_texture(dd);
        for (u32 k = 1; k < kHizLevels; ++k) {
            const rhi::Extent2D prev = hiz_size_[k - 1];
            hiz_size_[k] = {groups(prev.width, kHizRatio), groups(prev.height, kHizRatio)};
            rhi::TextureDesc hd;
            hd.size = hiz_size_[k];
            hd.format = rhi::TextureFormat::RGBA32F;
            hd.usage = rhi::TextureUsage::SampledStorage;
            hd.debug_name = "mce.3d.hiz";
            hiz_[k - 1] = device_->create_texture(hd);
        }
    }
    const View3D& view = scene.view();
    rhi::PassAttachment color;
    color.texture = occ_color_;
    for (f32& c : color.clear.color) c = kOccluderFar;
    rhi::PassAttachment depth;
    depth.texture = occ_depth_;
    depth.clear.depth = 1.0f;
    depth.store = rhi::StoreOp::DontCare;
    rhi::RenderPassDesc op;
    op.colors = Span<const rhi::PassAttachment>(&color, 1);
    op.depth = depth;
    op.debug_name = "scale.occluders";
    cmd.begin_pass(op);
    cmd.bind_pipeline(occluder_pipe_);
    const GpuMesh* bound = nullptr;
    for (u32 i = 0; i < scene.opaque_count(); ++i) {
        const Scene3D::Draw& d = draws[i];
        if (!occluder(d)) continue;
        if (d.mesh != bound) {
            cmd.bind_vertex_buffer(d.mesh->vertices);
            cmd.bind_index_buffer(d.mesh->indices, d.mesh->index_format);
            bound = d.mesh;
        }
        const OccU ou{view.view_proj * d.model, view.view * d.model};
        cmd.set_uniforms(rhi::bank::kObject, rhi::as_bytes(ou));
        cmd.draw(d.index_count, 1, d.first_index, 0, static_cast<i32>(d.base_vertex));
    }
    cmd.end_pass();
    cmd.bind_pipeline(hiz_reduce_);
    for (u32 k = 1; k < kHizLevels; ++k) {
        const rhi::Extent2D src = hiz_size_[k - 1], dst = hiz_size_[k];
        cmd.bind_texture(rhi::bank::kFirstSampler, hiz_level(k - 1), point_sampler_);
        cmd.bind_storage_image(0, hiz_[k - 1], 0, rhi::StorageAccess::Write);
        const HizU hu{{f32(src.width), f32(src.height), f32(dst.width), f32(dst.height)}};
        cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(hu));
        cmd.dispatch(groups(dst.width, 8), groups(dst.height, 8));
        cmd.barrier(rhi::barrier::kSampled | rhi::barrier::kStorageImage);
    }
    stats_.occlusion = true;
    stats_.occluder_draws = occluders;
}

bool Forward3D::batch_draw(rhi::CommandBuffer& cmd, const GpuMesh& mesh, u32 view, u32 batch, u32 level, u32 submesh) {
    const BatchView& bv = batch_views_[view * batch_count_ + batch];
    if (!bv.culled) return false;
    const GpuLod& r = mesh.lod(submesh, level);
    if (r.index_count == 0) return false;
    if (batches_gpu_) {
        cmd.draw_indirect_buffer(inst_args_, u64(bv.first_command + level * bv.submeshes + submesh) * sizeof(rhi::DrawIndexedIndirectCommand), 1);
    } else {
        if (bv.count[level] == 0) return false;
        cmd.draw(r.index_count, bv.count[level], r.first_index, 0, static_cast<i32>(r.base_vertex));
    }
    ++stats_.batch_draws;
    return true;
}

void Forward3D::bind_batch(rhi::CommandBuffer& cmd, const GpuMesh& mesh, u32 batch) {
    cmd.bind_storage_buffer(rhi::bank::kInstances, batch_gpu_[batch_gpu_index_[batch]].transforms, 0, 0);
    if (batches_gpu_) cmd.bind_storage_buffer(rhi::bank::kInstanceVisible, inst_visible_, 0, 0);
    cmd.bind_vertex_buffer(mesh.vertices);
    cmd.bind_index_buffer(mesh.indices, mesh.index_format);
}

// Shadow cascades and the velocity prepass: InstDrawU, alpha-masked like the main pass.
void Forward3D::draw_batches_depth(rhi::CommandBuffer& cmd, const Scene3D& scene, u32 view, const mat4& view_proj, bool velocity) {
    if (view >= batch_view_count_) return;
    const rhi::PipelineHandle pipe = velocity ? inst_velocity_ : inst_shadow_;
    if (pipe.is_null()) return;
    const Span<const InstanceBatch> batches = scene.batches();
    bool bound = false;
    const rhi::TextureHandle white = textures_->white();
    for (u32 bi = 0; bi < batch_count_; ++bi) {
        if (!batch_views_[view * batch_count_ + bi].culled) continue;
        const InstanceBatch& b = batches[bi];
        const GpuMesh& mesh = *scene.meshes().get(b.mesh);
        if (mesh.vertices.is_null()) continue;
        if (!bound) {
            cmd.bind_pipeline(pipe);
            bound = true;
        }
        bind_batch(cmd, mesh, bi);
        const BatchView& bv = batch_views_[view * batch_count_ + bi];
        const u32 levels = max(mesh.lod_levels, 1u);
        for (u32 s = 0; s < mesh.submeshes.size(); ++s) {
            const GpuSubmesh& sm = mesh.submeshes[s];
            const asset::MaterialChunk& mat = b.material ? b.material->params : sm.material;
            const u32 albedo = b.material ? b.material->albedo_key : sm.albedo_key;
            const bool masked = (mat.flags & asset::MaterialFlags::AlphaMask) != 0;
            cmd.bind_texture(rhi::bank::kAlbedo, masked && albedo != ~0u ? textures_->get(albedo) : white, material_sampler_);
            InstDrawU u{};
            u.view_proj = view_proj;
            u.local = mesh.rest_pose[sm.node] * sm.dequant;
            u.wind = batch_wind(b);
            u.uv_rect = tiled_uv_rect(sm.uv_rect, mat);
            u.base = {mat.base_color[3] * b.tint.w, 0.0f, 0.0f, 0.0f};
            for (u32 l = 0; l < levels; ++l) {
                u.info = {f32(bv.slot[l]), scene.time(), scene.time() - scene.dt(), masked ? mat.alpha_cutoff : 0.0f};
                cmd.set_uniforms(rhi::bank::kObject, rhi::as_bytes(u));
                if (batch_draw(cmd, mesh, view, bi, l, s)) {
                    if (velocity) ++stats_.velocity_draws;
                    else ++stats_.shadow_draws;
                }
            }
        }
    }
}

// ---- decals: the main pass's records and textures; stand-ins for the other lit passes
u32 Forward3D::bind_decals(rhi::CommandBuffer& cmd, const Scene3D& scene, bool on) {
    static constexpr DecalRecord kNone{};
    const rhi::TextureHandle white = textures_->white();
    rhi::TextureHandle tex[kMaxDecalTextures];
    bool resident[kMaxDecalTextures] = {};
    for (u32 t = 0; t < kMaxDecalTextures; ++t) tex[t] = white;
    Array<DecalRecord> records(mem::heap());
    if (on) {
        const Span<const u32> keys = scene.decal_textures();
        for (u32 t = 0; t < keys.size(); ++t) {
            resident[t] = textures_->contains(keys[t]);
            if (resident[t]) tex[t] = textures_->get(keys[t]);
        }
        const Span<const Decal> decals = scene.decals();
        const Span<const u32> slots = scene.decal_slots();
        for (u32 i = 0; i < decals.size(); ++i) {
            const Decal& d = decals[i];
            if (slots[i] != ~0u && !resident[slots[i]]) continue;  // its texture is not loaded yet
            DecalRecord r;
            r.inv_world = inverse(d.world);
            r.uv_rect = d.uv_rect;
            r.color = d.color;
            r.params = {d.roughness, d.metallic, clamp(d.material_weight, 0.0f, 1.0f), d.emissive};
            r.fade = {d.angle_fade, d.edge_fade, slots[i] == ~0u ? -1.0f : f32(slots[i]), 0.0f};
            r.axis = vec4(normalize(vec3{d.world.c[1][0], d.world.c[1][1], d.world.c[1][2]}), 0.0f);
            records.push_back(r);
        }
    }
    for (u32 t = 0; t < kMaxDecalTextures; ++t) cmd.bind_texture(rhi::bank::kDecalTexture0 + t, tex[t], material_sampler_);
    if (records.empty()) {
        cmd.set_storage_data(rhi::bank::kDecals, rhi::as_bytes(kNone));
        return 0;
    }
    cmd.set_storage_data(rhi::bank::kDecals, std::as_bytes(records.span()));
    return static_cast<u32>(records.size());
}

// ---- GPU particles: each emitter's ring spawns and moves in one dispatch
void Forward3D::record_particles_sim(rhi::CommandBuffer& cmd, const Scene3D& scene) {
    for (EmitterGpu& e : emitters_gpu_) e.seen = false;
    const Span<const ParticleEmitter> emitters = scene.particle_emitters();
    if (!particle_sim_.is_null() && !emitters.empty()) {
        cmd.push_debug_scope("particles");
        cmd.bind_pipeline(particle_sim_);
        const f32 dt = max(scene.dt(), 0.0f);
        for (const ParticleEmitter& em : emitters) {
            EmitterGpu* g = nullptr;
            for (EmitterGpu& e : emitters_gpu_)
                if (e.id == em.id && !e.seen) g = &e;
            if (g && (g->capacity != em.max_particles || g->seed != em.seed)) {
                destroy_buffer(*device_, g->particles);
                *g = {};
                g->id = em.id;
            }
            if (!g) {
                emitters_gpu_.push_back({});
                g = &emitters_gpu_.back();
                g->id = em.id;
            }
            g->seen = true;
            if (g->particles.is_null()) {
                g->capacity = em.max_particles;
                g->seed = em.seed;
                Array<byte> zeros(mem::heap());  // lifetime 0: never spawned
                zeros.resize(usize(g->capacity) * 2 * sizeof(vec4));
                std::memset(zeros.data(), 0, zeros.size());
                g->particles = device_->create_buffer({rhi::BufferUsage::Storage, zeros.size(), "mce.3d.particles"}, zeros.span());
            }
            u32 spawn = em.burst;
            if (em.emitting && em.rate > 0.0f) {
                g->carry += em.rate * dt;
                const f32 whole = std::floor(g->carry);
                g->carry -= whole;
                spawn += static_cast<u32>(min(whole, f32(kMaxParticlesPerEmitter)));
            }
            spawn = min(spawn, g->capacity);
            vec3 dir = normalize(em.direction);
            if (!(length_sq(dir) > 0.0f)) dir = {0, 1, 0};
            SimU su{};
            su.spawn = {f32(g->cursor), f32(spawn), f32(g->capacity), uint_bits(em.seed)};
            su.step = {dt, uint_bits(static_cast<u32>(g->spawned)), max(em.drag, 0.0f), clamp(em.bounce, 0.0f, 1.0f)};
            su.origin = vec4(em.position, clamp(em.spread, 0.0f, kPi));
            su.box = vec4(em.box, em.collide ? 1.0f : 0.0f);
            su.dir = vec4(dir, em.ground_height);
            su.ranges = {em.speed_min, max(em.speed_max, em.speed_min), max(em.lifetime_min, 1e-3f), max(em.lifetime_max, em.lifetime_min)};
            su.gravity = vec4(em.gravity, 0.0f);
            cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(su));
            cmd.bind_storage_buffer(rhi::bank::kParticles, g->particles, 0, 0);
            cmd.dispatch(groups(g->capacity, 64));
            g->cursor = (g->cursor + spawn) % g->capacity;
            g->spawned += spawn;
            stats_.particles_spawned += spawn;
            ++stats_.particle_emitters;
        }
        cmd.barrier(rhi::barrier::kStorageBuffer);
        cmd.pop_debug_scope();
    }
    for (u32 i = 0; i < emitters_gpu_.size();) {
        if (emitters_gpu_[i].seen) {
            ++i;
            continue;
        }
        destroy_buffer(*device_, emitters_gpu_[i].particles);
        emitters_gpu_[i] = emitters_gpu_.back();
        emitters_gpu_.pop_back();
    }
}

// After the blended draws, in the main pass (its frame state bound).
void Forward3D::draw_particles(rhi::CommandBuffer& cmd, const Scene3D& scene) {
    if (particle_sim_.is_null()) return;
    const rhi::TextureHandle white = textures_->white();
    for (const ParticleEmitter& em : scene.particle_emitters()) {
        const rhi::BufferHandle buf = particle_buffer(em.id);
        if (buf.is_null()) continue;
        cmd.bind_pipeline(em.additive ? particle_add_ : particle_alpha_);
        cmd.bind_storage_buffer(rhi::bank::kParticles, buf, 0, 0);
        const bool textured = em.texture != ~0u && textures_->contains(em.texture);
        cmd.bind_texture(rhi::bank::kAlbedo, textured ? textures_->get(em.texture) : white, material_sampler_);
        const SpriteU su{{em.size_start, em.size_end, 0.0f, 0.0f}, em.color_start, em.color_end, {textured ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f}};
        cmd.set_uniforms(rhi::bank::kObject, rhi::as_bytes(su));
        u32 capacity = 0;
        for (const EmitterGpu& e : emitters_gpu_)
            if (e.id == em.id) capacity = e.capacity;
        cmd.draw_non_indexed(6, capacity);
    }
}

}  // namespace mce::render
