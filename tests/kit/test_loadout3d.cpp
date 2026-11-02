// Weapon switching (M-WEAPONSWITCH): presets are copied onto the single Weapon3D and ammo is kept per slot.

#include <doctest/doctest.h>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/input/ecs.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/loadout3d.h"
#include "mce/kit/seats3d.h"
#include "mce/string_id.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at() { return LocalTransform{{0, 0, 0}, math::quat::identity(), {1, 1, 1}}; }

struct Scene {
    World w;
    u64 t = 0;
    Entity pistol, smg, shotgun, player;
    Scene() {
        w.register_components<LocalTransform>();
        w.register_components<Rider3D>();
        install_combat3d(w, nullptr);
        install_loadout3d(w);
        WeaponPreset3D p;
        p.damage = 20;
        p.magazine = 12;
        p.reserve = 36;
        p.rate = 3;
        p.automatic = 0;
        pistol = w.spawn_with(at(), p);
        p.damage = 8;
        p.magazine = 30;
        p.reserve = 90;
        p.rate = 12;
        p.automatic = 1;
        smg = w.spawn_with(at(), p);
        p.damage = 9;
        p.pellets = 8;
        p.magazine = 6;
        p.reserve = 18;
        p.rate = 1.2f;
        p.spread_deg = 6;
        shotgun = w.spawn_with(at(), p);
        Loadout3D l;
        l.slot0 = pistol;
        l.slot1 = smg;
        l.slot2 = shotgun;
        player = w.spawn_with(at(), Weapon3D{}, l);
        run(2);
    }
    void run(u32 n, const char* action = nullptr) {
        for (u32 i = 0; i < n; ++i) {
            input::InputState in;
            in.tick = t;
            if (action && i == 0) {
                in.action_count = 1;
                in.action_names[0] = StringId(action);
                in.actions[0].held = in.actions[0].pressed = true;
            }
            input::publish(w, in);
            w.tick(kDt, t++);
        }
    }
    Weapon3D& gun() { return *w.try_get_mut<Weapon3D>(player); }
    Loadout3D& load() { return *w.try_get_mut<Loadout3D>(player); }
};

}  // namespace

TEST_CASE("loadout3d: the first slot is applied at the start, a never-used preset starts with a full magazine") {
    Scene s;
    CHECK(s.gun().damage == doctest::Approx(20.0f));
    CHECK(s.gun().magazine == 12);
    CHECK(s.gun().ammo == 12);
    CHECK(s.gun().reserve == 36);
    CHECK(s.gun().automatic == 0);
    CHECK(s.load().current == 0);
}

TEST_CASE("loadout3d: select switches, ammo and reserve are kept per slot, the draw time blocks the next shot") {
    Scene s;
    s.gun().ammo = 5;
    s.gun().reserve = 30;
    s.load().select = 3;  // the shotgun
    s.run(1);
    CHECK(s.load().current == 2);
    CHECK(s.load().select == 0);
    CHECK(s.gun().pellets == 8);
    CHECK(s.gun().damage == doctest::Approx(9.0f));
    CHECK(s.gun().ammo == 6);
    CHECK(s.gun().reserve == 18);
    CHECK(s.gun().cooldown > 0.3f);
    CHECK(s.w.try_get<WeaponPreset3D>(s.pistol)->ammo == 5);
    s.load().select = 1;
    s.run(1);
    CHECK(s.gun().ammo == 5);
    CHECK(s.gun().reserve == 30);
    CHECK(s.load().switches == 2);
}

TEST_CASE("loadout3d: a reload in progress is cancelled by a switch, and the equipment bonus stays on the damage") {
    Scene s;
    s.gun().reloading = 1;
    s.gun().reload_left = 1.0f;
    s.gun().bonus_applied = 2.0f;
    s.load().select = 2;
    s.run(1);
    CHECK(s.gun().reloading == 0);
    CHECK(s.gun().reload_left == 0.0f);
    CHECK(s.gun().damage == doctest::Approx(10.0f));
}

TEST_CASE("loadout3d: the Slot and WeaponNext / WeaponPrev actions switch, an empty slot is skipped") {
    Scene s;
    s.load().use_input = 1;
    s.run(1, "Slot2");
    CHECK(s.load().current == 1);
    s.w.destroy(s.shotgun);
    s.run(1, "WeaponNext");  // slot 2 is gone: back to the first
    CHECK(s.load().current == 0);
    s.run(1, "WeaponPrev");  // slot 2 is gone: the previous filled one is the SMG
    CHECK(s.load().current == 1);
    s.run(1, "Slot3");  // empty: nothing happens
    CHECK(s.load().current == 1);
}

TEST_CASE("loadout3d: a seated rider does not switch") {
    Scene s;
    Rider3D r;
    r.seated = 1;
    s.w.add(s.player, r);
    s.load().select = 2;
    s.run(2);
    CHECK(s.load().current == 0);
}
