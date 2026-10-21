// Vehicles as a component (M-GAMEPLAY3D step 4): Vehicle3D on a RigidBody3D: driving with the
// component's inputs and with the player's input, the throttle braking against the motion, wheel
// models following the wheels, rebuilds, refusals, removal, and the replay hash.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/input/ecs.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/vehicles3d.h"

using namespace mce;
using namespace mce::kit;
using math::vec3;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

struct Scene {
    World w;
    u64 t = 0;
    math::vec2 move{};
    bool handbrake = false;
    Scene() {
        w.register_components<LocalTransform>();
        install_vehicles3d(w, nullptr);
        BoxCollider3D road;
        road.half = {300.0f, 0.5f, 300.0f};
        road.friction = 1.0f;
        w.spawn_with(at(0, -0.5f, 0), road);
    }
    void run(u32 n, bool with_input = false) {
        for (u32 i = 0; i < n; ++i) {
            if (with_input) {
                input::InputState in;
                in.tick = t;
                in.move = {move.x, move.y};
                in.jump_held = handbrake;
                input::publish(w, in);
            }
            w.tick(kDt, t++);
        }
    }
    // A 1.2 t car body (1.8 x 0.8 x 4 m) facing -Z.
    Entity car(f32 x = 0.0f) {
        RigidBody3D b;
        b.half = {0.9f, 0.4f, 2.0f};
        b.density = 1200.0f / (8.0f * 0.9f * 0.4f * 2.0f);
        Vehicle3D v;
        v.front_axle = -1.4f;
        v.rear_axle = 1.4f;
        v.track = 0.9f;
        v.wheel_y = -0.3f;
        v.drive = VehicleDrive3D::All;
        return w.spawn_with(at(x, 1.2f, 0), b, v);
    }
    Vehicle3D& veh(Entity e) { return *w.try_get_mut<Vehicle3D>(e); }
    vec3 pos(Entity e) { return w.try_get<LocalTransform>(e)->position; }
    Physics3D& phys() { return w.resource<Physics3D>(); }
};

}  // namespace

TEST_CASE("vehicles3d: a car drives, its wheel models follow, it brakes and reverses") {
    Scene s;
    const Entity car = s.car();
    const Entity fl = s.w.spawn_with(at(0, 0, 0));
    const Entity rr = s.w.spawn_with(at(0, 0, 0));
    s.veh(car).wheel_fl = fl;
    s.veh(car).wheel_rr = rr;
    s.run(90);
    CHECK(s.phys().world.stats().vehicles == 1);
    CHECK(s.veh(car).wheels_on_ground == 4);
    // The wheel models sit on the wheels: front-left is ahead (-Z) and to the left (-X).
    CHECK(s.pos(fl).x == doctest::Approx(-0.9f).epsilon(0.05));
    CHECK(s.pos(fl).z == doctest::Approx(-1.4f).epsilon(0.05));
    CHECK(s.pos(fl).y == doctest::Approx(0.35f).epsilon(0.05));
    CHECK(s.pos(rr).x == doctest::Approx(0.9f).epsilon(0.05));
    CHECK(s.pos(rr).z == doctest::Approx(1.4f).epsilon(0.05));

    s.veh(car).throttle = 1.0f;
    s.run(180);
    CHECK(s.veh(car).speed > 8.0f);
    CHECK(s.veh(car).gear >= 1);
    CHECK(s.veh(car).rpm > 1000.0f);
    CHECK(s.pos(car).z < -10.0f);
    CHECK(s.pos(fl).z < s.pos(car).z);  // the wheel models came along

    // Throttle the other way: it brakes first (still in a forward gear), then reverses.
    s.veh(car).throttle = -1.0f;
    const f32 fast = s.veh(car).speed;
    s.run(30);
    CHECK(s.veh(car).speed < fast - 3.0f);
    CHECK(s.veh(car).gear >= 0);
    s.run(300);
    CHECK(s.veh(car).speed < -1.0f);
    CHECK(s.veh(car).gear < 0);
}

TEST_CASE("vehicles3d: the player's Move steers and drives it; Jump is the handbrake") {
    Scene s;
    const Entity car = s.car();
    s.veh(car).use_input = 1;
    s.run(60, true);
    s.move = {0.3f, 1.0f};  // throttle, a little right
    s.run(180, true);
    CHECK(s.veh(car).speed > 5.0f);
    CHECK(s.pos(car).x > 1.0f);  // drifted right of its line
    s.move = {0.0f, 0.0f};
    s.handbrake = true;
    s.run(240, true);
    CHECK(std::fabs(s.veh(car).speed) < 0.5f);
}

TEST_CASE("vehicles3d: rebuilt on a settings change; a kinematic body is refused once; removal") {
    Scene s;
    const Entity car = s.car();
    s.run(60);
    CHECK(s.phys().world.stats().vehicles == 1);
    const Entity fl = s.w.spawn_with(at(0, 0, 0));
    s.veh(car).wheel_fl = fl;  // not a rebuild
    s.veh(car).track = 1.1f;   // a rebuild: wider
    s.run(30);
    CHECK(s.phys().world.stats().vehicles == 1);
    CHECK(s.pos(fl).x == doctest::Approx(s.pos(car).x - 1.1f).epsilon(0.05));

    RigidBody3D lift;
    lift.type = BodyType3DKind::Kinematic;
    const Entity bad = s.w.spawn_with(at(10, 1, 0), lift, Vehicle3D{});
    s.run(3);
    CHECK(s.phys().world.stats().vehicles == 1);
    CHECK(s.w.alive(bad));

    s.w.remove<Vehicle3D>(car);
    s.run(1);
    CHECK(s.phys().world.stats().vehicles == 0);
    const Entity other = s.car(-6.0f);
    s.run(2);
    CHECK(s.phys().world.stats().vehicles == 1);
    s.w.destroy(other);
    s.run(1);
    CHECK(s.phys().world.stats().vehicles == 0);
}

TEST_CASE("vehicles3d: the replay hash holds with vehicles") {
    auto run = [] {
        Scene s;
        const Entity a = s.car();
        const Entity b = s.car(5.0f);
        s.veh(a).throttle = 1.0f;
        s.veh(a).steer = 0.4f;
        s.veh(b).drive = VehicleDrive3D::Rear;
        s.veh(b).throttle = 0.8f;
        s.run(200);
        s.veh(b).handbrake = 1.0f;
        s.run(100);
        return s.w.state_hash();
    };
    CHECK(run() == run());
}
