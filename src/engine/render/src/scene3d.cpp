#include "mce/render/scene3d.h"

#include <cmath>

#include "mce/app.h"

#include "mce/render/sort_key.h"

namespace mce::render {

using namespace mce::math;

Camera3D orbit_camera(vec3 target, f32 yaw, f32 pitch, f32 distance, f32 fov_y) {
    Camera3D c;
    c.orientation = normalize(quat_from_axis_angle({0, 1, 0}, yaw) * quat_from_axis_angle({1, 0, 0}, -pitch));
    const vec3 forward = rotate(c.orientation, {0, 0, -1});
    c.position = target - forward * distance;
    c.fov_y = fov_y;
    c.near_z = math::max(0.05f, distance * 0.01f);
    c.far_z = math::max(100.0f, distance * 20.0f);
    return c;
}

View3D compute_view_3d(const Camera3D& cam, rhi::Extent2D viewport) {
    View3D v;
    v.viewport = viewport;
    v.position = cam.position;
    v.forward = rotate(normalize(cam.orientation), {0, 0, -1});
    v.near_z = cam.near_z;
    v.far_z = cam.far_z;
    v.view = inverse_rigid(trs(cam.position, normalize(cam.orientation), {1, 1, 1}));
    const f32 aspect = viewport.height ? f32(viewport.width) / f32(viewport.height) : 1.0f;
    v.proj = perspective_rh_no(cam.fov_y, aspect, cam.near_z, cam.far_z);
    v.view_proj = v.proj * v.view;
    frustum_planes(v.view_proj, v.planes);
    return v;
}

void frustum_planes(const mat4& m, vec4 out[6]) {
    const vec4 r0 = m.row(0), r1 = m.row(1), r2 = m.row(2), r3 = m.row(3);
    const vec4 planes[6] = {r3 + r0, r3 - r0, r3 + r1, r3 - r1, r3 + r2, r3 - r2};
    for (int i = 0; i < 6; ++i) {
        const f32 l = length(planes[i].xyz());
        out[i] = l > 0.0f ? planes[i] * (1.0f / l) : planes[i];
    }
}

bool sphere_visible(const View3D& v, vec3 c, f32 r) {
    for (const vec4& p : v.planes)
        if (dot(p.xyz(), c) + p.w < -r) return false;
    return true;
}

aabb3 transform_aabb(const mat4& m, const aabb3& b) {
    const f32 lo[3] = {b.min.x, b.min.y, b.min.z}, hi[3] = {b.max.x, b.max.y, b.max.z};
    f32 omin[3], omax[3];
    for (int i = 0; i < 3; ++i) {
        omin[i] = omax[i] = m.c[3][i];
        for (int j = 0; j < 3; ++j) {
            const f32 a = m.c[j][i] * lo[j], bb = m.c[j][i] * hi[j];
            omin[i] += math::min(a, bb);
            omax[i] += math::max(a, bb);
        }
    }
    return {{omin[0], omin[1], omin[2]}, {omax[0], omax[1], omax[2]}};
}

void frustum_slice_sphere(const View3D& view, f32 n, f32 f, f32& center_depth, f32& radius) {
    // k = tan of the half-angle to the slice's corner rays (half-diagonal slope).
    const f32 tx = view.proj.c[0][0] != 0.0f ? 1.0f / view.proj.c[0][0] : 1.0f;
    const f32 ty = view.proj.c[1][1] != 0.0f ? 1.0f / view.proj.c[1][1] : 1.0f;
    const f32 k2 = tx * tx + ty * ty;
    if (k2 >= (f - n) / (f + n)) {  // the far cap's circle bounds the whole slice
        center_depth = f;
        radius = f * std::sqrt(k2);
    } else {
        center_depth = 0.5f * (f + n) * (1.0f + k2);
        radius = 0.5f * std::sqrt((f - n) * (f - n) + 2.0f * (f * f + n * n) * k2 + (f + n) * (f + n) * k2 * k2);
    }
}

namespace {

vec3 light_direction(vec3 d) {
    vec3 dir = normalize(d);
    if (!(length_sq(dir) > 0.0f)) dir = {0, -1, 0};
    return dir;
}

// Light-space rotation (eye at the origin looking along `dir`).
mat4 light_rotation(vec3 dir) {
    const vec3 up = std::fabs(dir.y) > 0.99f ? vec3{0, 0, 1} : vec3{0, 1, 0};
    return look_at_rh({0, 0, 0}, dir, up);
}

}  // namespace

void fit_cascade(const View3D& view, vec3 light_dir, f32 near_depth, f32 far_depth, u32 resolution,
                 const aabb3& scene_bounds, Cascade& out) {
    f32 cd = 0.0f, r = 0.0f;
    frustum_slice_sphere(view, near_depth, far_depth, cd, r);
    // Round the radius up to a whole number of 1/16 m so tiny float noise never resizes texels.
    r = std::ceil(r * 16.0f) / 16.0f;
    const vec3 dir = light_direction(light_dir);
    const mat4 lr = light_rotation(dir);
    const vec3 c = transform_point(lr, view.position + view.forward * cd);
    const f32 texel = 2.0f * r / f32(max(resolution, 1u));
    const f32 cx = std::floor(c.x / texel) * texel, cy = std::floor(c.y / texel) * texel;
    // Depth: from the nearer of (slice sphere, scene bounds) to the slice's far side. Light-space
    // z is negative in front of the (origin) eye; ortho near/far are distances along -z.
    const vec3 sc = transform_point(lr, scene_bounds.center());
    const f32 sr = length(scene_bounds.extents());
    const f32 zn = min(-c.z - r, -sc.z - sr) - 0.5f;
    const f32 zf = -c.z + r + 0.5f;
    out.view_proj = ortho_rh_no(cx - r, cx + r, cy - r, cy + r, zn, zf) * lr;
    out.near_depth = near_depth;
    out.far_depth = far_depth;
    out.texel_world = texel;
    out.radius = r;
    out.depth_range = zf - zn;
}

Scene3D::Scene3D(const MeshStore& meshes, mem::IAllocator& alloc)
    : meshes_(&meshes),
      alloc_(&alloc),
      point_lights_(alloc),
      clusters_(alloc),
      cascade_casters_(alloc),
      local_tiles_(alloc),
      local_casters_(alloc),
      instances_(alloc),
      draws_(alloc),
      shadow_draws_(alloc),
      joint_palette_(alloc),
      batches_(alloc),
      decals_(alloc),
      decal_slots_(alloc),
      emitters_(alloc) {}

void Scene3D::begin(const Camera3D& camera, rhi::Extent2D viewport) {
    view_ = compute_view_3d(camera, viewport);
    instances_.clear();
    draws_.clear();
    shadow_draws_.clear();
    probe_draws_.clear();
    probes_.clear();
    gi_ = {};
    atmosphere_ = {};
    volumetrics_ = {};
    joint_palette_.clear();
    point_lights_.clear();
    clusters_.clear();
    cascade_casters_.clear();
    local_tiles_.clear();
    local_casters_.clear();
    batches_.clear();
    culling_ = {};
    decals_.clear();
    decal_slots_.clear();
    decal_texture_count_ = 0;
    emitters_.clear();
    cascade_count_ = 0;
    opaque_count_ = 0;
    stats_ = {};
    built_ = false;
}

void Scene3D::add_point_light(const PointLight& light) {
    MCE_ASSERT_MSG(!built_, "Scene3D::add_point_light after build");
    point_lights_.push_back(light);
}

void Scene3D::add_reflection_probe(const ReflectionProbe& probe) {
    MCE_ASSERT_MSG(!built_, "Scene3D::add_reflection_probe after build");
    if (probes_.size() >= kMaxReflectionProbes) {
        ++stats_.probes_dropped;
        return;
    }
    probes_.push_back(probe);
}

void Scene3D::set_gi_volume(const GiVolume& gi) {
    MCE_ASSERT_MSG(!built_, "Scene3D::set_gi_volume after build");
    gi_ = gi;
    for (u32& c : gi_.counts) c = clamp(c, 2u, kMaxGiProbesPerAxis);
}

void Scene3D::add(const MeshInstance& instance) {
    MCE_ASSERT_MSG(!built_, "Scene3D::add after build");
    if (!meshes_->get(instance.mesh)) return;
    instances_.push_back(instance);
}

namespace {

// The batch path's bounding sphere of an instance: the mesh's rest bounds' sphere under `w`.
void instance_sphere(const GpuMesh& mesh, const mat4& w, vec3& center, f32& radius, f32& scale) {
    const f32 sx = length(vec3{w.c[0][0], w.c[0][1], w.c[0][2]});
    const f32 sy = length(vec3{w.c[1][0], w.c[1][1], w.c[1][2]});
    const f32 sz = length(vec3{w.c[2][0], w.c[2][1], w.c[2][2]});
    scale = math::max(sx, math::max(sy, sz));
    center = transform_point(w, mesh.bounds.center());
    radius = length(mesh.bounds.extents()) * scale;
}

}  // namespace

void Scene3D::add_batch(const InstanceBatch& batch) {
    MCE_ASSERT_MSG(!built_, "Scene3D::add_batch after build");
    const GpuMesh* mesh = meshes_->get(batch.mesh);
    if (!mesh || mesh->skinned() || batch.transforms.empty()) return;
    InstanceBatch b = batch;
    if (b.bounds.min.x > b.bounds.max.x) {
        bool first = true;
        for (const mat4& w : b.transforms) {
            vec3 c;
            f32 r = 0.0f, sc = 0.0f;
            instance_sphere(*mesh, w, c, r, sc);
            const aabb3 box{c - vec3{r, r, r}, c + vec3{r, r, r}};
            b.bounds = first ? box : merge(b.bounds, box);
            first = false;
        }
    }
    batches_.push_back(b);
    ++stats_.batches;
    stats_.batch_instances += static_cast<u32>(b.transforms.size());
}

void Scene3D::add_decal(const Decal& decal) {
    MCE_ASSERT_MSG(!built_, "Scene3D::add_decal after build");
    decals_.push_back(decal);
}

void Scene3D::add_particle_emitter(const ParticleEmitter& emitter) {
    MCE_ASSERT_MSG(!built_, "Scene3D::add_particle_emitter after build");
    if (emitter.max_particles == 0) return;
    emitters_.push_back(emitter);
    emitters_.back().max_particles = math::min(emitter.max_particles, kMaxParticlesPerEmitter);
    ++stats_.particle_emitters;
}

vec4 batch_wind(const InstanceBatch& b) {
    vec2 d = b.wind_direction;
    const f32 l = std::sqrt(d.x * d.x + d.y * d.y);
    d = l > 0.0f ? vec2{d.x / l, d.y / l} : vec2{1.0f, 0.0f};
    return {b.wind_amplitude, b.wind_frequency, d.x, d.y};
}

u32 batch_lod(const GpuMesh& mesh, vec3 center, f32 radius, f32 scale, const View3D& view, const LodSettings& lod) {
    const u32 levels = mesh.lod_levels;
    if (levels <= 1) return 0;
    if (lod.force_level != ~0u) return math::min(lod.force_level, levels - 1);
    if (!lod.enabled) return 0;
    const f32 distance = math::max(length(center - view.position) - radius, view.near_z);
    const f32 px_per_unit = view.proj.c[1][1] * 0.5f * static_cast<f32>(view.viewport.height) / distance;
    u32 level = 0;
    for (u32 l = 1; l < levels; ++l)
        if (mesh.lod_error[l] * scale * px_per_unit <= lod.pixel_error) level = l;
    return level;
}

void cull_batch(const InstanceBatch& batch, const GpuMesh& mesh, const vec4 planes[6], const View3D& lod_view,
                const LodSettings& lod, u32 level_bias, Array<BatchVisible>& out) {
    const u32 levels = math::max(mesh.lod_levels, 1u);
    for (u32 i = 0; i < batch.transforms.size(); ++i) {
        vec3 c;
        f32 r = 0.0f, scale = 0.0f;
        instance_sphere(mesh, batch.transforms[i], c, r, scale);
        bool inside = true;
        for (int p = 0; p < 6 && inside; ++p) inside = dot(planes[p].xyz(), c) + planes[p].w >= -r;
        if (!inside) continue;
        if (batch.cull_distance > 0.0f && length(c - lod_view.position) - r > batch.cull_distance) continue;
        const u32 level = math::min(batch_lod(mesh, c, r, scale, lod_view, lod) + level_bias, levels - 1);
        out.push_back({level, i});
    }
}

namespace {

// splitmix64: the scatter's deterministic stream.
struct SplitMix {
    u64 state;
    u64 next() {
        u64 z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    f32 unit() { return static_cast<f32>(next() >> 40) * (1.0f / 16777216.0f); }  // [0, 1)
};

}  // namespace

void scatter_foliage(const FoliageScatter& sc, Array<mat4>& out) {
    out.clear();
    if (sc.count == 0) return;
    const f32 hx = math::max(sc.half_extents.x, 1e-3f), hz = math::max(sc.half_extents.y, 1e-3f);
    // A grid of at least `count` cells with the rectangle's aspect; a shuffled pick of them, one
    // instance jittered inside each.
    const u32 gx = math::max(1u, static_cast<u32>(std::ceil(std::sqrt(f32(sc.count) * hx / hz))));
    const u32 gz = math::max(1u, (sc.count + gx - 1) / gx);
    Array<u32> cells;
    cells.resize(usize(gx) * gz);
    for (u32 i = 0; i < cells.size(); ++i) cells[i] = i;
    SplitMix rng{u64(sc.seed) * 0x2545F4914F6CDD1Dull + 1};
    for (u32 i = static_cast<u32>(cells.size()) - 1; i > 0; --i) {
        const u32 j = static_cast<u32>(rng.next() % (u64(i) + 1));
        const u32 t = cells[i];
        cells[i] = cells[j];
        cells[j] = t;
    }
    out.resize(sc.count);
    const f32 cw = 2.0f * hx / f32(gx), cd = 2.0f * hz / f32(gz);
    for (u32 i = 0; i < sc.count; ++i) {
        const u32 cell = cells[i];
        const u32 column = cell % gx, row = cell / gx;
        const f32 x = -hx + (f32(column) + rng.unit()) * cw;
        const f32 z = -hz + (f32(row) + rng.unit()) * cd;
        const f32 yaw = sc.random_yaw ? rng.unit() * 2.0f * kPi : 0.0f;
        const f32 s = sc.scale_min + (sc.scale_max - sc.scale_min) * rng.unit();
        out[i] = trs(sc.origin + vec3{x, 0.0f, z}, quat_from_axis_angle({0, 1, 0}, yaw), {s, s, s});
    }
}

u32 Scene3D::select_lod(const MeshInstance& inst, const GpuMesh& mesh) const {
    const u32 levels = mesh.lod_levels;
    if (levels <= 1) return 0;
    if (lod_.force_level != ~0u) return math::min(lod_.force_level, levels - 1);
    if (!lod_.enabled) return 0;
    // World-space bounding sphere and the instance's largest axis scale.
    const aabb3 wb = transform_aabb(inst.world, mesh.bounds);
    const f32 radius = length(wb.extents());
    const f32 sx = length(vec3{inst.world.c[0][0], inst.world.c[0][1], inst.world.c[0][2]});
    const f32 sy = length(vec3{inst.world.c[1][0], inst.world.c[1][1], inst.world.c[1][2]});
    const f32 sz = length(vec3{inst.world.c[2][0], inst.world.c[2][1], inst.world.c[2][2]});
    const f32 scale = math::max(sx, math::max(sy, sz));
    const f32 distance = math::max(length(wb.center() - view_.position) - radius, view_.near_z);
    // Pixels per world unit at that distance: proj[1][1] = 1 / tan(fov_y / 2).
    const f32 px_per_unit = view_.proj.c[1][1] * 0.5f * static_cast<f32>(view_.viewport.height) / distance;
    auto coarsest = [&](f32 limit) {
        u32 level = 0;
        for (u32 l = 1; l < levels; ++l)
            if (mesh.lod_error[l] * scale * px_per_unit <= limit) level = l;
        return level;
    };
    const u32 loose = coarsest(lod_.pixel_error);
    if (!inst.lod_state) return loose;
    const u32 strict = coarsest(lod_.pixel_error * (1.0f - math::clamp(lod_.hysteresis, 0.0f, 0.9f)));
    const u32 previous = *inst.lod_state;
    return math::clamp(previous, strict, loose);  // stay put inside the band
}

void Scene3D::build() {
    MCE_ASSERT_MSG(!built_, "Scene3D::build twice");
    app::IClock& clock = app::steady_clock();
    const f64 t_begin = clock.now_seconds();
    stats_.instances = static_cast<u32>(instances_.size());
    if (atmosphere_.enabled) {
        // The light above the air, then through it; the ambient from the sky and the lit ground.
        const vec3 sun = atmosphere_quantized_sun(atmosphere_, -normalize(light_.direction));
        const vec3 above = light_.color * light_.intensity;
        atmosphere_sun_ = sun;
        sun_irradiance_ = above * math::kPi;
        const vec3 t = atmosphere_transmittance(atmosphere_, atmosphere_.altitude, sun);
        if (atmosphere_.sun_transmittance) light_.color = light_.color * t;
        if (atmosphere_.ambient) {
            const vec3 sky = atmosphere_sky_ambient(atmosphere_, sun) * sun_irradiance_;
            light_.ambient_sky = sky;
            light_.ambient_ground = atmosphere_.ground_albedo * (above * t * max(sun.y, 0.0f) + sky);
        }
    }
    const vec3 forward = view_.forward;
    bool any = false;
    bool local_wanted = false;  // a light asks for a local shadow: casters are collected without the sun's
    if (local_shadows_.enabled)
        for (const PointLight& l : point_lights_) local_wanted = local_wanted || l.cast_shadows;
    const bool collect_casters = light_.shadows || local_wanted;
    const bool taa = post_.taa || post_.motion_blur.enabled;  // previous transforms for the velocity prepass
    Array<u32> skin_first(*alloc_);   // per skin of the current instance: palette offset
    Array<u32> skin_prev(*alloc_);    // and its previous pose's palette offset (TAA)
    Array<aabb3> skin_box(*alloc_);   // and its model-space bounds
    for (u32 i = 0; i < instances_.size(); ++i) {
        const MeshInstance& inst = instances_[i];
        const GpuMesh& mesh = *meshes_->get(inst.mesh);
        const bool skinning = mesh.skinned() && inst.skinning;
        if (skinning) {
            skin_first.resize(mesh.skins.size());
            skin_box.resize(mesh.skins.size());
            for (u32 k = 0; k < mesh.skins.size(); ++k) {
                skin_first[k] = static_cast<u32>(joint_palette_.size());
                joint_palette_.resize(joint_palette_.size() + mesh.skins[k].joint_count);
                const Span<mat4> palette = joint_palette_.span().subspan(skin_first[k], mesh.skins[k].joint_count);
                skin_palette(mesh, k, inst.pose, palette);
                skin_box[k] = skin_bounds(mesh, k, palette);
            }
            skin_prev.resize(mesh.skins.size());
            for (u32 k = 0; k < mesh.skins.size(); ++k) {
                skin_prev[k] = skin_first[k];
                if (!taa || inst.prev_pose.empty()) continue;
                skin_prev[k] = static_cast<u32>(joint_palette_.size());
                joint_palette_.resize(joint_palette_.size() + mesh.skins[k].joint_count);
                skin_palette(mesh, k, inst.prev_pose, joint_palette_.span().subspan(skin_prev[k], mesh.skins[k].joint_count));
            }
            ++stats_.skinned_instances;
        }
        const u32 lod = select_lod(inst, mesh);
        if (inst.lod_state) *inst.lod_state = static_cast<u8>(lod);
        const u32 shadow_lod = math::min(lod + lod_.shadow_bias, mesh.lod_levels - 1);
        if (mesh.lod_levels > 1) ++stats_.lod_instances[lod];
        for (u32 s = 0; s < mesh.submeshes.size(); ++s) {
            const GpuSubmesh& sm = mesh.submeshes[s];
            Draw d;
            d.instance = i;
            d.submesh = s;
            d.mesh = &mesh;
            d.dequant = mat4::identity();
            d.palette = ~0u;
            d.prev_palette = ~0u;
            const mat4& prev_world = taa && inst.prev_world ? *inst.prev_world : inst.world;
            aabb3 wb;
            if (sm.skin != ~0u) {
                // Skinned geometry lives in bind space: glTF ignores the skinned node's transform.
                d.normal = transpose(inverse(inst.world));
                if (skinning) {
                    d.model = inst.world;
                    d.prev_model = prev_world;
                    d.dequant = sm.dequant;
                    d.palette = skin_first[sm.skin];
                    d.prev_palette = skin_prev[sm.skin];
                    wb = transform_aabb(inst.world, skin_box[sm.skin]);
                } else {  // the bind pose, drawn rigidly
                    d.model = inst.world * sm.dequant;
                    d.prev_model = prev_world * sm.dequant;
                    wb = transform_aabb(inst.world, sm.bounds);
                }
            } else {
                const mat4& node = sm.node < inst.pose.size() ? inst.pose[sm.node] : mesh.rest_pose[sm.node];
                const mat4 node_world = inst.world * node;
                wb = transform_aabb(node_world, sm.bounds);
                d.model = node_world * sm.dequant;
                d.prev_model = d.model;
                if (taa && (inst.prev_world || !inst.prev_pose.empty())) {
                    const Span<const mat4> pp = inst.prev_pose.empty() ? inst.pose : inst.prev_pose;
                    const mat4& prev_node = sm.node < pp.size() ? pp[sm.node] : mesh.rest_pose[sm.node];
                    d.prev_model = prev_world * prev_node * sm.dequant;
                }
                d.normal = transpose(inverse(node_world));
            }
            scene_bounds_ = any ? merge(scene_bounds_, wb) : wb;
            any = true;
            const vec3 c = wb.center();
            d.view_depth = dot(c - view_.position, forward);
            d.center = c;
            d.radius = length(wb.extents());
            if (collect_casters && inst.cast_shadows && !(material_of(inst, sm).flags & asset::MaterialFlags::AlphaBlend)) {
                const GpuLod& sl = mesh.lod(s, shadow_lod);
                d.lod = shadow_lod;
                d.first_index = sl.first_index;
                d.index_count = sl.index_count;
                d.base_vertex = sl.base_vertex;
                shadow_draws_.push_back(d);
            }
            if ((!probes_.empty() || gi_.enabled) && !(material_of(inst, sm).flags & asset::MaterialFlags::AlphaBlend)) {
                const GpuLod& pl = mesh.lod(s, 0);
                d.lod = 0;
                d.first_index = pl.first_index;
                d.index_count = pl.index_count;
                d.base_vertex = pl.base_vertex;
                probe_draws_.push_back(d);
            }
            ++stats_.submeshes;
            if (!sphere_visible(view_, c, length(wb.extents()))) continue;
            const GpuLod& ml = mesh.lod(s, lod);
            d.lod = lod;
            d.first_index = ml.first_index;
            d.index_count = ml.index_count;
            d.base_vertex = ml.base_vertex;
            draws_.push_back(d);
            ++stats_.visible;
            stats_.triangles += ml.index_count / 3;
            stats_.lod_triangles[lod] += ml.index_count / 3;
        }
    }
    stats_.shadow_casters = static_cast<u32>(shadow_draws_.size());
    stats_.joints = static_cast<u32>(joint_palette_.size());
    for (const InstanceBatch& b : batches_) {  // their casters reach into the cascades' depth range
        scene_bounds_ = any ? merge(scene_bounds_, b.bounds) : b.bounds;
        any = true;
    }
    // Decals: the visible ones in submission order, within the decal and texture limits.
    for (const Decal& d : decals_) {
        const vec3 c = transform_point(d.world, {0, 0, 0});
        const f32 r = 0.5f * (length(vec3{d.world.c[0][0], d.world.c[0][1], d.world.c[0][2]}) +
                              length(vec3{d.world.c[1][0], d.world.c[1][1], d.world.c[1][2]}) +
                              length(vec3{d.world.c[2][0], d.world.c[2][1], d.world.c[2][2]}));
        if (!sphere_visible(view_, c, r)) {
            ++stats_.decals_culled;
            continue;
        }
        if (decal_slots_.size() >= kMaxDecals) {
            ++stats_.decals_dropped;
            continue;
        }
        u32 slot = ~0u;
        if (d.texture != ~0u) {
            for (u32 t = 0; t < decal_texture_count_; ++t)
                if (decal_textures_[t] == d.texture) slot = t;
            if (slot == ~0u && decal_texture_count_ < kMaxDecalTextures) {
                slot = decal_texture_count_;
                decal_textures_[decal_texture_count_++] = d.texture;
            }
            if (slot == ~0u) {
                ++stats_.decals_dropped;
                continue;
            }
        }
        decal_slots_.push_back(slot);
        decals_[decal_slots_.size() - 1] = d;  // compacts in place: the write index trails the read one
    }
    decals_.resize(decal_slots_.size());
    stats_.decals = static_cast<u32>(decals_.size());

    // Sort: opaque by (material, depth ascending), blended by depth descending. The radix sort is
    // stable, so equal keys keep instance/submesh submission order.
    const usize n = draws_.size();
    if (n > 1) {
        Array<u64> keys(*alloc_);
        Array<u32> order(*alloc_);
        keys.resize(n);
        order.resize(n);
        const f32 far_z = math::max(1e-3f, view_.far_z);
        for (usize i = 0; i < n; ++i) {
            const Draw& d = draws_[i];
            const GpuSubmesh& sm = d.mesh->submeshes[d.submesh];
            const f32 dn = math::clamp(d.view_depth / far_z, 0.0f, 1.0f);
            const auto q = static_cast<u32>(dn * f32(0xFFFFFF));
            const MeshInstance& inst = instances_[d.instance];
            const asset::MaterialChunk& mat = material_of(inst, sm);
            const bool blend = (mat.flags & asset::MaterialFlags::AlphaBlend) != 0;
            const auto material = static_cast<u16>(((albedo_key_of(inst, sm) & 0x7FFF) << 1) |
                                                   ((mat.flags & asset::MaterialFlags::DoubleSided) ? 1u : 0u));
            keys[i] = blend ? sort_key_3d(PassId::Transparent, 0, 0xFFFFFFu - q) : sort_key_3d(PassId::Opaque, material, q);
            order[i] = static_cast<u32>(i);
        }
        radix_sort(keys.span(), order.span(), *alloc_);
        Array<Draw> sorted(*alloc_);
        sorted.reserve(n);
        for (usize i = 0; i < n; ++i) sorted.push_back(draws_[order[i]]);
        draws_ = std::move(sorted);
    }
    opaque_count_ = 0;
    while (opaque_count_ < draws_.size() &&
           !(material_of(instances_[draws_[opaque_count_].instance], draws_[opaque_count_].mesh->submeshes[draws_[opaque_count_].submesh])
                 .flags &
             asset::MaterialFlags::AlphaBlend))
        ++opaque_count_;

    // Shadow fit (rhi-renderer.md §3.5): an orthographic box around the scene's bounding sphere,
    // seen from the light, with the light-space origin snapped to shadow-map texels so a moving
    // prop that changes the bounds does not make every shadow edge crawl.
    if (any) {
        vec3 dir = normalize(light_.direction);
        if (!(length_sq(dir) > 0.0f)) dir = {0, -1, 0};
        const vec3 center = scene_bounds_.center();
        const f32 r = math::max(length(scene_bounds_.extents()) * 1.05f, 0.01f);  // margin for snapping + PCF
        const vec3 up = std::fabs(dir.y) > 0.99f ? vec3{0, 0, 1} : vec3{0, 1, 0};
        mat4 lv = look_at_rh(center - dir * (2.0f * r), center, up);
        constexpr f32 kShadowRes = 2048.0f;
        const f32 texel = 2.0f * r / kShadowRes;
        lv.c[3][0] = std::floor(lv.c[3][0] / texel) * texel;
        lv.c[3][1] = std::floor(lv.c[3][1] / texel) * texel;
        light_view_proj_ = ortho_rh_no(-r, r, -r, r, 0.5f * r, 3.5f * r) * lv;
        build_cascades();
    }

    stats_.point_lights = static_cast<u32>(point_lights_.size());
    const f64 t_clusters = clock.now_seconds();
    clusters_.prepare(view_, point_lights_.span(), cluster_grid_);
    if (local_wanted) build_local_shadows();
    const f64 t_end = clock.now_seconds();
    stats_.cluster_ms = static_cast<f32>((t_end - t_clusters) * 1000.0);
    stats_.build_ms = static_cast<f32>((t_end - t_begin) * 1000.0);
    built_ = true;
}

const ClusterLights& Scene3D::clusters() const {
    if (!clusters_.assigned()) {
        const f64 t0 = app::steady_clock().now_seconds();
        clusters_.assign();
        stats_.cluster_ms += static_cast<f32>((app::steady_clock().now_seconds() - t0) * 1000.0);
    }
    return clusters_;
}

// Cascade ranges: nominal [n_i, f_i] with f_i = distance * splits[i]. Cascade i+1 is fitted from
// the start of cascade i's blend zone so the shader can cross-fade the two without a gap.
void Scene3D::build_cascades() {
    if (!light_.shadows || shadow_draws_.empty()) return;
    const ShadowSettings& ss = shadow_settings_;
    const u32 n = clamp(ss.cascades, 1u, kMaxCascades);
    const f32 dist = clamp(ss.distance, view_.near_z * 2.0f, view_.far_z);
    const f32 blend = clamp(ss.blend, 0.0f, 0.5f);
    f32 nominal_near = view_.near_z, fit_near = view_.near_z, prev = 0.0f;
    for (u32 i = 0; i < n; ++i) {
        f32 far = i + 1 == n ? dist : dist * clamp(ss.splits[i], 0.0f, 1.0f);
        far = max(far, max(prev, nominal_near) + 0.01f);
        Cascade& c = cascades_[i];
        fit_cascade(view_, light_.direction, fit_near, far, ss.resolution, scene_bounds_, c);
        c.near_depth = nominal_near;
        c.first_caster = static_cast<u32>(cascade_casters_.size());
        // Casters: bounding sphere vs the cascade's light-space box (clip x/y in [-1, 1], z up
        // to the far side; everything nearer than the box is inside its stretched depth range).
        const f32 xy_scale = 1.0f / c.radius, z_scale = 2.0f / c.depth_range;
        for (u32 di = 0; di < shadow_draws_.size(); ++di) {
            const Draw& d = shadow_draws_[di];
            const vec4 p = c.view_proj * vec4(d.center, 1.0f);
            const f32 rx = d.radius * xy_scale;
            if (std::fabs(p.x) > 1.0f + rx || std::fabs(p.y) > 1.0f + rx || p.z - d.radius * z_scale > 1.0f) continue;
            cascade_casters_.push_back(di);
        }
        c.caster_count = static_cast<u32>(cascade_casters_.size()) - c.first_caster;
        stats_.cascade_casters[i] = c.caster_count;
        fit_near = max(view_.near_z, far - blend * (far - nominal_near));
        nominal_near = far;
        prev = far;
    }
    cascade_count_ = n;
}

// Local-light shadows (M-VISUAL 4). Importance = luminance × (radius / distance)², the light's
// brightness times its projected size (capped once the camera is inside the sphere); ties keep
// submission order. Tiles go out greedily in that order (6 for a point light, 1 for a spot) until
// the atlas or `max_lights` runs out: a point light that no longer fits leaves room for later spots.
void Scene3D::build_local_shadows() {
    const LocalShadowSettings& ls = local_shadows_;
    const u32 res = max(shadow_settings_.resolution, 1u);
    const u32 tile = clamp(ls.tile, 16u, res);
    const u32 per_row = res / tile;
    const u32 capacity = per_row * per_row * ls.layers;
    struct Candidate {
        f32 score;
        u32 light;
    };
    Array<Candidate> cand(*alloc_);
    for (u32 i = 0; i < point_lights_.size(); ++i) {
        const PointLight& l = point_lights_[i];
        if (!l.cast_shadows || !(l.intensity > 0.0f) || l.radius <= 0.0f) continue;
        if (!sphere_visible(view_, l.position, l.radius)) continue;  // lights nothing on screen
        const vec3 c = l.color * l.intensity;
        const f32 luma = 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
        const f32 size = min(l.radius / max(length(l.position - view_.position), 1e-3f), 2.0f);
        cand.push_back({luma * size * size, i});
    }
    // Stable insertion sort by descending score (a handful of shadowed lights per frame).
    for (u32 i = 1; i < cand.size(); ++i) {
        const Candidate c = cand[i];
        u32 j = i;
        for (; j > 0 && cand[j - 1].score < c.score; --j) cand[j] = cand[j - 1];
        cand[j] = c;
    }
    const f32 texel_ndc = 2.0f / f32(tile);
    // Cube faces see a little past 90° so a PCF footprint at a face edge stays inside the face's tile.
    const f32 face_tan = 1.0f / max(1.0f - 5.0f / f32(tile), 0.5f);
    static const vec3 kFaceDir[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const vec3 kFaceUp[6] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
    u32 used = 0;
    for (const Candidate& c : cand) {
        const PointLight& l = point_lights_[c.light];
        const bool spot = l.spot_outer > 0.0f;
        const u32 need = spot ? 1u : 6u;
        if (stats_.local_shadow_lights >= ls.max_lights || used + need > capacity) {
            ++stats_.local_shadows_dropped;
            continue;
        }
        const f32 zn = clamp(l.radius * 0.01f, 0.02f, 0.5f);
        const u32 first_record = static_cast<u32>(local_tiles_.size());
        for (u32 f = 0; f < need; ++f) {
            LocalShadowTile t;
            vec3 dir, up;
            f32 tan_half;
            if (spot) {
                dir = normalize(l.direction);
                if (!(length_sq(dir) > 0.0f)) dir = {0, -1, 0};
                up = std::fabs(dir.y) > 0.99f ? vec3{0, 0, 1} : vec3{0, 1, 0};
                tan_half = std::tan(min(l.spot_outer, 1.45f));
            } else {
                dir = kFaceDir[f];
                up = kFaceUp[f];
                tan_half = face_tan;
            }
            t.view_proj = perspective_rh_no(2.0f * std::atan(tan_half), 1.0f, zn, l.radius) * look_at_rh(l.position, l.position + dir, up);
            const u32 slot = used + f;
            const u32 in_layer = slot % (per_row * per_row);
            t.layer = slot / (per_row * per_row);
            t.rect = rhi::Rect2D{i32((in_layer % per_row) * tile), i32((in_layer / per_row) * tile), tile, tile};
            t.light = c.light;
            t.texel_scale = tan_half * texel_ndc;
            // Casters: bounding sphere vs the tile's frustum (Gribb-Hartmann planes, unnormalized
            // distance scaled by the plane normal's length).
            const mat4& m = t.view_proj;
            const vec4 r0 = m.row(0), r1 = m.row(1), r2 = m.row(2), r3 = m.row(3);
            const vec4 planes[6] = {r3 + r0, r3 - r0, r3 + r1, r3 - r1, r3 + r2, r3 - r2};
            t.first_caster = static_cast<u32>(local_casters_.size());
            for (u32 di = 0; di < shadow_draws_.size(); ++di) {
                const Draw& d = shadow_draws_[di];
                bool inside = true;
                for (const vec4& p : planes)
                    if (dot(p.xyz(), d.center) + p.w < -d.radius * length(p.xyz())) {
                        inside = false;
                        break;
                    }
                if (inside) local_casters_.push_back(di);
            }
            t.caster_count = static_cast<u32>(local_casters_.size()) - t.first_caster;
            local_tiles_.push_back(t);
        }
        used += need;
        clusters_.set_shadow(c.light, first_record, need);
        ++stats_.local_shadow_lights;
    }
    stats_.local_shadow_tiles = static_cast<u32>(local_tiles_.size());
    stats_.local_shadow_casters = static_cast<u32>(local_casters_.size());
}

}  // namespace mce::render
