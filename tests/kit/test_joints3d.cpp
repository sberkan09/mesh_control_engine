// 3D joints as components (M-GAMEPLAY3D step 2): Joint3D on RigidBody3D entities, to the world and
// to another body; a door, a motor-driven drawbridge, rebuilds on a settings change, removal,
// breaking with its event, and the replay hash.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/joints3d.h"
#include "mce/kit/kit3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

struct Scene {
    World w;
    u64 t = 0;
    Scene() {
        w.register_components<LocalTransform>();
        install_joints3d(w, nullptr);
        BoxCollider3D floor;
        floor.half = {20.0f, 0.5f, 20.0f};
        w.spawn_with(at(0, -0.5f, 0), floor);  // top at y = 0
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    // A 1 m wide door panel (0.1 thick, 2 tall) standing with its left edge at x = 0.
    Entity door() {
        RigidBody3D b;
        b.half = {0.5f, 1.0f, 0.05f};
        b.density = 200.0f;
        return w.spawn_with(at(0.5f, 1.05f, 0), b);
    }
    const Joint3D& joint(Entity e) { return *w.try_get<Joint3D>(e); }
    math::vec3 pos(Entity e) { return w.try_get<LocalTransform>(e)->position; }
    Physics3D& phys() { return w.resource<Physics3D>(); }
};

}  // namespace

TEST_CASE("joints3d: a door swings on its hinge, within its limits") {
    Scene s;
    const Entity d = s.door();
    Joint3D j;
    j.kind = JointKind3D::Hinge;
    j.anchor = {-0.5f, 0, 0};  // the left edge
    j.axis = {0, 1, 0};
    j.limits = 1;
    j.limit_min = 0.0f;
    j.limit_max = 90.0f;
    s.w.add(d, j);
    s.run(1);
    CHECK(s.phys().world.stats().joints == 1);
    // A push on the door's free end (toward -z) turns it about +y, the positive way.
    s.w.try_get_mut<RigidBody3D>(d)->impulse = {0, 0, -60.0f};
    f32 widest = 0.0f;
    for (int i = 0; i < 120; ++i) {
        s.run(1);
        widest = math::max(widest, s.joint(d).position);
    }
    CHECK(widest > 60.0f);
    CHECK(widest < 93.0f);
    // The hinge edge stays where it was; the door does not fall over.
    const LocalTransform& t = *s.w.try_get<LocalTransform>(d);
    const math::vec3 edge = t.position + math::rotate(t.rotation, math::vec3{-0.5f, 0, 0});
    CHECK(math::length(edge - math::vec3{0, 1.05f, 0}) < 0.02f);
    CHECK(s.joint(d).force > 0.0f);
}

TEST_CASE("joints3d: a drawbridge raised by a position motor; switching the motor and the joint") {
    Scene s;
    RigidBody3D deck;
    deck.half = {1.0f, 0.1f, 2.0f};
    deck.density = 100.0f;
    const Entity b = s.w.spawn_with(at(0, 1.0f, 2.0f), deck);  // hinged along its z = 0 edge
    Joint3D j;
    j.kind = JointKind3D::Hinge;
    j.anchor = {0, 0, -2.0f};
    j.axis = {-1, 0, 0};  // turning about -x lifts the far (+z) end
    j.motor = JointMotor3D::Position;
    j.motor_target = 70.0f;  // degrees
    j.motor_frequency = 5.0f;
    s.w.add(b, j);
    s.run(240);
    CHECK(s.joint(b).position == doctest::Approx(70.0f).epsilon(0.05));
    CHECK(s.pos(b).y > 2.5f);  // the deck's middle went up

    // Lowered again by changing only the motor target (no rebuild: the angle keeps its zero).
    s.w.try_get_mut<Joint3D>(b)->motor_target = 0.0f;
    s.run(300);
    CHECK(std::fabs(s.joint(b).position) < 3.0f);

    // Motor off and joint off: the deck falls to the floor.
    s.w.try_get_mut<Joint3D>(b)->motor = JointMotor3D::Off;
    s.w.try_get_mut<Joint3D>(b)->enabled = 0;
    s.run(120);
    CHECK(s.pos(b).y < 0.3f);
    CHECK(s.phys().world.stats().joints == 1);  // kept, switched off
}

TEST_CASE("joints3d: two bodies joined, rebuilt on a settings change, gone with a body or the component") {
    Scene s;
    RigidBody3D box;
    box.half = {0.25f, 0.25f, 0.25f};
    const Entity a = s.w.spawn_with(at(0, 3, 0), box);
    const Entity c = s.w.spawn_with(at(1, 3, 0), box);
    Joint3D j;
    j.kind = JointKind3D::Distance;
    j.connected = c;
    s.w.add(a, j);  // anchors at both centres: a 1 m rod
    s.run(90);      // both fall and land, still 1 m apart
    CHECK(s.joint(a).position == doctest::Approx(1.0f).epsilon(0.03));
    CHECK(s.pos(a).y == doctest::Approx(0.25f).epsilon(0.1));

    // A longer rod: changing a setting rebuilds the joint at the bodies' current poses.
    s.w.try_get_mut<Joint3D>(a)->min_distance = 2.0f;
    s.w.try_get_mut<Joint3D>(a)->max_distance = 2.0f;
    s.run(90);
    CHECK(s.joint(a).position == doctest::Approx(2.0f).epsilon(0.03));
    CHECK(s.phys().world.stats().joints == 1);

    s.w.destroy(c);  // the other body goes: so does the joint
    s.run(1);
    CHECK(s.phys().world.stats().joints == 0);
    CHECK(s.joint(a).force == 0.0f);

    // To a static collider instead, then the component removed.
    BoxCollider3D post;
    post.half = {0.1f, 1.0f, 0.1f};
    const Entity p = s.w.spawn_with(at(3, 1, 0), post);
    Joint3D r;
    r.kind = JointKind3D::Distance;
    r.connected = p;
    r.connected_anchor = {0, 1.0f, 0};  // the post's top
    r.min_distance = 0.0f;
    s.w.add(a, r);
    s.run(1);
    CHECK(s.phys().world.stats().joints == 1);
    s.w.remove<Joint3D>(a);
    s.run(1);
    CHECK(s.phys().world.stats().joints == 0);

    // A joint nothing can build (both sides static) is refused once, not every tick.
    Joint3D bad;
    bad.connected = p;
    const Entity floor_post = s.w.spawn_with(at(-3, 1, 0), post, bad);
    s.run(3);
    CHECK(s.phys().world.stats().joints == 0);
    CHECK(s.w.alive(floor_post));
}

TEST_CASE("joints3d: an overloaded joint breaks once, with an event, until a setting changes") {
    Scene s;
    RigidBody3D weight;
    weight.half = {0.5f, 0.5f, 0.5f};
    weight.density = 2000.0f;  // 2 t: 19.6 kN
    const Entity e = s.w.spawn_with(at(0, 4, 0), weight);
    Joint3D j;
    j.kind = JointKind3D::Fixed;
    j.break_force = 5000.0f;
    s.w.add(e, j);
    u32 breaks = 0;
    for (int i = 0; i < 60; ++i) {
        s.run(1);
        for (const JointBreak3D& ev : EventAccess(s.w).read<JointBreak3D>()) {
            CHECK(ev.joint == e);
            CHECK(ev.connected.is_null());
            ++breaks;
        }
    }
    CHECK(breaks == 1);
    CHECK(s.joint(e).broken == 1);
    CHECK(s.pos(e).y < 1.0f);  // fell

    // A stronger rating rebuilds it where the weight now lies.
    s.w.try_get_mut<Joint3D>(e)->break_force = 1e6f;
    s.run(2);
    CHECK(s.joint(e).broken == 0);
    CHECK(s.joint(e).force > 0.0f);
}

TEST_CASE("joints3d: the replay hash holds with joints") {
    auto run = [] {
        Scene s;
        const Entity d = s.door();
        Joint3D j;
        j.anchor = {-0.5f, 0, 0};
        j.motor = JointMotor3D::Velocity;
        j.motor_target = 45.0f;
        j.motor_max_force = 100.0f;
        s.w.add(d, j);
        RigidBody3D ball;
        ball.shape = BodyShape3D::Sphere;
        ball.radius = 0.2f;
        s.w.spawn_with(at(1.5f, 4, 0.5f), ball);
        s.run(240);
        return s.w.state_hash();
    };
    CHECK(run() == run());
}
