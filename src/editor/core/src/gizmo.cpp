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

const char* gizmo_mode_label(GizmoMode mode) {
    switch (mode) {
        case GizmoMode::Rotate: return "Rotate";
        case GizmoMode::Scale: return "Scale";
        case GizmoMode::Move: break;
    }
    return "Move";
}

bool TransformGizmo::pivot(const EditorScene& scene, vec2& out) {
    const LocalTransform* t = scene.get<LocalTransform>(scene.selection().anchor());
    if (!t) return false;
    out = {t->position.x, t->position.y};
    return true;
}

GizmoPart TransformGizmo::hit(const EditorCamera2D& cam, rhi::Extent2D size, vec2 pivot_world, vec2 mouse_px, GizmoMode mode) {
    const vec2 o = cam.world_to_screen(pivot_world, size);  // screen: +y down
    const vec2 d = mouse_px - o;
    if (mode == GizmoMode::Rotate)
        return std::fabs(length(d) - kRingPx) <= kPickPx ? GizmoPart::Ring : GizmoPart::None;
    if (std::fabs(d.x) <= kBoxPx * 0.5f && std::fabs(d.y) <= kBoxPx * 0.5f) return GizmoPart::XY;
    const f32 dx = dist_to_segment(mouse_px, o, o + vec2{kArrowPx + kHeadPx, 0.0f});
    const f32 dy = dist_to_segment(mouse_px, o, o - vec2{0.0f, kArrowPx + kHeadPx});
    if (dx <= kPickPx && dx <= dy) return GizmoPart::X;
    if (dy <= kPickPx) return GizmoPart::Y;
    return GizmoPart::None;
}

void TransformGizmo::lines(const EditorCamera2D& cam, vec2 p, GizmoPart hot, Array<OverlayLine>& out) const {
    const f32 px = 1.0f / cam.ppm();
    const GizmoPart lit = dragging() ? part_ : hot;
    const auto color = [&](GizmoPart part, Color base) { return lit == part ? kHot : base; };
    const f32 len = kArrowPx * px, head = kHeadPx * px;
    if (mode_ == GizmoMode::Rotate) {
        const Color c = color(GizmoPart::Ring, kXY);
        const f32 r = kRingPx * px;
        constexpr int kSegments = 48;
        for (int i = 0; i < kSegments; ++i) {
            const f32 a0 = kTau * static_cast<f32>(i) / kSegments, a1 = kTau * static_cast<f32>(i + 1) / kSegments;
            out.push_back({p + vec2{std::cos(a0), std::sin(a0)} * r, p + vec2{std::cos(a1), std::sin(a1)} * r, c,
                           lit == GizmoPart::Ring ? 3.0f : 2.0f});
        }
        // The swept angle, from the grab direction, while dragging.
        if (dragging()) {
            const vec2 g = grab_ - pivot_;
            const f32 a0 = std::atan2(g.y, g.x);
            out.push_back({p, p + vec2{std::cos(a0), std::sin(a0)} * r, kHot, 1.0f});
            out.push_back({p, p + vec2{std::cos(a0 + angle_), std::sin(a0 + angle_)} * r, kHot, 2.0f});
        }
        return;
    }
    const bool scale = mode_ == GizmoMode::Scale;
    // X handle: an arrow (move) or a shaft ending in a square (scale).
    const Color cx = color(GizmoPart::X, kX);
    const Color cy = color(GizmoPart::Y, kY);
    if (scale) {
        const f32 h = head * 0.5f;
        const vec2 xe = p + vec2{len * (dragging() ? factor_.x : 1.0f), 0.0f};
        const vec2 ye = p + vec2{0.0f, len * (dragging() ? factor_.y : 1.0f)};
        out.push_back({p, xe, cx, 2.0f});
        const vec2 qx[4] = {xe + vec2{-h, -h}, xe + vec2{h, -h}, xe + vec2{h, h}, xe + vec2{-h, h}};
        quad_lines(qx, cx, 2.0f, out);
        out.push_back({p, ye, cy, 2.0f});
        const vec2 qy[4] = {ye + vec2{-h, -h}, ye + vec2{h, -h}, ye + vec2{h, h}, ye + vec2{-h, h}};
        quad_lines(qy, cy, 2.0f, out);
    } else {
        const vec2 xt = p + vec2{len + head, 0.0f};
        out.push_back({p, p + vec2{len, 0.0f}, cx, 2.0f});
        out.push_back({xt, p + vec2{len, head * 0.6f}, cx, 2.0f});
        out.push_back({xt, p + vec2{len, -head * 0.6f}, cx, 2.0f});
        out.push_back({p + vec2{len, head * 0.6f}, p + vec2{len, -head * 0.6f}, cx, 2.0f});
        const vec2 yt = p + vec2{0.0f, len + head};
        out.push_back({p, p + vec2{0.0f, len}, cy, 2.0f});
        out.push_back({yt, p + vec2{head * 0.6f, len}, cy, 2.0f});
        out.push_back({yt, p + vec2{-head * 0.6f, len}, cy, 2.0f});
        out.push_back({p + vec2{head * 0.6f, len}, p + vec2{-head * 0.6f, len}, cy, 2.0f});
    }
    // Centre square
    const Color cc = color(GizmoPart::XY, kXY);
    const f32 h = kBoxPx * 0.5f * px;
    const vec2 q[4] = {p + vec2{-h, -h}, p + vec2{h, -h}, p + vec2{h, h}, p + vec2{-h, h}};
    quad_lines(q, cc, lit == GizmoPart::XY ? 3.0f : 2.0f, out);
}

bool TransformGizmo::begin(const EditorScene& scene, GizmoPart part, vec2 mouse_world) {
    starts_.clear();
    if (part == GizmoPart::None) return false;
    if ((part == GizmoPart::Ring) != (mode_ == GizmoMode::Rotate)) return false;
    // The anchor first: snapping is computed on it, and it is the pivot.
    const EntityId anchor = scene.selection().anchor();
    if (const LocalTransform* t = scene.get<LocalTransform>(anchor)) starts_.push_back({anchor, t->position, t->rotation, t->scale});
    for (EntityId id : scene.selection().items()) {
        if (id == anchor) continue;
        if (const LocalTransform* t = scene.get<LocalTransform>(id)) starts_.push_back({id, t->position, t->rotation, t->scale});
    }
    if (starts_.empty()) return false;
    part_ = part;
    grab_ = mouse_world;
    pivot_ = {starts_[0].position.x, starts_[0].position.y};
    delta_ = {};
    angle_ = 0.0f;
    factor_ = {1.0f, 1.0f};
    return true;
}

void TransformGizmo::update(EditorScene& scene, vec2 mouse_world, f32 snap) {
    if (!dragging()) return;
    if (mode_ == GizmoMode::Rotate) {
        const vec2 g = grab_ - pivot_, m = mouse_world - pivot_;
        if (length(g) <= 1e-6f || length(m) <= 1e-6f) return;
        f32 a = std::atan2(m.y, m.x) - std::atan2(g.y, g.x);
        while (a > kPi) a -= kTau;
        while (a < -kPi) a += kTau;
        if (snap > 0.0f) a = snap_value(a, kRotateSnap);
        angle_ = a;
        const f32 c = std::cos(a), s = std::sin(a);
        const quat turn = rotation_z(a);
        for (const Start& st : starts_) {
            const LocalTransform* cur = scene.get<LocalTransform>(st.id);
            if (!cur) continue;
            LocalTransform t = *cur;
            const vec2 off{st.position.x - pivot_.x, st.position.y - pivot_.y};
            t.position = {pivot_.x + off.x * c - off.y * s, pivot_.y + off.x * s + off.y * c, st.position.z};
            t.rotation = normalize(turn * st.rotation);
            scene.set(st.id, t);
        }
        return;
    }
    if (mode_ == GizmoMode::Scale) {
        const vec2 g = grab_ - pivot_, m = mouse_world - pivot_;
        vec2 f{1.0f, 1.0f};
        const auto ratio = [](f32 now, f32 then) { return std::fabs(then) > 1e-6f ? now / then : 1.0f; };
        if (part_ == GizmoPart::X) f.x = ratio(m.x, g.x);
        if (part_ == GizmoPart::Y) f.y = ratio(m.y, g.y);
        if (part_ == GizmoPart::XY) f.x = f.y = ratio(length(m), length(g));
        if (snap > 0.0f) {
            // Snap the anchor's resulting scale, then apply the same factor to the group.
            const vec3 a = starts_[0].scale;
            if (part_ != GizmoPart::Y && std::fabs(a.x) > 1e-6f) f.x = snap_value(a.x * f.x, kScaleSnap) / a.x;
            if (part_ != GizmoPart::X && std::fabs(a.y) > 1e-6f) f.y = snap_value(a.y * f.y, kScaleSnap) / a.y;
        }
        // A drag through the pivot shrinks to the minimum instead of mirroring the selection.
        f = max(f, vec2{kMinScale, kMinScale});
        factor_ = f;
        for (const Start& st : starts_) {
            const LocalTransform* cur = scene.get<LocalTransform>(st.id);
            if (!cur) continue;
            LocalTransform t = *cur;
            const auto scaled = [](f32 v, f32 k) {
                const f32 r = v * k;
                return std::fabs(r) < kMinScale ? (v < 0.0f ? -kMinScale : kMinScale) : r;
            };
            t.scale = {scaled(st.scale.x, f.x), scaled(st.scale.y, f.y), st.scale.z};
            t.position = {pivot_.x + (st.position.x - pivot_.x) * f.x, pivot_.y + (st.position.y - pivot_.y) * f.y, st.position.z};
            scene.set(st.id, t);
        }
        return;
    }
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

void TransformGizmo::cancel(EditorScene& scene) {
    for (const Start& s : starts_) {
        const LocalTransform* cur = scene.get<LocalTransform>(s.id);
        if (!cur) continue;
        LocalTransform t = *cur;
        t.position = s.position;
        t.rotation = s.rotation;
        t.scale = s.scale;
        scene.set(s.id, t);
    }
    part_ = GizmoPart::None;
    delta_ = {};
    angle_ = 0.0f;
    factor_ = {1.0f, 1.0f};
}

}  // namespace mce::editor
