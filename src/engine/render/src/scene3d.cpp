#include "mce/render/scene3d.h"

#include <cmath>

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

Scene3D::Scene3D(const MeshStore& meshes, mem::IAllocator& alloc)
    : meshes_(&meshes), alloc_(&alloc), instances_(alloc), draws_(alloc), shadow_draws_(alloc) {}

void Scene3D::begin(const Camera3D& camera, rhi::Extent2D viewport) {
    view_ = compute_view_3d(camera, viewport);
    instances_.clear();
    draws_.clear();
    shadow_draws_.clear();
    opaque_count_ = 0;
    stats_ = {};
    built_ = false;
}

void Scene3D::add(const MeshInstance& instance) {
    MCE_ASSERT_MSG(!built_, "Scene3D::add after build");
    if (!meshes_->get(instance.mesh)) return;
    instances_.push_back(instance);
}

void Scene3D::build() {
    MCE_ASSERT_MSG(!built_, "Scene3D::build twice");
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
            if (light_.shadows && !(sm.material.flags & asset::MaterialFlags::AlphaBlend)) shadow_draws_.push_back(d);
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
    }
    built_ = true;
}

}  // namespace mce::render
