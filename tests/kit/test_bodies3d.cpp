// 3D bodies as components (M-DATA-REACH 6): RigidBody3D, CharacterController3D, Trigger3D on
// kit3d's Physics3D world.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
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
        install_bodies3d(w, nullptr);
        BoxCollider3D floor;
        floor.half = {20.0f, 0.5f, 20.0f};
        w.spawn_with(at(0, -0.5f, 0), floor);  // top at y = 0
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    const LocalTransform& pose(Entity e) { return *w.try_get<LocalTransform>(e); }
    Physics3D& phys() { return w.resource<Physics3D>(); }
};

}  // namespace

TEST_CASE("bodies3d: a dynamic body falls, lands and writes its pose back; outside moves teleport it") {
    Scene s;
    RigidBody3D box;
    box.half = {0.5f, 0.5f, 0.5f};
    const Entity e = s.w.spawn_with(at(0, 5, 0), box);
    s.run(1);
    CHECK(s.phys().world.has_body(e));
    s.run(30);
    CHECK(s.pose(e).position.y < 5.0f);
    CHECK(s.w.try_get<RigidBody3D>(e)->current_velocity.y < 0.0f);
    s.run(150);
    CHECK(s.pose(e).position.y == doctest::Approx(0.5f).epsilon(0.02));

    // Moved from outside (the editor, MCP): the body goes there and falls again.
    s.w.try_get_mut<LocalTransform>(e)->position = {3.0f, 4.0f, 0.0f};
    s.run(1);
    CHECK(s.pose(e).position.x == doctest::Approx(3.0f).epsilon(0.01));
    CHECK(s.pose(e).position.y > 3.5f);

    // An impulse, applied once.
    s.run(150);
    s.w.try_get_mut<RigidBody3D>(e)->impulse = {0.0f, 6000.0f, 0.0f};  // 1000 kg: 6 m/s up
    s.run(10);
    CHECK(s.pose(e).position.y > 1.0f);
    CHECK(s.w.try_get<RigidBody3D>(e)->impulse.y == 0.0f);

    // Destroying the entity removes the body.
    s.w.destroy(e);
    CHECK_FALSE(s.phys().world.has_body(e));
}

TEST_CASE("bodies3d: a rolling ball stops on set_velocity with stop_spin (a respawn)") {
    Scene s;
    RigidBody3D ball;
    ball.shape = BodyShape3D::Sphere;
    ball.radius = 0.5f;
    ball.velocity = {4.0f, 0.0f, 0.0f};
    ball.set_velocity = true;
    const Entity e = s.w.spawn_with(at(0, 0.5f, 0), ball);
    s.run(30);
    physics::BodyState3D st;
    REQUIRE(s.phys().world.body_state(e, st));
    CHECK(math::length(st.angular_velocity) > 1.0f);  // rolling
    // Back to the start, still: no velocity and no spin left over.
    RigidBody3D& b = *s.w.try_get_mut<RigidBody3D>(e);
    b.velocity = {};
    b.set_velocity = true;
    b.stop_spin = true;
    s.w.try_get_mut<LocalTransform>(e)->position = {0.0f, 0.5f, 0.0f};
    s.run(30);
    CHECK_FALSE(s.w.try_get<RigidBody3D>(e)->stop_spin);
    CHECK(std::fabs(s.pose(e).position.x) < 0.02f);
    REQUIRE(s.phys().world.body_state(e, st));
    CHECK(math::length(st.angular_velocity) < 0.05f);
}

TEST_CASE("bodies3d: kinematic bodies move by velocity; runtime colliders join the world") {
    Scene s;
    RigidBody3D k;
    k.type = BodyType3DKind::Kinematic;
    k.shape = BodyShape3D::Sphere;
    k.velocity = {2.0f, 0.0f, 0.0f};
    const Entity e = s.w.spawn_with(at(0, 3, 0), k);
    s.run(61);
    CHECK(s.pose(e).position.x == doctest::Approx(2.0f).epsilon(0.05));
    CHECK(s.pose(e).position.y == doctest::Approx(3.0f));

    BoxCollider3D wall;
    wall.half = {0.5f, 2.0f, 2.0f};
    const Entity we = s.w.spawn_with(at(10, 2, 0), wall);
    s.run(1);
    auto hit = s.phys().world.raycast({0, 1, 0}, {1, 0, 0}, 20.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->point.x == doctest::Approx(9.5f).epsilon(1e-3));
    s.w.try_get_mut<LocalTransform>(we)->position.x = 15.0f;  // moved: rebuilt there
    s.run(1);
    hit = s.phys().world.raycast({0, 1, 0}, {1, 0, 0}, 20.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->point.x == doctest::Approx(14.5f).epsilon(1e-3));
}

TEST_CASE("bodies3d: a character walks, stands on the floor and jumps") {
    Scene s;
    CharacterController3D c;
    c.move = {3.0f, 0.0f, 0.0f};
    const Entity e = s.w.spawn_with(at(0, 2, 0), c);
    s.run(90);
    const CharacterController3D& cc = *s.w.try_get<CharacterController3D>(e);
    CHECK(cc.grounded);
    CHECK(s.pose(e).position.y == doctest::Approx(0.9f).epsilon(0.05));  // capsule centre: half_height + radius
    CHECK(s.pose(e).position.x > 3.0f);
    s.w.try_get_mut<CharacterController3D>(e)->jump = true;
    s.run(10);
    CHECK_FALSE(s.w.try_get<CharacterController3D>(e)->grounded);
    CHECK(s.pose(e).position.y > 1.2f);
    s.run(90);
    CHECK(s.w.try_get<CharacterController3D>(e)->grounded);
}

TEST_CASE("bodies3d: Trigger3D raises enter and exit events for bodies passing through") {
    Scene s;
    Trigger3D tr;
    tr.half = {1.0f, 0.5f, 1.0f};
    const Entity zone = s.w.spawn_with(at(0, 3, 0), tr);
    RigidBody3D ball;
    ball.shape = BodyShape3D::Sphere;
    ball.radius = 0.25f;
    const Entity b = s.w.spawn_with(at(0, 6, 0), ball);
    u32 enters = 0, exits = 0;
    for (u32 i = 0; i < 180; ++i) {
        s.run(1);
        for (const TriggerEnter3D& ev : EventAccess(s.w).read<TriggerEnter3D>()) {
            CHECK(ev.trigger == zone);
            CHECK(ev.other == b);
            ++enters;
        }
        exits += static_cast<u32>(EventAccess(s.w).read<TriggerExit3D>().size());
    }
    CHECK(enters == 1);
    CHECK(exits == 1);
    CHECK(s.w.try_get<Trigger3D>(zone)->enters == 1);
    CHECK(s.w.try_get<Trigger3D>(zone)->inside == 0);
    CHECK(s.w.try_get<Trigger3D>(zone)->last_other == b);
}
