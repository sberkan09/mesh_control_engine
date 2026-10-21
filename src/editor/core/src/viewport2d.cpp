#include "mce/editor/viewport2d.h"

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/kit/components.h"
#include "mce/physics/components.h"

namespace mce::editor {

using namespace mce::math;

vec2 EditorCamera2D::screen_to_world(vec2 px, rhi::Extent2D size) const {
    const f32 s = 1.0f / ppm();
    return {center.x + (px.x - 0.5f * static_cast<f32>(size.width)) * s,
            center.y + (0.5f * static_cast<f32>(size.height) - px.y) * s};
}

vec2 EditorCamera2D::world_to_screen(vec2 w, rhi::Extent2D size) const {
    return {(w.x - center.x) * ppm() + 0.5f * static_cast<f32>(size.width),
            0.5f * static_cast<f32>(size.height) - (w.y - center.y) * ppm()};
}

void EditorCamera2D::pan_pixels(vec2 delta_px) {
    center.x -= delta_px.x / ppm();
    center.y += delta_px.y / ppm();
}

void EditorCamera2D::zoom_at(vec2 px, rhi::Extent2D size, f32 factor) {
    const vec2 before = screen_to_world(px, size);
    zoom = clamp(zoom * factor, 1.0f / 64.0f, 64.0f);
    const vec2 after = screen_to_world(px, size);
    center = center + (before - after);
}

void EditorCamera2D::frame(const aabb2& box, rhi::Extent2D size) {
    center = box.center();
    const f32 w = max(box.max.x - box.min.x, 1.0f) * 1.25f;
    const f32 h = max(box.max.y - box.min.y, 1.0f) * 1.25f;
    const f32 fit = min(static_cast<f32>(max(1u, size.width)) / w, static_cast<f32>(max(1u, size.height)) / h);
    zoom = clamp(fit / pixels_per_meter, 1.0f / 64.0f, 64.0f);
}

render::Camera2D EditorCamera2D::to_render() const {
    render::Camera2D c;
    c.position = center;
    c.zoom = zoom;
    c.pixel_snap = false;
    return c;
}

namespace {

void make_quad(vec2 pos, f32 angle, vec2 lo, vec2 hi, vec2 out[4]) {
    const f32 c = std::cos(angle), s = std::sin(angle);
    const vec2 local[4] = {{lo.x, lo.y}, {hi.x, lo.y}, {hi.x, hi.y}, {lo.x, hi.y}};
    for (int i = 0; i < 4; ++i) out[i] = {pos.x + local[i].x * c - local[i].y * s, pos.y + local[i].x * s + local[i].y * c};
}

f32 cross(vec2 a, vec2 b, vec2 p) { return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x); }

}  // namespace

bool entity_quad(const EditorScene& scene, EntityId id, vec2 out[4]) {
    if (!scene.get<LocalTransform>(id)) return false;
    const WorldTransform placed = scene.world_transform(id);  // children sit where their parent puts them
    const WorldTransform* t = &placed;
    const vec2 pos{t->position.x, t->position.y};
    const vec2 sc{t->scale.x, t->scale.y};
    const f32 angle = angle_z(t->rotation);
    if (const kit::Sprite* s = scene.get<kit::Sprite>(id)) {
        const vec2 size{s->size.x * sc.x, s->size.y * sc.y};
        make_quad(pos, angle, {-s->pivot.x * size.x, -s->pivot.y * size.y},
                  {(1.0f - s->pivot.x) * size.x, (1.0f - s->pivot.y) * size.y}, out);
        return true;
    }
    if (const kit::TextLabel* l = scene.get<kit::TextLabel>(id)) {
        // Layout needs the font; the pick box assumes a typical 0.55 em advance.
        const f32 h = l->height * sc.y;
        const f32 w = max(1.0f, static_cast<f32>(l->text.view().size())) * 0.55f * h;
        const f32 x0 = l->align == kit::TextAlign::Left ? 0.0f : (l->align == kit::TextAlign::Center ? -0.5f * w : -w);
        make_quad(pos, angle, {x0, -0.75f * h}, {x0 + w, 0.75f * h}, out);
        return true;
    }
    if (const physics::Collider2D* c = scene.get<physics::Collider2D>(id)) {
        const physics::ColliderShape& sh = c->shape;
        vec2 half{0.25f, 0.25f};
        if (sh.type == physics::ShapeType::Box) half = sh.half;
        else if (sh.type == physics::ShapeType::Circle) half = {sh.radius, sh.radius};
        else if (sh.vertex_count > 0) {
            half = {0, 0};
            for (u32 i = 0; i < sh.vertex_count; ++i) half = max(half, vec2(std::fabs(sh.vertices[i].x), std::fabs(sh.vertices[i].y)));
        }
        const vec2 lo{(sh.offset.x - half.x) * sc.x, (sh.offset.y - half.y) * sc.y};
        const vec2 hi{(sh.offset.x + half.x) * sc.x, (sh.offset.y + half.y) * sc.y};
        make_quad(pos, angle, lo, hi, out);
        return true;
    }
    make_quad(pos, angle, {-0.25f, -0.25f}, {0.25f, 0.25f}, out);  // a marker for empties/cameras
    return true;
}

bool point_in_quad(const vec2 q[4], vec2 p) {
    bool pos = false, neg = false;
    for (int i = 0; i < 4; ++i) {
        const f32 c = cross(q[i], q[(i + 1) % 4], p);
        pos |= c > 0.0f;
        neg |= c < 0.0f;
    }
    return !(pos && neg);
}

void entities_in_rect(const EditorScene& scene, const aabb2& rect, Array<EntityId>& out) {
    for (const DocEntity& e : scene.doc().entities()) {
        vec2 q[4];
        if (!entity_quad(scene, e.id, q)) continue;
        aabb2 b{q[0], q[0]};
        for (const vec2& p : q) {
            b.min = min(b.min, p);
            b.max = max(b.max, p);
        }
        if (b.overlaps(rect)) out.push_back(e.id);
    }
}

EntityId pick_entity(const EditorScene& scene, vec2 world) {
    EntityId best = kNoEntity;
    i64 best_key = 0;
    const Span<const DocEntity> ents = scene.doc().entities();
    for (usize i = 0; i < ents.size(); ++i) {
        vec2 q[4];
        if (!entity_quad(scene, ents[i].id, q) || !point_in_quad(q, world)) continue;
        i64 layer = 0, order = 0;
        if (const kit::Sprite* s = scene.get<kit::Sprite>(ents[i].id)) {
            layer = s->layer;
            order = s->order;
        } else if (const kit::TextLabel* l = scene.get<kit::TextLabel>(ents[i].id)) {
            layer = l->layer;
            order = l->order;
        }
        // layer (16 bits) | order (24 bits, clamped like the sort key) | file index (24 bits)
        order = clamp<i64>(order, -(1 << 23), (1 << 23) - 1);
        const i64 key = ((layer + 32768) << 48) | ((order + (1 << 23)) << 24) | static_cast<i64>(i & 0xFFFFFF);
        if (best == kNoEntity || key > best_key) {
            best = ents[i].id;
            best_key = key;
        }
    }
    return best;
}

aabb2 scene_bounds(const EditorScene& scene) {
    bool any = false;
    aabb2 box{{-5, -5}, {5, 5}};
    for (const DocEntity& e : scene.doc().entities()) {
        vec2 q[4];
        if (!entity_quad(scene, e.id, q)) continue;
        for (const vec2& p : q) {
            if (!any) box = {p, p};
            any = true;
            box.min = min(box.min, p);
            box.max = max(box.max, p);
        }
    }
    return box;
}

}  // namespace mce::editor
