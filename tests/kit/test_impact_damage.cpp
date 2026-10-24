// Crash damage (M-CONTACTEVENTS 4): ImpactDamage3D turns ContactBegin3D into Health3D damage.

#include <doctest/doctest.h>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/impact_damage.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/seats3d.h"

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
        install_impact_damage(w, nullptr);
        BoxCollider3D floor;
        floor.half = {20.0f, 0.5f, 20.0f};
        w.spawn_with(at(0, -0.5f, 0), floor);  // top at y = 0
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    const Health3D& hp(Entity e) { return *w.try_get<Health3D>(e); }
};

RigidBody3D opted_in() {
    RigidBody3D b;
    b.contact_events = 1;
    return b;
}

}  // namespace

TEST_CASE("impact damage: a body hurt by its own landing loses health once") {
    Scene s;
    Health3D h;
    const Entity e = s.w.spawn_with(at(0, 3.5f, 0), opted_in(), h, ImpactDamage3D{});
    s.run(240);
    CHECK(s.hp(e).current < h.max);
    CHECK(s.hp(e).hits == 1);
}

TEST_CASE("impact damage: a slow touch, a disabled component or no ImpactDamage3D takes nothing") {
    Scene s;
    Health3D h;
    ImpactDamage3D tough;
    tough.min_speed = 100.0f;
    ImpactDamage3D off;
    off.enabled = 0;
    const Entity a = s.w.spawn_with(at(0, 3.5f, 0), opted_in(), h, tough);
    const Entity b = s.w.spawn_with(at(4, 3.5f, 0), opted_in(), h, off);
    const Entity c = s.w.spawn_with(at(8, 3.5f, 0), opted_in(), h);
    s.run(240);
    CHECK(s.hp(a).hits == 0);
    CHECK(s.hp(b).hits == 0);
    CHECK(s.hp(c).hits == 0);
}

TEST_CASE("impact damage: the driver of the other body is the instigator") {
    Scene s;
    Health3D h;
    h.max = 1000.0f;
    h.current = 1000.0f;
    const Entity victim = s.w.spawn_with(at(0, 0.5f, 0), RigidBody3D{}, h, ImpactDamage3D{});
    const Entity driver = s.w.spawn_with(at(30, 1, 0), Health3D{});
    VehicleSeat3D seat;
    seat.driver = driver;
    s.w.spawn_with(at(0, 6.0f, 0), opted_in(), seat);  // falls onto the victim
    s.run(240);
    REQUIRE(s.hp(victim).hits >= 1);
    CHECK(s.hp(victim).last_attacker == driver);
    CHECK(s.hp(victim).current < 1000.0f);
}

TEST_CASE("impact damage: the driver takes their share of a crash, and none with occupant_share 0") {
    for (const f32 share : {0.5f, 0.0f}) {
        Scene s;
        Health3D h;
        const Entity driver = s.w.spawn_with(at(30, 1, 0), h);
        VehicleSeat3D seat;
        seat.driver = driver;
        ImpactDamage3D dmg;
        dmg.occupant_share = share;
        const Entity car = s.w.spawn_with(at(0, 3.5f, 0), opted_in(), h, dmg, seat);
        s.run(240);
        REQUIRE(s.hp(car).hits == 1);
        const f32 lost_car = h.max - s.hp(car).current;
        const f32 lost_driver = h.max - s.hp(driver).current;
        CHECK(lost_driver == doctest::Approx(lost_car * share).epsilon(0.01));
    }
}
