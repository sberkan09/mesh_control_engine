// Reload and dry-fire events (M-WEAPONFEEL): started and finished once per reload, a dry pull clicks once per fire
// interval.

#include <doctest/doctest.h>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/weapon_events3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

struct Scene {
    World w;
    u64 t = 0;
    u32 started = 0, reloaded = 0, dry = 0, last_rounds = 0;
    Entity last_owner;
    Scene() {
        w.register_components<LocalTransform>();
        install_combat3d(w, nullptr);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) {
            w.tick(kDt, t++);
            auto ev = EventAccess(w);
            started += static_cast<u32>(ev.read<WeaponReloadStarted3D>().size());
            for (const WeaponReloaded3D& e : ev.read<WeaponReloaded3D>()) {
                ++reloaded;
                last_rounds = e.rounds;
                last_owner = e.owner;
            }
            dry += static_cast<u32>(ev.read<WeaponDry3D>().size());
        }
    }
    void reset_counts() { started = reloaded = dry = 0; }
    Entity gun(Weapon3D g) { return w.spawn_with(LocalTransform{{0, 1, 0}, math::quat::identity(), {1, 1, 1}}, g); }
    Weapon3D& wep(Entity e) { return *w.try_get_mut<Weapon3D>(e); }
};

Weapon3D rifle() {
    Weapon3D g;
    g.magazine = 6;
    g.ammo = 2;
    g.reserve = 10;
    g.reload_time = 0.5f;
    g.rate = 10.0f;
    return g;
}

}  // namespace

TEST_CASE("weapon events: a manual reload starts and ends once with the rounds taken") {
    Scene s;
    Weapon3D g = rifle();
    const Entity owner = s.w.spawn_with(LocalTransform{});
    g.owner = owner;
    const Entity gun = s.gun(g);
    s.run(2);
    s.wep(gun).reload = 1;
    s.run(1);
    CHECK(s.started == 1);
    CHECK(s.reloaded == 0);
    s.run(40);
    CHECK(s.started == 1);
    CHECK(s.reloaded == 1);
    CHECK(s.last_rounds == 4);
    CHECK(s.last_owner == owner);
    CHECK(s.dry == 0);
}

TEST_CASE("weapon events: an empty magazine reloads on its own and a full one does not start a reload") {
    Scene s;
    Weapon3D g = rifle();
    const Entity gun = s.gun(g);
    s.run(1);  // primed: a gun with 0 rounds is filled on its first tick, so empty it afterwards
    s.wep(gun).ammo = 0;
    s.run(1);
    CHECK(s.started == 1);
    s.run(40);
    CHECK(s.reloaded == 1);
    CHECK(s.last_rounds == 6);
    CHECK(s.last_owner == gun);  // no owner set: the weapon's own entity

    s.wep(gun).reload = 1;  // full: nothing to load
    s.run(5);
    CHECK(s.started == 1);
}

TEST_CASE("weapon events: a zero reload time still reports both events") {
    Scene s;
    Weapon3D g = rifle();
    g.reload_time = 0.0f;
    const Entity gun = s.gun(g);
    s.wep(gun).reload = 1;
    s.run(2);
    CHECK(s.started == 1);
    CHECK(s.reloaded == 1);
}

TEST_CASE("weapon events: a dry pull clicks once per fire interval and only when it cannot reload") {
    Scene s;
    Weapon3D g = rifle();
    g.reserve = 0;
    const Entity gun = s.gun(g);
    s.run(1);
    s.wep(gun).ammo = 0;
    s.run(1);
    CHECK(s.started == 0);  // nothing to load
    s.wep(gun).fire = 1;
    s.run(1);
    CHECK(s.dry == 1);
    s.wep(gun).fire = 1;
    s.run(1);  // inside the 0.1 s interval
    CHECK(s.dry == 1);
    s.run(8);
    s.wep(gun).fire = 1;
    s.run(1);
    CHECK(s.dry == 2);
}

TEST_CASE("weapon events: a gun with spare rounds never reports dry, and neither does one that never runs dry") {
    Scene s;
    Weapon3D g = rifle();
    const Entity gun = s.gun(g);
    s.run(1);
    s.wep(gun).ammo = 0;
    s.run(1);
    s.reset_counts();
    s.wep(gun).fire = 1;
    s.run(1);  // starts reloading instead
    CHECK(s.dry == 0);

    Weapon3D inf;  // magazine 0
    const Entity pistol = s.gun(inf);
    s.wep(pistol).fire = 1;
    s.run(2);
    CHECK(s.dry == 0);
}
