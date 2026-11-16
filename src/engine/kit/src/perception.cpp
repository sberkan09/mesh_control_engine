#include "mce/kit/perception.h"

#include <cmath>

#include "kit3d_internal.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/ai.h"
#include "mce/kit/kit3d.h"
#include "mce/physics/world2d.h"

namespace mce::kit {

namespace {

constexpr f32 kDegToRad = 0.017453292519943295f;

struct Pose {
    math::vec3 pos;
    math::quat rot;
};

bool pose_of(World& w, Entity e, Pose& out) {
    const LocalTransform* t = w.try_get<LocalTransform>(e);
    if (!t) return false;
    math::vec3 scale;
    world_pose(w, e, *t, out.pos, out.rot, scale);
    return true;
}

// The eye and the forward direction of a perceiver (forward is -Z in 3D, +X in the 2D plane).
void eye_of(const Perceiver& p, const Pose& pose, math::vec3& eye, math::vec3& fwd) {
    const bool flat = p.space == PerceptionSpace::Plane2D;
    eye = pose.pos + math::rotate(pose.rot, flat ? math::vec3{p.eye.x, p.eye.y, 0.0f} : p.eye);
    fwd = math::rotate(pose.rot, flat ? math::vec3{1.0f, 0.0f, 0.0f} : math::vec3{0.0f, 0.0f, -1.0f});
    if (flat) {
        eye.z = pose.pos.z;
        fwd.z = 0.0f;
    }
}

// Whether a ray from `eye` reaches `target` (an entity at `to`) before anything in `mask`.
bool clear_line(Frame& f, const Perceiver& p, Entity self, Entity target, math::vec3 eye, math::vec3 to, PerceptionStats& stats) {
    ++stats.rays;
    return line_of_sight(f, p.space, eye, to, p.mask, self, target);
}

struct Seen {
    Entity entity;
    math::vec3 at;
    f32 dist2 = 0.0f;
};

}  // namespace

bool line_of_sight(Frame& f, PerceptionSpace space, math::vec3 from, math::vec3 to, u16 mask, Entity ignore, Entity target) {
    const math::vec3 d = to - from;
    const f32 dist = math::length(d);
    if (dist < 1e-4f) return true;
    if (space == PerceptionSpace::Plane2D) {
        const physics::World2D* w2 = f.try_resource<physics::World2D>();
        if (!w2) return true;
        physics::QueryFilter q;
        q.layer_mask = mask;
        if (!ignore.is_null()) q.exclude = Span<const Entity>(&ignore, 1);
        const std::optional<physics::RayHit2D> h = w2->raycast({from.x, from.y}, {d.x / dist, d.y / dist}, dist, q);
        return !h || (!target.is_null() && h->entity == target);
    }
    const Physics3D* w3 = f.try_resource<Physics3D>();
    if (!w3) return true;
    physics::QueryFilter3D q;
    q.layer_mask = mask;
    if (!ignore.is_null()) q.exclude = Span<const Entity>(&ignore, 1);
    q.include_characters = true;
    const std::optional<physics::RayHit3D> h = w3->world.raycast(from, d * (1.0f / dist), dist, q);
    return !h || (!target.is_null() && h->entity == target);
}

void ai_perception(Frame& f) {
    World& w = f.world();
    PerceptionStats* stats = f.try_resource<PerceptionStats>();
    if (!stats) return;
    *stats = PerceptionStats{};

    // The stimuli once per tick, in ECS order.
    struct Shown {
        Entity entity;
        math::vec3 at;
        u32 team;
        f32 visibility;
    };
    Array<Shown> shown;
    f.query<const Stimulus>().each([&](Entity e, const Stimulus& s) {
        Pose pose;
        if (!s.visible || s.visibility <= 0.0f || !pose_of(w, e, pose)) return;
        shown.push_back(Shown{e, pose.pos + math::rotate(pose.rot, s.offset), s.team, s.visibility});
    });
    const Span<const Noise> noises = f.events().read<Noise>();

    Array<Entity> perceivers;
    f.query<const Perceiver>().each([&](Entity e, const Perceiver&) { perceivers.push_back(e); });
    for (Entity e : perceivers) {
        Perceiver& p = *w.try_get_mut<Perceiver>(e);
        Pose pose;
        if (!pose_of(w, e, pose)) continue;
        math::vec3 eye, fwd;
        eye_of(p, pose, eye, fwd);
        const bool flat = p.space == PerceptionSpace::Plane2D;
        const u32 interval = p.interval > 0 ? p.interval : 1u;
        const bool check = (f.tick + e.index) % interval == 0;
        const bool had = !p.target.is_null();

        if (check) {
            ++stats->checks;
            const f32 cos_half = std::cos(0.5f * p.sight_angle * kDegToRad);
            const f32 fwd_len = math::length(fwd);
            Seen best;
            bool found = false;
            for (const Shown& s : shown) {
                if (s.entity == e || (p.team != 0 && s.team == p.team)) continue;
                math::vec3 d = s.at - eye;
                if (flat) d.z = 0.0f;
                const f32 dist2 = math::dot(d, d);
                const f32 range = p.sight_range * s.visibility;
                if (dist2 > range * range || (found && dist2 >= best.dist2)) continue;
                if (p.sight_angle < 360.0f && dist2 > 1e-8f && fwd_len > 1e-6f &&
                    math::dot(d, fwd) < cos_half * std::sqrt(dist2) * fwd_len)
                    continue;
                if (!clear_line(f, p, e, s.entity, eye, flat ? math::vec3{s.at.x, s.at.y, eye.z} : s.at, *stats)) continue;
                best = Seen{s.entity, s.at, dist2};
                found = true;
            }
            if (found) {
                if (!p.sees || p.target != best.entity) ++p.sightings;
                p.target = best.entity;
                p.last_seen = best.at;
                p.since_seen = 0.0f;
                p.sees = 1;
            } else {
                p.sees = 0;
            }
        }
        if (!p.sees && !p.target.is_null()) {
            p.since_seen += f.dt;
            if (p.since_seen + f.dt * 1e-3f >= p.forget || !w.alive(p.target)) p.target = Entity{};
        }
        p.awareness = p.sees ? 1.0f : p.target.is_null() || p.forget <= 0.0f ? 0.0f : 1.0f - p.since_seen / p.forget;

        // Noises from the last tick: the nearest within reach.
        p.heard = 0;
        f32 nearest = 0.0f;
        for (const Noise& n : noises) {
            if (n.source == e || (p.team != 0 && n.team == p.team)) continue;
            math::vec3 d = n.position - pose.pos;
            if (flat) d.z = 0.0f;
            const f32 reach = n.loudness * p.hearing;
            const f32 dist2 = math::dot(d, d);
            if (dist2 > reach * reach || (p.heard && dist2 >= nearest)) continue;
            nearest = dist2;
            p.heard_at = n.position;
            p.heard = 1;
        }
        if (p.heard) {
            ++p.hearings;
            ++stats->heard;
        }

        // The blackboard (keys the tree does not declare are skipped).
        if (!p.target_key.empty()) {
            if (!p.target.is_null()) bb_set_entity(w, e, p.target_key.view(), p.target);
            else if (had) bb_clear(w, e, p.target_key.view());
        }
        if (!p.sees_key.empty() && check) bb_set_number(w, e, p.sees_key.view(), p.sees ? 1.0f : 0.0f);
        if (!p.last_seen_key.empty() && p.sees) bb_set_vector(w, e, p.last_seen_key.view(), p.last_seen);
        if (!p.heard_key.empty() && p.heard) bb_set_vector(w, e, p.heard_key.view(), p.heard_at);
    }
}

void install_perception(World& world) {
    if (world.try_resource<PerceptionStats>()) return;
    world.register_components<Perceiver, Stimulus>();
    world.register_event<Noise>(Phase::FixedUpdate_Pre);
    world.set_resource(PerceptionStats{});
    world.add_system(Phase::FixedUpdate_Pre, "ai_perception", &ai_perception);
}

void perceiver_views(World& w, Array<PerceiverView>& out) {
    out.clear();
    w.query<const Perceiver>().each([&](Entity e, const Perceiver& p) {
        Pose pose;
        if (!pose_of(w, e, pose)) return;
        PerceiverView v;
        v.entity = e;
        eye_of(p, pose, v.eye, v.forward);
        out.push_back(v);
    });
}

void perception_debug_lines(World& w, Array<PerceptionLine>& out) {
    out.clear();
    const math::Color cone{0.9f, 0.9f, 0.3f, 0.6f}, seen{0.3f, 1.0f, 0.3f, 1.0f}, lost{1.0f, 0.8f, 0.2f, 1.0f};
    w.query<const Perceiver>().each([&](Entity e, const Perceiver& p) {
        Pose pose;
        if (!pose_of(w, e, pose)) return;
        math::vec3 eye, fwd;
        eye_of(p, pose, eye, fwd);
        const bool flat = p.space == PerceptionSpace::Plane2D;
        const math::vec3 axis = flat ? math::vec3{0.0f, 0.0f, 1.0f} : math::vec3{0.0f, 1.0f, 0.0f};
        const f32 half = 0.5f * (p.sight_angle < 360.0f ? p.sight_angle : 360.0f) * kDegToRad;
        constexpr u32 kArc = 12;
        math::vec3 prev;
        for (u32 i = 0; i <= kArc; ++i) {
            const f32 a = -half + 2.0f * half * static_cast<f32>(i) / static_cast<f32>(kArc);
            const math::vec3 pt = eye + math::rotate(math::quat_from_axis_angle(axis, a), fwd) * p.sight_range;
            if (i == 0 || i == kArc) out.push_back(PerceptionLine{eye, pt, cone});
            if (i > 0) out.push_back(PerceptionLine{prev, pt, cone});
            prev = pt;
        }
        if (!p.target.is_null()) out.push_back(PerceptionLine{eye, p.last_seen, p.sees ? seen : lost});
    });
}

}  // namespace mce::kit
