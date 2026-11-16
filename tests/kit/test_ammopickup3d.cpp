// Ammo crates (M-AMMOPICKUP): top-up, the cap, stored presets, grenades, respawn, ignored holders.

#include <doctest/doctest.h>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/ammopickup3d.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/grenade3d.h"
#include "mce/kit/loadout3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

struct Scene {
    World w;
    u64 t = 0;
    u32 picked = 0, rounds = 0, grenades = 0;
    Scene() {
        w.register_components<LocalTransform>();
        install_combat3d(w, nullptr);
        install_ammopickup3d(w);
    }
    Entity holder(u32 reserve, f32 x = 0.0f) {
        Weapon3D wp;
        wp.magazine = 10;
        wp.reserve = reserve;
        return w.spawn_with(at(x, 0, 0), wp);
    }
    Entity crate(AmmoPickup3D a, f32 x = 0.5f) { return w.spawn_with(at(x, 0, 0), a); }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) {
            w.tick(kDt, t++);
            for (const AmmoPickedUp3D& e : EventAccess(w).read<AmmoPickedUp3D>()) {
                ++picked;
                rounds += e.rounds;
                grenades += e.grenades;
            }
        }
    }
    u32 reserve(Entity e) { return w.try_get<Weapon3D>(e)->reserve; }
};

}  // namespace

TEST_CASE("ammopickup3d: a holder in range gets the rounds and the crate is gone") {
    Scene s;
    const Entity h = s.holder(10);
    const Entity c = s.crate(AmmoPickup3D{});
    s.run(3);
    CHECK(s.reserve(h) == 40);
    CHECK(s.picked == 1);
    CHECK(s.rounds == 30);
    CHECK(!s.w.alive(c));
}

TEST_CASE("ammopickup3d: out of range, unarmed, infinite-ammo and magazine-less holders take nothing") {
    Scene s;
    const Entity far = s.holder(10, 20.0f);
    Weapon3D inf;
    inf.magazine = 10;
    inf.infinite_ammo = 1;
    inf.reserve = 5;
    const Entity infinite = s.w.spawn_with(at(0, 0, 0), inf);
    Weapon3D nomag;
    nomag.magazine = 0;
    nomag.reserve = 5;
    const Entity nm = s.w.spawn_with(at(0, 0, 0), nomag);
    const Entity c = s.crate(AmmoPickup3D{});
    s.run(5);
    CHECK(s.picked == 0);
    CHECK(s.w.alive(c));
    CHECK(s.reserve(far) == 10);
    CHECK(s.reserve(infinite) == 5);
    CHECK(s.reserve(nm) == 5);
}

TEST_CASE("ammopickup3d: the reserve is never filled past max_reserve, and a full holder leaves the crate") {
    Scene s;
    const Entity h = s.holder(140);
    AmmoPickup3D a;
    a.rounds = 30;
    a.max_reserve = 150;
    const Entity c = s.crate(a);
    s.run(3);
    CHECK(s.reserve(h) == 150);
    CHECK(s.rounds == 10);
    CHECK(!s.w.alive(c));
    Scene full;
    const Entity h2 = full.holder(150);
    const Entity c2 = full.crate(a);
    full.run(5);
    CHECK(full.picked == 0);
    CHECK(full.w.alive(c2));
    CHECK(full.reserve(h2) == 150);
}

TEST_CASE("ammopickup3d: stored presets of a Loadout3D are topped up too") {
    Scene s;
    WeaponPreset3D a;
    a.magazine = 10;
    a.reserve = 20;
    WeaponPreset3D b = a;
    b.reserve = 140;
    const Entity pa = s.w.spawn_with(at(0, 0, 0), a);
    const Entity pb = s.w.spawn_with(at(0, 0, 0), b);
    const Entity h = s.holder(0);
    Loadout3D lo;
    lo.slot0 = pa;
    lo.slot1 = pb;
    s.w.add(h, lo);
    s.crate(AmmoPickup3D{});
    s.run(3);
    CHECK(s.reserve(h) == 50);                                  // the wielded weapon: slot 0's 20 copied on, plus 30
    CHECK(s.w.try_get<WeaponPreset3D>(pb)->reserve == 150);     // capped
    CHECK(s.w.try_get<WeaponPreset3D>(pa)->reserve == 20);      // slot 0 is current: lives on the Weapon3D
}

TEST_CASE("ammopickup3d: grenades are added up to max_count") {
    Scene s;
    const Entity h = s.holder(150);  // rounds full: only the grenades count
    Grenade3D g;
    g.count = 4;
    g.max_count = 5;
    s.w.add(h, g);
    AmmoPickup3D a;
    a.grenades = 3;
    s.crate(a);
    s.run(3);
    CHECK(s.w.try_get<Grenade3D>(h)->count == 5);
    CHECK(s.grenades == 1);
    CHECK(s.picked == 1);
}

TEST_CASE("ammopickup3d: a crate with respawn waits and can be taken again") {
    Scene s;
    const Entity h = s.holder(0);
    AmmoPickup3D a;
    a.respawn = 1.0f;
    a.rounds = 10;
    const Entity c = s.crate(a);
    s.run(3);
    CHECK(s.reserve(h) == 10);
    CHECK(s.w.alive(c));
    s.run(30);  // inside the 1 s wait
    CHECK(s.picked == 1);
    s.run(40);
    CHECK(s.picked == 2);
    CHECK(s.reserve(h) == 20);
}
