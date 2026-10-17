// Getting in and out of vehicles (M-OPENWORLD C1): a Rider3D character boards a VehicleSeat3D car
// with the Interact action or `enter`, drives it with the player's input, steps out, and the
// refusals (locked, too far, too fast, taken), the cleanup (car or rider destroyed) and the replay hash.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/input/ecs.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/effects.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/seats3d.h"
#include "mce/kit/vehicles3d.h"
#include "mce/string_id.h"

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
    Scene() {
        w.register_components<LocalTransform>();
        install_seats3d(w, nullptr);
        BoxCollider3D road;
        road.half = {300.0f, 0.5f, 300.0f};
        road.friction = 1.0f;
        w.spawn_with(at(0, -0.5f, 0), road);
    }
    // `interact` presses the Interact action on the first of these ticks.
    void run(u32 n, bool interact = false) {
        for (u32 i = 0; i < n; ++i) {
            input::InputState in;
            in.tick = t;
            in.move = {move.x, move.y};
            if (interact && i == 0) {
                in.action_count = 1;
                in.action_names[0] = StringId("Interact");
                in.actions[0].held = in.actions[0].pressed = true;
            }
            input::publish(w, in);
            w.tick(kDt, t++);
        }
    }
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
        return w.spawn_with(at(x, 1.2f, 0), b, v, VehicleSeat3D{});
    }
    Entity rider(f32 x, f32 z = 0.0f) {
        Rider3D r;
        r.use_input = 1;
        CharacterController3D c;
        c.speed = 7.0f;
        return w.spawn_with(at(x, 1.0f, z), c, r);
    }
    vec3 pos(Entity e) { return w.try_get<LocalTransform>(e)->position; }
    Vehicle3D& veh(Entity e) { return *w.try_get_mut<Vehicle3D>(e); }
    Rider3D& rid(Entity e) { return *w.try_get_mut<Rider3D>(e); }
    VehicleSeat3D& seat(Entity e) { return *w.try_get_mut<VehicleSeat3D>(e); }
};

}  // namespace

TEST_CASE("seats3d: a rider boards with Interact, drives the car, and steps out") {
    Scene s;
    const Entity car = s.car();
    const Entity me = s.rider(-2.0f);
    s.run(60);  // settle
    REQUIRE(s.w.has<CharacterController3D>(me));
    CHECK_FALSE(s.rid(me).seated);

    s.run(2, true);
    CHECK(s.rid(me).seated);
    CHECK(s.rid(me).vehicle == car);
    CHECK(s.seat(car).driver == me);
    CHECK(s.veh(car).use_input == 1);
    CHECK_FALSE(s.w.has<CharacterController3D>(me));

    // Drive forward (Move y): the rider rides the seat.
    s.move = {0.0f, 1.0f};
    s.run(120);
    CHECK(s.veh(car).speed > 3.0f);
    CHECK(math::length(s.pos(me) - s.pos(car)) < 1.0f);

    // Too fast to get out.
    s.run(2, true);
    CHECK(s.rid(me).seated);

    // Let it roll to a stop, then step out.
    s.move = {0.0f, 0.0f};
    s.veh(car).brake = 1.0f;
    s.run(240);
    CHECK(std::fabs(s.veh(car).speed) < 1.0f);
    s.run(2, true);
    CHECK_FALSE(s.rid(me).seated);
    CHECK(s.seat(car).driver.is_null());
    CHECK(s.veh(car).use_input == 0);
    CHECK(s.veh(car).handbrake == doctest::Approx(1.0f));
    s.run(2);
    CHECK(s.w.has<CharacterController3D>(me));
    CHECK(math::length(s.pos(me) - s.pos(car)) > 1.0f);
    CHECK(s.w.try_get<CharacterController3D>(me)->speed == doctest::Approx(7.0f));  // the settings came back
}

TEST_CASE("seats3d: refusals - too far, locked, taken") {
    Scene s;
    const Entity car = s.car();
    const Entity far_away = s.rider(-12.0f);
    s.run(30);
    s.rid(far_away).enter = 1;
    s.run(2);
    CHECK_FALSE(s.rid(far_away).seated);
    CHECK(s.rid(far_away).enter == 0);

    const Entity me = s.rider(-2.0f);
    s.run(30);
    s.seat(car).locked = 1;
    s.rid(me).enter = 1;
    s.run(2);
    CHECK_FALSE(s.rid(me).seated);

    s.seat(car).locked = 0;
    s.rid(me).enter = 1;
    s.run(2);
    CHECK(s.rid(me).seated);

    const Entity other = s.rider(2.0f);
    s.run(30);
    s.rid(other).enter = 1;
    s.run(2);
    CHECK_FALSE(s.rid(other).seated);
    CHECK(s.seat(car).driver == me);
}

TEST_CASE("seats3d: an AI driver is switched off while someone sits and back on after") {
    Scene s;
    const Entity car = s.car();
    VehicleDriver3D d;
    s.w.add<VehicleDriver3D>(car, d);
    const Entity me = s.rider(-2.0f);
    s.run(30);
    s.rid(me).enter = 1;
    s.run(2);
    REQUIRE(s.rid(me).seated);
    CHECK(s.w.try_get<VehicleDriver3D>(car)->drive == 0);
    s.rid(me).enter = 1;
    s.run(2);
    CHECK_FALSE(s.rid(me).seated);
    CHECK(s.w.try_get<VehicleDriver3D>(car)->drive == 1);
}

TEST_CASE("seats3d: the seat is freed when the car or the rider is destroyed") {
    {
        Scene s;
        const Entity car = s.car();
        const Entity me = s.rider(-2.0f);
        s.run(30);
        s.rid(me).enter = 1;
        s.run(2);
        REQUIRE(s.rid(me).seated);
        s.w.destroy(car);
        s.run(3);
        CHECK_FALSE(s.rid(me).seated);
        CHECK(s.w.has<CharacterController3D>(me));
    }
    {
        Scene s;
        const Entity car = s.car();
        const Entity me = s.rider(-2.0f);
        s.run(30);
        s.rid(me).enter = 1;
        s.run(2);
        REQUIRE(s.seat(car).driver == me);
        s.w.destroy(me);
        s.run(2);
        CHECK(s.seat(car).driver.is_null());
        CHECK(s.veh(car).use_input == 0);
    }
}

TEST_CASE("seats3d: a follow camera follows the car while seated") {
    Scene s;
    const Entity car = s.car();
    const Entity cam = s.w.spawn_with(at(0, 3, 8), CameraFollow3D{});
    const Entity me = s.rider(-2.0f);
    s.rid(me).camera = cam;
    s.w.try_get_mut<CameraFollow3D>(cam)->target = me;
    s.run(30);
    s.rid(me).enter = 1;
    s.run(2);
    CHECK(s.w.try_get<CameraFollow3D>(cam)->target == car);
    s.rid(me).enter = 1;
    s.run(2);
    CHECK(s.w.try_get<CameraFollow3D>(cam)->target == me);
}

TEST_CASE("seats3d: the replay hash holds with boarding and driving") {
    auto run = [] {
        Scene s;
        s.car();
        const Entity me = s.rider(-2.0f);
        s.run(30);
        s.run(2, true);
        s.move = {0.3f, 1.0f};
        s.run(150);
        s.move = {};
        s.w.try_get_mut<Vehicle3D>(s.w.try_get<Rider3D>(me)->vehicle)->brake = 1.0f;
        s.run(200);
        s.run(2, true);
        s.run(60);
        return s.w.state_hash();
    };
    CHECK(run() == run());
}
