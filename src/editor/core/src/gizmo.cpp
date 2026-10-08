#include "mce/editor/gizmo.h"

#include <cmath>

#include "mce/ecs/transform.h"

namespace mce::editor {

using namespace mce::math;

namespace {

f32 dist_to_segment(vec2 p, vec2 a, vec2 b) {
    const vec2 ab = b - a;
    const f32 len2 = dot(ab, ab);
    const f32 t = len2 > 0.0f ? clamp(dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return length(p - (a + ab * t));
}

const Color kX{0.95f, 0.32f, 0.30f, 1.0f};
const Color kY{0.40f, 0.90f, 0.35f, 1.0f};
const Color kXY{0.35f, 0.65f, 1.0f, 1.0f};
const Color kHot{1.0f, 0.92f, 0.25f, 1.0f};

}  // namespace

f32 snap_value(f32 v, f32 step) { return step > 0.0f ? std::round(v / step) * step : v; }

bool TranslateGizmo::pivot(const EditorScene& scene, vec2& out) {
    const LocalTransform* t = scene.get<LocalTransform>(scene.selection().anchor());
    if (!t) return false;
    out = {t->position.x, t->position.y};
    return true;
}

GizmoPart TranslateGizmo::hit(const EditorCamera2D& cam, rhi::Extent2D size, vec2 pivot_world, vec2 mouse_px) {
    const vec2 o = cam.world_to_screen(pivot_world, size);  // screen: +y down
    const vec2 d = mouse_px - o;
    if (std::fabs(d.x) <= kBoxPx * 0.5f && std::fabs(d.y) <= kBoxPx * 0.5f) return GizmoPart::XY;
    const f32 dx = dist_to_segment(mouse_px, o, o + vec2{kArrowPx + kHeadPx, 0.0f});
    const f32 dy = dist_to_segment(mouse_px, o, o - vec2{0.0f, kArrowPx + kHeadPx});
    if (dx <= kPickPx && dx <= dy) return GizmoPart::X;
    if (dy <= kPickPx) return GizmoPart::Y;
    return GizmoPart::None;
}

void TranslateGizmo::lines(const EditorCamera2D& cam, vec2 p, GizmoPart hot, Array<OverlayLine>& out) const {
    const f32 px = 1.0f / cam.ppm();
    const GizmoPart lit = dragging() ? part_ : hot;
    const auto color = [&](GizmoPart part, Color base) { return lit == part ? kHot : base; };
    const f32 len = kArrowPx * px, head = kHeadPx * px;
    // X arrow
    const Color cx = color(GizmoPart::X, kX);
    const vec2 xt = p + vec2{len + head, 0.0f};
    out.push_back({p, p + vec2{len, 0.0f}, cx, 2.0f});
    out.push_back({xt, p + vec2{len, head * 0.6f}, cx, 2.0f});
    out.push_back({xt, p + vec2{len, -head * 0.6f}, cx, 2.0f});
    out.push_back({p + vec2{len, head * 0.6f}, p + vec2{len, -head * 0.6f}, cx, 2.0f});
    // Y arrow
    const Color cy = color(GizmoPart::Y, kY);
    const vec2 yt = p + vec2{0.0f, len + head};
    out.push_back({p, p + vec2{0.0f, len}, cy, 2.0f});
    out.push_back({yt, p + vec2{head * 0.6f, len}, cy, 2.0f});
    out.push_back({yt, p + vec2{-head * 0.6f, len}, cy, 2.0f});
    out.push_back({p + vec2{head * 0.6f, len}, p + vec2{-head * 0.6f, len}, cy, 2.0f});
    // Centre square
    const Color cc = color(GizmoPart::XY, kXY);
    const f32 h = kBoxPx * 0.5f * px;
    const vec2 q[4] = {p + vec2{-h, -h}, p + vec2{h, -h}, p + vec2{h, h}, p + vec2{-h, h}};
    quad_lines(q, cc, lit == GizmoPart::XY ? 3.0f : 2.0f, out);
}

bool TranslateGizmo::begin(const EditorScene& scene, GizmoPart part, vec2 mouse_world) {
    starts_.clear();
    if (part == GizmoPart::None) return false;
    // The anchor first: snapping is computed on it.
    const EntityId anchor = scene.selection().anchor();
    if (const LocalTransform* t = scene.get<LocalTransform>(anchor)) starts_.push_back({anchor, t->position});
    for (EntityId id : scene.selection().items()) {
        if (id == anchor) continue;
        if (const LocalTransform* t = scene.get<LocalTransform>(id)) starts_.push_back({id, t->position});
    }
    if (starts_.empty()) return false;
    part_ = part;
    grab_ = mouse_world;
    delta_ = {};
    return true;
}

void TranslateGizmo::update(EditorScene& scene, vec2 mouse_world, f32 snap) {
    if (!dragging()) return;
    vec2 d = mouse_world - grab_;
    if (part_ == GizmoPart::X) d.y = 0.0f;
    if (part_ == GizmoPart::Y) d.x = 0.0f;
    if (snap > 0.0f) {
        const vec3 a = starts_[0].position;
        if (part_ != GizmoPart::Y) d.x = snap_value(a.x + d.x, snap) - a.x;
        if (part_ != GizmoPart::X) d.y = snap_value(a.y + d.y, snap) - a.y;
    }
    delta_ = d;
    for (const Start& s : starts_) {
        const LocalTransform* cur = scene.get<LocalTransform>(s.id);
        if (!cur) continue;
        LocalTransform t = *cur;
        t.position = {s.position.x + d.x, s.position.y + d.y, s.position.z};
        scene.set(s.id, t);
    }
}

void TranslateGizmo::cancel(EditorScene& scene) {
    for (const Start& s : starts_) {
        const LocalTransform* cur = scene.get<LocalTransform>(s.id);
        if (!cur) continue;
        LocalTransform t = *cur;
        t.position = s.position;
        scene.set(s.id, t);
    }
    part_ = GizmoPart::None;
    delta_ = {};
}

}  // namespace mce::editor
