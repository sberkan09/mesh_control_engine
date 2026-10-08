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
    const mat4& m = v.view_proj;
    const vec4 r0 = m.row(0), r1 = m.row(1), r2 = m.row(2), r3 = m.row(3);
    const vec4 planes[6] = {r3 + r0, r3 - r0, r3 + r1, r3 - r1, r3 + r2, r3 - r2};
    for (int i = 0; i < 6; ++i) {
        const f32 l = length(planes[i].xyz());
        v.planes[i] = l > 0.0f ? planes[i] * (1.0f / l) : planes[i];
    }
    return v;
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
      instances_(alloc),
      draws_(alloc),
      shadow_draws_(alloc) {}

void Scene3D::begin(const Camera3D& camera, rhi::Extent2D viewport) {
    view_ = compute_view_3d(camera, viewport);
    instances_.clear();
    draws_.clear();
    shadow_draws_.clear();
    point_lights_.clear();
    clusters_.clear();
    cascade_casters_.clear();
    cascade_count_ = 0;
    opaque_count_ = 0;
    stats_ = {};
    built_ = false;
}

void Scene3D::add_point_light(const PointLight& light) {
    MCE_ASSERT_MSG(!built_, "Scene3D::add_point_light after build");
    point_lights_.push_back(light);
}

void Scene3D::add(const MeshInstance& instance) {
    MCE_ASSERT_MSG(!built_, "Scene3D::add after build");
    if (!meshes_->get(instance.mesh)) return;
    instances_.push_back(instance);
}

void Scene3D::build() {
    MCE_ASSERT_MSG(!built_, "Scene3D::build twice");
    app::IClock& clock = app::steady_clock();
    const f64 t_begin = clock.now_seconds();
    stats_.instances = static_cast<u32>(instances_.size());
    const vec3 forward = view_.forward;
    bool any = false;
    for (u32 i = 0; i < instances_.size(); ++i) {
        const MeshInstance& inst = instances_[i];
        const GpuMesh& mesh = *meshes_->get(inst.mesh);
        for (u32 s = 0; s < mesh.submeshes.size(); ++s) {
            const GpuSubmesh& sm = mesh.submeshes[s];
            const mat4& node = sm.node < inst.pose.size() ? inst.pose[sm.node] : mesh.rest_pose[sm.node];
            const mat4 node_world = inst.world * node;
            const aabb3 wb = transform_aabb(node_world, sm.bounds);
            scene_bounds_ = any ? merge(scene_bounds_, wb) : wb;
            any = true;
            Draw d;
            d.instance = i;
            d.submesh = s;
            d.mesh = &mesh;
            d.model = node_world * sm.dequant;
            d.normal = transpose(inverse(node_world));
            const vec3 c = wb.center();
            d.view_depth = dot(c - view_.position, forward);
            d.center = c;
            d.radius = length(wb.extents());
            if (light_.shadows && inst.cast_shadows && !(sm.material.flags & asset::MaterialFlags::AlphaBlend))
                shadow_draws_.push_back(d);
            ++stats_.submeshes;
            if (!sphere_visible(view_, c, length(wb.extents()))) continue;
            draws_.push_back(d);
            ++stats_.visible;
            stats_.triangles += sm.index_count / 3;
        }
    }
    stats_.shadow_casters = static_cast<u32>(shadow_draws_.size());

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
            const bool blend = (sm.material.flags & asset::MaterialFlags::AlphaBlend) != 0;
            const auto material = static_cast<u16>(((sm.albedo_key & 0x7FFF) << 1) |
                                                   ((sm.material.flags & asset::MaterialFlags::DoubleSided) ? 1u : 0u));
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
           !(draws_[opaque_count_].mesh->submeshes[draws_[opaque_count_].submesh].material.flags & asset::MaterialFlags::AlphaBlend))
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
    clusters_.build(view_, point_lights_.span(), cluster_grid_);
    const f64 t_end = clock.now_seconds();
    stats_.cluster_ms = static_cast<f32>((t_end - t_clusters) * 1000.0);
    stats_.build_ms = static_cast<f32>((t_end - t_begin) * 1000.0);
    built_ = true;
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

}  // namespace mce::render
