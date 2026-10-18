#include "mce/kit/debug_lines.h"

#include <cmath>
#include <cstring>

#include "kit3d_internal.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/joints3d.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/nav3d.h"
#include "mce/kit/vehicles3d.h"

namespace mce::kit {
namespace {

using math::vec3;

constexpr f32 kDegToRad = 0.017453292519943295f;
constexpr f32 kPi = 3.14159265358979323846f;

struct Pose {
    vec3 pos;
    math::quat rot = math::quat::identity();
    vec3 scale{1.0f, 1.0f, 1.0f};
    vec3 at(vec3 local) const { return pos + math::rotate(rot, {local.x * scale.x, local.y * scale.y, local.z * scale.z}); }
};

bool pose_of(World& w, Entity e, Pose& out) {
    const LocalTransform* t = w.try_get<LocalTransform>(e);
    if (!t) return false;
    world_pose(w, e, *t, out.pos, out.rot, out.scale);
    return true;
}

struct Sink {
    Array<GameplayLine>& out;
    u32 category = 0;
    void line(vec3 a, vec3 b, math::Color c) { out.push_back(GameplayLine{a, b, c, category}); }
};

void box(Sink& s, const Pose& p, vec3 center, vec3 half, f32 pitch_deg, math::Color c) {
    const math::quat pitch = math::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, pitch_deg * kDegToRad);
    vec3 v[8];
    for (u32 i = 0; i < 8; ++i) {
        const vec3 corner{(i & 1u) ? half.x : -half.x, (i & 2u) ? half.y : -half.y, (i & 4u) ? half.z : -half.z};
        v[i] = p.at(center + math::rotate(pitch, corner));
    }
    for (u32 i = 0; i < 8; ++i)
        for (u32 bit = 1; bit <= 4; bit <<= 1)
            if (!(i & bit)) s.line(v[i], v[i | bit], c);
}

// A circle of `radius` round `center` in the plane spanned by u and v (world space), 12 segments.
void circle(Sink& s, vec3 center, vec3 u, vec3 v, f32 radius, math::Color c) {
    constexpr u32 kSeg = 12;
    vec3 prev = center + u * radius;
    for (u32 i = 1; i <= kSeg; ++i) {
        const f32 a = 2.0f * kPi * static_cast<f32>(i) / static_cast<f32>(kSeg);
        const vec3 pt = center + u * (radius * std::cos(a)) + v * (radius * std::sin(a));
        s.line(prev, pt, c);
        prev = pt;
    }
}

void sphere(Sink& s, const Pose& p, vec3 center, f32 radius, math::Color c) {
    const vec3 o = p.at(center);
    const vec3 x = math::rotate(p.rot, {1, 0, 0}), y = math::rotate(p.rot, {0, 1, 0}), z = math::rotate(p.rot, {0, 0, 1});
    circle(s, o, x, y, radius, c);
    circle(s, o, y, z, radius, c);
    circle(s, o, z, x, radius, c);
}

void capsule(Sink& s, const Pose& p, f32 radius, f32 half_height, math::Color c) {
    const vec3 x = math::rotate(p.rot, {1, 0, 0}), y = math::rotate(p.rot, {0, 1, 0}), z = math::rotate(p.rot, {0, 0, 1});
    const vec3 top = p.pos + y * half_height, bot = p.pos - y * half_height;
    circle(s, top, x, z, radius, c);
    circle(s, bot, x, z, radius, c);
    s.line(top + x * radius, bot + x * radius, c);
    s.line(top - x * radius, bot - x * radius, c);
    s.line(top + z * radius, bot + z * radius, c);
    s.line(top - z * radius, bot - z * radius, c);
    s.line(top, top + y * radius, c);
    s.line(bot, bot - y * radius, c);
}

void cross(Sink& s, vec3 at, f32 r, math::Color c) {
    s.line(at - vec3{r, 0, 0}, at + vec3{r, 0, 0}, c);
    s.line(at - vec3{0, r, 0}, at + vec3{0, r, 0}, c);
    s.line(at - vec3{0, 0, r}, at + vec3{0, 0, r}, c);
}

}  // namespace

u32 gameplay_category_bit(const char* name) {
    static const struct {
        const char* name;
        u32 bit;
    } kNames[] = {{"colliders", kDebugColliders}, {"bodies", kDebugBodies},         {"triggers", kDebugTriggers},
                  {"joints", kDebugJoints},       {"characters", kDebugCharacters}, {"vehicles", kDebugVehicles},
                  {"paths", kDebugPaths}};
    for (const auto& n : kNames)
        if (std::strcmp(n.name, name) == 0) return n.bit;
    return 0;
}

void gameplay_debug_lines(World& w, u32 mask, Array<GameplayLine>& out) {
    out.clear();
    Sink s{out};
    const math::Color collider{0.2f, 0.6f, 1.0f, 1.0f}, body{0.3f, 0.9f, 0.9f, 1.0f}, idle{0.4f, 0.4f, 0.4f, 1.0f},
        active{0.2f, 1.0f, 0.2f, 1.0f}, joint{1.0f, 0.5f, 0.1f, 1.0f}, broken{1.0f, 0.1f, 0.1f, 1.0f},
        walker{0.9f, 0.9f, 0.2f, 1.0f}, air{1.0f, 0.7f, 0.1f, 1.0f}, tyre{0.8f, 0.8f, 0.8f, 1.0f},
        skid{1.0f, 0.1f, 0.1f, 1.0f}, path{0.8f, 0.3f, 1.0f, 1.0f}, aim{1.0f, 0.3f, 0.8f, 1.0f};

    if (mask & kDebugColliders) {
        s.category = kDebugColliders;
        w.query<const BoxCollider3D>().each([&](Entity e, const BoxCollider3D& b) {
            Pose p;
            if (pose_of(w, e, p)) box(s, p, b.offset, b.half, b.pitch_deg, collider);
        });
    }
    if (mask & kDebugBodies) {
        s.category = kDebugBodies;
        w.query<const RigidBody3D>().each([&](Entity e, const RigidBody3D& b) {
            Pose p;
            if (!pose_of(w, e, p)) return;
            if (b.shape == BodyShape3D::Box) box(s, p, {}, b.half, 0.0f, body);
            else if (b.shape == BodyShape3D::Sphere) sphere(s, p, {}, b.radius, body);
            else if (b.shape == BodyShape3D::Capsule) capsule(s, p, b.radius, b.half_height, body);
        });
    }
    if (mask & kDebugTriggers) {
        s.category = kDebugTriggers;
        w.query<const Trigger3D>().each([&](Entity e, const Trigger3D& t) {
            Pose p;
            if (!pose_of(w, e, p)) return;
            const math::Color c = t.inside ? active : idle;
            if (t.shape == TriggerShape3D::Box) box(s, p, t.offset, t.half, 0.0f, c);
            else sphere(s, p, t.offset, t.radius, c);
        });
    }
    if (mask & kDebugJoints) {
        s.category = kDebugJoints;
        w.query<const Joint3D>().each([&](Entity e, const Joint3D& j) {
            Pose p;
            if (!pose_of(w, e, p)) return;
            const math::Color c = j.broken ? broken : joint;
            const vec3 at = p.at(j.anchor);
            cross(s, at, 0.1f, c);
            s.line(at, at + math::rotate(p.rot, j.axis), c);
            if (j.kind == JointKind3D::Distance) {
                Pose q;
                if (!j.connected.is_null() && pose_of(w, j.connected, q)) s.line(at, q.at(j.connected_anchor), c);
                else if (j.connected.is_null()) s.line(at, j.connected_anchor, c);
            } else if (!j.connected.is_null()) {
                Pose q;
                if (pose_of(w, j.connected, q)) s.line(at, q.pos, c);
            }
        });
    }
    if (mask & kDebugCharacters) {
        s.category = kDebugCharacters;
        w.query<const CharacterController3D>().each([&](Entity e, const CharacterController3D& c) {
            Pose p;
            if (pose_of(w, e, p)) capsule(s, p, c.radius, c.half_height, c.grounded ? walker : air);
        });
    }
    if (mask & kDebugVehicles) {
        s.category = kDebugVehicles;
        w.query<const Vehicle3D>().each([&](Entity e, const Vehicle3D& v) {
            Pose p;
            if (!pose_of(w, e, p)) return;
            for (u32 i = 0; i < 4; ++i) {
                const f32 z = i < 2 ? v.front_axle : v.rear_axle;
                const f32 x = (i & 1u) ? v.track : -v.track;
                const vec3 centre = p.at({x, v.wheel_y, z});
                const vec3 along = math::rotate(p.rot, {0.0f, 0.0f, 1.0f}) * v.wheel_radius;
                s.line(centre - along, centre + along, (v.skidding & (1u << i)) ? skid : tyre);
            }
        });
    }
    if (mask & kDebugPaths) {
        s.category = kDebugPaths;
        w.query<const Waypoint3D>().each([&](Entity e, const Waypoint3D& wp) {
            Pose p, q;
            if (!wp.next.is_null() && pose_of(w, e, p) && pose_of(w, wp.next, q)) s.line(p.pos, q.pos, path);
        });
        w.query<const VehicleDriver3D>().each([&](Entity e, const VehicleDriver3D& d) {
            Pose p, q;
            if (!d.target.is_null() && pose_of(w, e, p) && pose_of(w, d.target, q)) s.line(p.pos, q.pos, aim);
        });
        w.query<const NavAgent3D>().each([&](Entity e, const NavAgent3D& a) {
            Pose p;
            if (a.state == NavAgentState::Moving && pose_of(w, e, p)) s.line(p.pos, a.next_corner, aim);
        });
    }
}

}  // namespace mce::kit
