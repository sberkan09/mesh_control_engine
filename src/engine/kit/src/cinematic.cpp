#include "mce/kit/cinematic.h"

#include <cmath>
#include <cstddef>

#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/kit/timeline.h"

namespace mce::kit {

static_assert(sizeof(CameraRail3D) == 112);
static_assert(sizeof(CameraBlend3D) == 20);
static_assert(offsetof(Cutscene, bars) == offsetof(Cutscene, pad_) + sizeof(Cutscene::pad_));  // no hidden padding

namespace {

struct CinematicInstalled {};

// The entity's world position: WorldTransform when the hierarchy wrote one, else LocalTransform.
bool position_of(World& w, Entity e, math::vec3& out) {
    if (e.is_null() || !w.alive(e)) return false;
    if (const WorldTransform* wt = w.try_get<WorldTransform>(e)) {
        out = wt->position;
        return true;
    }
    if (const LocalTransform* lt = w.try_get<LocalTransform>(e)) {
        out = lt->position;
        return true;
    }
    return false;
}

f32 smoothstep01(f32 u) {
    const f32 x = math::clamp(u, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

f32 follow_blend(f32 smoothing, f32 dt) { return smoothing <= 0.0f ? 1.0f : 1.0f - std::exp(-smoothing * dt); }

}  // namespace

f32 camera_blend_weight(const CameraBlend3D& b) {
    if (b.from.is_null() || b.duration <= 0.0f) return 1.0f;
    return smoothstep01(b.elapsed / b.duration);
}

math::vec3 rail_point(const CameraRail3D& r, f32 t, math::vec3* tangent) {
    const u32 n = math::min(r.count, kRailPoints);
    if (tangent) *tangent = math::vec3{0.0f, 0.0f, -1.0f};
    if (n == 0) return {};
    if (n == 1) return r.points[0];
    // A looping rail closes back to its first point: one more span.
    const u32 spans = r.loop ? n : n - 1;
    const f32 s = math::clamp(t, 0.0f, 1.0f) * static_cast<f32>(spans);
    u32 i = static_cast<u32>(s);
    if (i >= spans) i = spans - 1;
    const f32 u = s - static_cast<f32>(i);
    auto at = [&](i32 k) {
        if (r.loop) return r.points[static_cast<u32>((k % static_cast<i32>(n) + static_cast<i32>(n)) % static_cast<i32>(n))];
        return r.points[static_cast<u32>(math::clamp(k, 0, static_cast<i32>(n) - 1))];
    };
    const i32 k = static_cast<i32>(i);
    const math::vec3 p0 = at(k - 1), p1 = at(k), p2 = at(k + 1), p3 = at(k + 2);
    // Catmull-Rom (uniform): passes through p1 at u = 0 and p2 at u = 1.
    const math::vec3 a = p1 * 2.0f;
    const math::vec3 b = p2 - p0;
    const math::vec3 c = p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3;
    const math::vec3 d = p1 * 3.0f - p0 - p2 * 3.0f + p3;
    if (tangent) *tangent = (b + c * (2.0f * u) + d * (3.0f * u * u)) * 0.5f;
    return (a + b * u + c * (u * u) + d * (u * u * u)) * 0.5f;
}

math::quat look_rotation(math::vec3 forward) {
    const f32 len = math::length(forward);
    if (len < 1e-6f) return math::quat::identity();
    const math::vec3 d = forward * (1.0f / len);
    // Yaw about +Y, then pitch about the camera's X: -Z ends up along d.
    const f32 yaw = std::atan2(-d.x, -d.z);
    const f32 pitch = std::asin(math::clamp(d.y, -1.0f, 1.0f));
    return math::normalize(math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, yaw) *
                           math::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, pitch));
}

// Before gameplay: the cutscenes' hold, letterbox and skip; camera blends move on.
void kit_cutscene(Frame& f) {
    World& w = f.world();
    f.query<CameraBlend3D>().each([&](CameraBlend3D& b) {
        if (b.elapsed < b.duration) b.elapsed = math::min(b.duration, b.elapsed + f.dt);
    });

    const input::InputState* in = f.try_resource<input::InputState>();
    bool hold = false;
    Array<Entity> skip;
    f.query<Cutscene, const TimelinePlayer>().each([&](Entity e, Cutscene& c, const TimelinePlayer& p) {
        c.active = p.playing && !p.finished;
        if (c.active) c.skipped = false;
        if (c.active && c.skippable && in && !c.skip_action.empty()) {
            const StringId id(c.skip_action.view());
            const input::ActionState* a = in->has_action(id) ? &in->action(id) : nullptr;
            const f32 held = a && a->held ? a->held_time : 0.0f;
            c.skip_progress = c.skip_hold > 0.0f ? math::min(1.0f, held / c.skip_hold) : (a && a->pressed ? 1.0f : 0.0f);
            if (c.skip_progress >= 1.0f) skip.push_back(e);
        } else {
            c.skip_progress = 0.0f;
        }
        const f32 goal = c.active ? 1.0f : 0.0f;
        const f32 step = c.letterbox_time > 0.0f ? f.dt / c.letterbox_time : 1.0f;
        c.bars = c.bars < goal ? math::min(goal, c.bars + step) : math::max(goal, c.bars - step);
        hold = hold || (c.active && c.hold_gameplay);
    });
    for (Entity e : skip) {
        if (!timeline_skip(f, e)) continue;
        if (Cutscene* c = w.try_get_mut<Cutscene>(e)) {
            c->active = false;
            c->skipped = true;
            c->skip_progress = 0.0f;
        }
    }
    // A skip this tick releases the hold at once: gameplay resumes on the same tick.
    hold = false;
    f.query<const Cutscene>().each([&](const Cutscene& c) { hold = hold || (c.active && c.hold_gameplay); });
    w.set_cutscene_held(hold);
}

// After the timelines and the follow cameras: rails, look-ats and blend weights.
void kit_cinematic(Frame& f) { place_cameras(f.world(), f.dt); }

void place_cameras(World& w, f32 dt) {
    w.query<CameraRail3D, LocalTransform>().each([&](Entity e, CameraRail3D& r, LocalTransform& t) {
        if (r.count < 2) return;
        if (r.speed > 0.0f) {
            r.t += r.speed * dt;
            if (r.loop) r.t -= std::floor(r.t);
            else r.t = math::min(r.t, 1.0f);
        }
        math::vec3 tangent;
        t.position = rail_point(r, r.t, &tangent);
        if (r.face_path && !w.try_get<CameraLookAt3D>(e) && math::length(tangent) > 1e-6f) t.rotation = look_rotation(tangent);
    });
    w.query<CameraLookAt3D, LocalTransform>().each([&](Entity e, CameraLookAt3D& l, LocalTransform& t) {
        math::vec3 target;
        if (!position_of(w, l.target, target)) return;
        math::vec3 eye = t.position;
        if (const WorldTransform* wt = w.try_get<WorldTransform>(e); wt && !w.try_get<CameraRail3D>(e)) eye = wt->position;
        const math::quat goal = look_rotation(target + l.offset - eye);
        if (!l.primed || l.smoothing <= 0.0f || dt <= 0.0f) {
            t.rotation = goal;
            l.primed = true;
        } else {
            t.rotation = math::normalize(math::slerp(t.rotation, goal, follow_blend(l.smoothing, dt)));
        }
    });
    w.query<CameraBlend3D>().each([&](CameraBlend3D& b) { b.weight = camera_blend_weight(b); });
}

bool cutscene_view(World& world, CutsceneView& out) {
    out = CutsceneView{};
    bool found = false;
    world.query<const Cutscene>().each([&](Entity e, const Cutscene& c) {
        if (c.bars <= 0.0f && !c.active) return;
        // The first active cutscene wins over one whose bars are still going out.
        if (found && (out.active || !c.active)) return;
        found = true;
        out.entity = e;
        out.active = c.active;
        out.bars = smoothstep01(c.bars);
        out.letterbox = c.letterbox;
        out.skip_progress = c.skip_progress;
        out.show_skip = c.active && c.skippable;
    });
    return found;
}

void install_cinematic(World& world) {
    if (world.try_resource<CinematicInstalled>()) return;
    world.set_resource(CinematicInstalled{});
    world.register_components<CameraRail3D, CameraLookAt3D, CameraShake3D, CameraBlend3D, Cutscene>();
    world.add_system(Phase::FixedUpdate_Pre, "kit_cutscene", &kit_cutscene);
    world.run_during_cutscenes(Phase::FixedUpdate_Pre, "kit_cutscene");
    world.add_system(Phase::FixedUpdate_Post, "kit_cinematic", &kit_cinematic);
    world.run_during_cutscenes(Phase::FixedUpdate_Post, "kit_cinematic");
}

}  // namespace mce::kit
