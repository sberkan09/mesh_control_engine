#include "mce/editor/orbit_camera.h"

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/editor/viewport2d.h"
#include "mce/asset/asset_system.h"
#include "mce/hash.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/nav3d.h"
#include "mce/physics/components.h"

namespace mce::editor {

using namespace mce::math;

namespace {

constexpr f32 kMaxPitch = 1.5533f;  // 89 degrees

quat orbit_orientation(f32 yaw, f32 pitch) {
    return normalize(quat_from_axis_angle({0, 1, 0}, yaw) * quat_from_axis_angle({1, 0, 0}, -pitch));
}

// The collider's footprint in the sprite plane: centre, half extents and angle about +Z.
struct Footprint {
    vec2 center;
    vec2 half;
    f32 angle = 0.0f;
};

bool collider_footprint(const EditorScene& scene, EntityId id, Footprint& out) {
    const physics::Collider2D* c = scene.get<physics::Collider2D>(id);
    if (!scene.get<LocalTransform>(id) || !c || c->is_sensor) return false;
    const WorldTransform placed = scene.world_transform(id);
    const WorldTransform* t = &placed;
    const physics::ColliderShape& sh = c->shape;
    vec2 half{0.25f, 0.25f};
    f32 local_angle = 0.0f;
    if (sh.type == physics::ShapeType::Box) {
        half = sh.half;
        local_angle = sh.rotation;
    } else if (sh.type == physics::ShapeType::Circle) {
        half = {sh.radius, sh.radius};
    } else if (sh.vertex_count > 0) {
        half = {0, 0};
        for (u32 i = 0; i < sh.vertex_count; ++i)
            half = max(half, vec2(std::fabs(sh.vertices[i].x), std::fabs(sh.vertices[i].y)));
    }
    const f32 angle = angle_z(t->rotation);
    const vec2 off{sh.offset.x * t->scale.x, sh.offset.y * t->scale.y};
    const f32 cs = std::cos(angle), sn = std::sin(angle);
    out.center = {t->position.x + off.x * cs - off.y * sn, t->position.y + off.x * sn + off.y * cs};
    out.half = {std::fabs(half.x * t->scale.x), std::fabs(half.y * t->scale.y)};
    out.angle = angle + local_angle;
    return out.half.x > 0.0f && out.half.y > 0.0f;
}

// A kit3d BoxCollider3D placed as kit3d_colliders places it (kit3d.cpp).
struct Box3D {
    vec3 center, half;
    quat rotation;
};

bool box_collider_3d(const EditorScene& scene, EntityId id, Box3D& out) {
    const kit::BoxCollider3D* c = scene.get<kit::BoxCollider3D>(id);
    if (!c || !scene.get<LocalTransform>(id)) return false;
    const WorldTransform t = scene.world_transform(id);
    out.half = {std::fabs(c->half.x * t.scale.x), std::fabs(c->half.y * t.scale.y), std::fabs(c->half.z * t.scale.z)};
    out.rotation = normalize(t.rotation * quat_from_axis_angle({1.0f, 0.0f, 0.0f}, radians(c->pitch_deg)));
    out.center = t.position + rotate(t.rotation, {c->offset.x * t.scale.x, c->offset.y * t.scale.y, c->offset.z * t.scale.z});
    return out.half.x > 0.0f && out.half.y > 0.0f && out.half.z > 0.0f;
}

void box_edges(const Box3D& b, Color color, Array<Segment3D>& out) {
    vec3 p[8];
    for (int i = 0; i < 8; ++i) {
        const vec3 l{(i & 1) ? b.half.x : -b.half.x, (i & 2) ? b.half.y : -b.half.y, (i & 4) ? b.half.z : -b.half.z};
        p[i] = b.center + rotate(b.rotation, l);
    }
    static constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                          {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : kEdges) out.push_back({p[e[0]], p[e[1]], color});
}

// The 12 edges of an oriented box.
void box_edges(vec3 center, vec3 half, f32 angle, Color color, Array<Segment3D>& out) {
    const f32 c = std::cos(angle), s = std::sin(angle);
    vec3 p[8];
    for (int i = 0; i < 8; ++i) {
        const vec3 l{(i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y, (i & 4) ? half.z : -half.z};
        p[i] = {center.x + l.x * c - l.y * s, center.y + l.x * s + l.y * c, center.z + l.z};
    }
    static constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                          {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : kEdges) out.push_back({p[e[0]], p[e[1]], color});
}

}  // namespace

// ---------------------------------------------------------------- OrbitCamera

void OrbitCamera::orbit(vec2 delta_px, f32 radians_per_px) {
    yaw -= delta_px.x * radians_per_px;
    pitch = clamp(pitch + delta_px.y * radians_per_px, -kMaxPitch, kMaxPitch);
}

void OrbitCamera::pan(vec2 delta_px, rhi::Extent2D viewport) {
    const quat o = orbit_orientation(yaw, pitch);
    const f32 per_px = 2.0f * effective * std::tan(fov_y * 0.5f) / static_cast<f32>(max(1u, viewport.height));
    target = target - rotate(o, {1, 0, 0}) * (delta_px.x * per_px) + rotate(o, {0, 1, 0}) * (delta_px.y * per_px);
}

void OrbitCamera::dolly(f32 notches) {
    distance = clamp(distance * std::pow(0.85f, notches), min_distance, max_distance);
    effective = min(effective, distance);
}

void OrbitCamera::frame(const aabb3& box) {
    if (box.min.x > box.max.x) return;
    target = (box.min + box.max) * 0.5f;
    const f32 radius = max(0.5f, length(box.max - box.min) * 0.5f);
    distance = clamp(radius / std::sin(fov_y * 0.5f) * 1.1f, min_distance, max_distance);
    effective = distance;
}

vec3 OrbitCamera::back() const { return -rotate(orbit_orientation(yaw, pitch), {0, 0, -1}); }

render::Camera3D OrbitCamera::camera() const { return render::orbit_camera(target, yaw, pitch, effective, fov_y); }

bool OrbitCamera::resolve(const physics::World3D& world) {
    const auto hit = world.camera_sweep(target, target + back() * distance, probe_radius);
    effective = hit ? max(0.05f, length(hit->position - target)) : distance;
    return hit.has_value();
}

// ---------------------------------------------------------------- OrbitColliders

void OrbitColliders::sync(const EditorScene& scene, f32 depth) {
    if (scene.revision() == revision_ && depth == depth_) return;
    revision_ = scene.revision();
    depth_ = depth;
    world_ = physics::World3D();
    bodies_ = 0;
    bounds_ = aabb3{{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
    for (const DocEntity& e : scene.doc().entities()) {
        Footprint f;
        if (!collider_footprint(scene, e.id, f)) continue;
        physics::BodyDesc3D d;
        d.type = physics::BodyType3D::Static;
        d.shape.type = physics::ShapeType3D::Box;
        d.shape.half = {f.half.x, f.half.y, depth * 0.5f};
        d.position = {f.center.x, f.center.y, 0.0f};
        d.rotation = rotation_z(f.angle);
        if (!world_.create_body(Entity{e.id, 1}, d)) continue;
        ++bodies_;
        const f32 c = std::fabs(std::cos(f.angle)), s = std::fabs(std::sin(f.angle));
        const vec3 ext{f.half.x * c + f.half.y * s, f.half.x * s + f.half.y * c, depth * 0.5f};
        bounds_.min = min(bounds_.min, d.position - ext);
        bounds_.max = max(bounds_.max, d.position + ext);
    }
    // kit3d boxes as the game builds them (static bodies), so the boom stops at walls and floors.
    for (const DocEntity& e : scene.doc().entities()) {
        Box3D b;
        if (!box_collider_3d(scene, e.id, b)) continue;
        physics::BodyDesc3D d;
        d.type = physics::BodyType3D::Static;
        d.shape = physics::Shape3D::box(b.half);
        d.position = b.center;
        d.rotation = b.rotation;
        if (!world_.create_body(Entity{e.id, 1}, d)) continue;
        ++bodies_;
        const f32 r = length(b.half);
        bounds_.min = min(bounds_.min, b.center - vec3{r, r, r});
        bounds_.max = max(bounds_.max, b.center + vec3{r, r, r});
    }
}

bool frame_3d(const EditorScene& scene, OrbitCamera& orbit) {
    // Through the scene's camera when it has one: the orbit pivot ten metres ahead of it.
    for (const DocEntity& e : scene.doc().entities()) {
        const kit::Camera3DComponent* c = scene.get<kit::Camera3DComponent>(e.id);
        if (!c || !c->primary || !scene.get<LocalTransform>(e.id)) continue;
        const WorldTransform t = scene.world_transform(e.id);
        const vec3 f = normalize(rotate(t.rotation, {0.0f, 0.0f, -1.0f}));
        orbit.yaw = std::atan2(-f.x, -f.z);
        orbit.pitch = clamp(-std::asin(clamp(f.y, -1.0f, 1.0f)), -kMaxPitch, kMaxPitch);
        orbit.fov_y = radians(clamp(c->fov_deg, 5.0f, 170.0f));
        orbit.distance = orbit.effective = 10.0f;
        orbit.target = t.position + f * orbit.distance;
        return true;
    }
    // Else every placed model's origin.
    aabb3 box{{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
    for (const DocEntity& e : scene.doc().entities()) {
        if (!scene.get<kit::MeshRef>(e.id) || !scene.get<LocalTransform>(e.id)) continue;
        const vec3 p = scene.world_transform(e.id).position;
        box.min = min(box.min, p - vec3{1, 1, 1});
        box.max = max(box.max, p + vec3{1, 1, 1});
    }
    if (box.min.x > box.max.x) return false;
    orbit.frame(box);
    return true;
}

// ---------------------------------------------------------------- wireframe + projection

void orbit_wireframe(const EditorScene& scene, f32 depth, Array<Segment3D>& out, bool sprite_quads) {
    const Color collider{0.45f, 0.85f, 1.0f, 0.9f}, quad{1.0f, 1.0f, 1.0f, 0.45f}, selected{1.0f, 0.62f, 0.1f, 1.0f};
    for (const DocEntity& e : scene.doc().entities()) {
        const bool sel = scene.selection().contains(e.id);
        Footprint f;
        if (collider_footprint(scene, e.id, f))
            box_edges({f.center.x, f.center.y, 0.0f}, {f.half.x, f.half.y, depth * 0.5f}, f.angle, sel ? selected : collider, out);
        Box3D b;
        if (box_collider_3d(scene, e.id, b)) box_edges(b, sel ? selected : collider, out);
        if (sel && scene.get<kit::MeshRef>(e.id) && scene.get<LocalTransform>(e.id)) {  // a cross at the model's origin
            const vec3 p = scene.world_transform(e.id).position;
            out.push_back({p - vec3{0.5f, 0, 0}, p + vec3{0.5f, 0, 0}, selected});
            out.push_back({p - vec3{0, 0.5f, 0}, p + vec3{0, 0.5f, 0}, selected});
            out.push_back({p - vec3{0, 0, 0.5f}, p + vec3{0, 0, 0.5f}, selected});
        }
        vec2 q[4];
        if (sprite_quads && entity_quad(scene, e.id, q))
            for (int i = 0; i < 4; ++i) out.push_back({{q[i].x, q[i].y, 0.0f}, {q[(i + 1) % 4].x, q[(i + 1) % 4].y, 0.0f}, sel ? selected : quad});
    }
    // The ground grid's axes: X red, Y green, Z blue.
    out.push_back({{0, 0, 0}, {1, 0, 0}, Color{0.9f, 0.3f, 0.3f, 1}});
    out.push_back({{0, 0, 0}, {0, 1, 0}, Color{0.3f, 0.9f, 0.3f, 1}});
    out.push_back({{0, 0, 0}, {0, 0, 1}, Color{0.3f, 0.5f, 1.0f, 1}});
}

void NavMeshPreview::sync(const EditorScene& scene, asset::AssetSystem* assets) {
    const kit::NavMeshSurface3D* surface = nullptr;
    for (const DocEntity& e : scene.doc().entities()) {
        surface = scene.get<kit::NavMeshSurface3D>(e.id);
        if (surface) break;
    }
    show_ = surface && surface->show;
    const asset::Guid guid = surface ? surface->baked.guid : asset::Guid{};
    if (guid.is_null() || !assets) {
        if (mesh_.loaded()) mesh_.clear();
        guid_ = {};
        hash_ = 0;
        tris_.clear();
        areas_.clear();
        links_.clear();
        return;
    }
    Result<asset::AssetHandle<asset::NavMesh>> h = assets->load_sync<asset::NavMesh>(guid);
    if (!h) return;
    const Span<const byte> blob = assets->resolve(h.value());
    const u64 hash = fnv1a64(blob.data(), blob.size());
    if (guid != guid_ || hash != hash_) {
        guid_ = guid;
        hash_ = hash;
        tris_.clear();
        areas_.clear();
        links_.clear();
        if (mesh_.load(blob, 1)) mesh_.debug_triangles(tris_, areas_, 0.03f, &links_);
    }
    assets->release(asset::AnyHandle(h.value()));
}

void NavMeshPreview::segments(Array<Segment3D>& out) const {
    if (!show_) return;
    const Color ground{0.2f, 0.85f, 0.55f, 0.55f}, other{0.95f, 0.75f, 0.2f, 0.7f}, link{0.95f, 0.35f, 0.9f, 1.0f};
    usize budget = kMaxSegments;
    for (usize t = 0; t < areas_.size() && budget >= 3; ++t, budget -= 3) {
        const Color c = areas_[t] == physics::kNavAreaGround ? ground : other;
        const vec3* v = &tris_[t * 3];
        out.push_back({v[0], v[1], c});
        out.push_back({v[1], v[2], c});
        out.push_back({v[2], v[0], c});
    }
    for (usize i = 0; i + 1 < links_.size(); i += 2) {
        const vec3 a = links_[i], b = links_[i + 1];
        const vec3 mid = (a + b) * 0.5f + vec3{0.0f, 0.5f, 0.0f};  // an arc of two segments
        out.push_back({a, mid, link});
        out.push_back({mid, b, link});
    }
}

bool project_segment(const render::View3D& view, const Segment3D& seg, vec2& a_px, vec2& b_px) {
    vec4 a = view.view_proj * vec4{seg.a.x, seg.a.y, seg.a.z, 1.0f};
    vec4 b = view.view_proj * vec4{seg.b.x, seg.b.y, seg.b.z, 1.0f};
    const f32 near_w = max(1e-4f, view.near_z);
    if (a.w < near_w && b.w < near_w) return false;
    if (a.w < near_w || b.w < near_w) {  // clip to the near plane (w = near in clip space)
        const f32 t = (near_w - a.w) / (b.w - a.w);
        const vec4 m = a + (b - a) * t;
        (a.w < near_w ? a : b) = m;
    }
    const f32 w = static_cast<f32>(view.viewport.width), h = static_cast<f32>(view.viewport.height);
    a_px = {(a.x / a.w * 0.5f + 0.5f) * w, (0.5f - a.y / a.w * 0.5f) * h};
    b_px = {(b.x / b.w * 0.5f + 0.5f) * w, (0.5f - b.y / b.w * 0.5f) * h};
    return true;
}

}  // namespace mce::editor
