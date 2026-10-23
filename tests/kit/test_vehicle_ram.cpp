// Vehicle ram (M-VEHICLEHIT): a moving car hurts the Health3D bodies in the zone in front of it.

#include <doctest/doctest.h>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/seats3d.h"
#include "mce/kit/vehicle_ram.h"
#include "mce/kit/vehicles3d.h"

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
        install_vehicle_ram(w, nullptr);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    // Forward is -Z: the car sits at the origin.
    Entity car(f32 speed) {
        Vehicle3D v;
        v.speed = speed;
        return w.spawn_with(at(0, 0, 0), v, VehicleRam3D{});
    }
    Entity victim(f32 x, f32 z) {
        Health3D h;
        h.team = 3;
        return w.spawn_with(at(x, 0.0f, z), h);
    }
    const Health3D& h(Entity e) { return *w.try_get<Health3D>(e); }
    f32 hp(Entity e) { return h(e).current; }
    f32 full(Entity e) { return h(e).max; }
};

}  // namespace

TEST_CASE("vehicle ram: a fast car hurts a body in front of it") {
    Scene s;
    s.car(15.0f);
    Entity v = s.victim(0.0f, -2.4f);
    f32 before = s.hp(v);
    s.run(2);
    CHECK(s.hp(v) < before);
}

TEST_CASE("vehicle ram: slow, behind, to the side, or too far means no damage") {
    Scene s;
    s.car(2.0f);
    Entity slow = s.victim(0.0f, -2.4f);
    s.run(2);
    CHECK(s.hp(slow) == doctest::Approx(s.full(slow)));

    Scene f;
    f.car(15.0f);
    Entity behind = f.victim(0.0f, 2.4f);
    Entity side = f.victim(3.0f, -2.4f);
    Entity far = f.victim(0.0f, -8.0f);
    f.run(2);
    CHECK(f.hp(behind) == doctest::Approx(f.full(behind)));
    CHECK(f.hp(side) == doctest::Approx(f.full(side)));
    CHECK(f.hp(far) == doctest::Approx(f.full(far)));
}

TEST_CASE("vehicle ram: reversing hits what is behind the car") {
    Scene s;
    s.car(-15.0f);
    Entity behind = s.victim(0.0f, 2.4f);
    Entity front = s.victim(0.0f, -2.4f);
    s.run(2);
    CHECK(s.hp(behind) < s.full(behind));
    CHECK(s.hp(front) == doctest::Approx(s.full(front)));
}

TEST_CASE("vehicle ram: the same victim is hit once a second") {
    Scene s;
    Entity c = s.car(15.0f);
    Entity v = s.victim(0.0f, -2.4f);
    // The vehicle state system rewrites speed on a car without a body, so hold it each tick.
    auto drive = [&](u32 n) {
        for (u32 i = 0; i < n; ++i) {
            s.w.try_get_mut<Vehicle3D>(c)->speed = 15.0f;
            s.run(1);
        }
    };
    drive(2);
    f32 once = s.hp(v);
    CHECK(once < s.full(v));
    s.w.try_get_mut<Health3D>(v)->current = 1000.0f;
    s.w.try_get_mut<Health3D>(v)->max = 1000.0f;
    drive(20);
    CHECK(s.hp(v) == doctest::Approx(1000.0f));
    drive(60);
    CHECK(s.hp(v) < 1000.0f);
}

TEST_CASE("vehicle ram: the driver is the instigator and is spared") {
    Scene s;
    Entity c = s.car(15.0f);
    Health3D dh;
    dh.team = 1;
    Entity driver = s.w.spawn_with(at(0.0f, 0.0f, -2.4f), dh);
    VehicleSeat3D seat;
    seat.driver = driver;
    s.w.add(c, seat);
    Entity v = s.victim(0.5f, -2.4f);
    s.run(2);
    CHECK(s.hp(driver) == doctest::Approx(s.full(driver)));
    CHECK(s.hp(v) < s.full(v));
    CHECK(s.h(v).last_attacker == driver);
}
