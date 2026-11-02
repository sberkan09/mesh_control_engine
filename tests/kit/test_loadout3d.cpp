// Weapon switching (M-OPENWORLD-GAPS 2b): Loadout3D slots, input, nothing lost on a switch, the mirror, a lost slot.

#include <doctest/doctest.h>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/input/ecs.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/loadout3d.h"
#include "mce/string_id.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

struct Scene {
    World w;
    u64 t = 0;
    Entity holder, rifle, shotgun;
    Scene() {
        w.register_components<LocalTransform>();
        install_loadout3d(w);
        holder = w.spawn_with(at(0, 1, 0));
        Weapon3D r;
        r.owner = holder;
        r.magazine = 10;
        r.reserve = 40;
        r.rate = 10.0f;
        rifle = w.spawn_with(at(0, 1, 0), r);
        Weapon3D s;
        s.owner = holder;
        s.magazine = 4;
        s.reserve = 8;
        s.rate = 1.0f;
        s.pellets = 5;
        shotgun = w.spawn_with(at(0, 1, 0), s);
        Loadout3D l;
        l.slot0 = rifle;
        l.slot1 = shotgun;
        l.use_input = 1;
        w.add(holder, l);
    }
    // One tick with the named actions pressed (held too).
    void run(u32 n = 1, std::initializer_list<const char*> pressed = {}) {
        for (u32 i = 0; i < n; ++i) {
            input::InputState in;
            in.tick = t;
            u32 k = 0;
            for (const char* name : pressed) {
                in.action_names[k] = StringId(name);
                in.actions[k].held = in.actions[k].pressed = true;
                ++k;
            }
            in.action_count = k;
            input::publish(w, in);
            w.tick(kDt, t++);
        }
    }
    Loadout3D& lo() { return *w.try_get_mut<Loadout3D>(holder); }
    Weapon3D& wep(Entity e) { return *w.try_get_mut<Weapon3D>(e); }
};

}  // namespace

TEST_CASE("loadout3d: the actions switch the weapon in hand and only it gets the input") {
    Scene s;
    s.run();
    CHECK(s.lo().active == 0);
    CHECK(s.wep(s.rifle).use_input == 1);
    CHECK(s.wep(s.shotgun).use_input == 0);
    s.run(1, {"Weapon2"});
    CHECK(s.lo().active == 1);
    s.run();
    CHECK(s.wep(s.rifle).use_input == 0);
    CHECK(s.wep(s.shotgun).use_input == 1);
    s.run(1, {"NextWeapon"});  // wraps over the empty slots 2 and 3
    CHECK(s.lo().active == 0);
    s.run(1, {"PrevWeapon"});
    CHECK(s.lo().active == 1);
    s.run(1, {"Weapon4"});  // empty: ignored
    CHECK(s.lo().active == 1);
    s.lo().select = 1;  // a script
    s.run();
    CHECK(s.lo().active == 0);
    CHECK(s.lo().select == 0);
}

TEST_CASE("loadout3d: a switch loses nothing and the drawn weapon waits") {
    Scene s;
    s.run();
    s.wep(s.rifle).ammo = 3;
    s.wep(s.rifle).reserve = 17;
    s.wep(s.rifle).reloading = 1;
    s.wep(s.rifle).reload_left = 5.0f;
    s.run(1, {"Weapon2"});
    s.run(10);
    s.run(1, {"Weapon1"});
    s.run(3);
    const Weapon3D& r = s.wep(s.rifle);
    CHECK(r.ammo == 3);
    CHECK(r.reserve == 17);
    CHECK(r.reloading == 1);  // still reloading (the reload of a weapon in the pack carries on)
    CHECK(s.wep(s.shotgun).ammo == 4);
    // The rifle was just drawn: it waits switch_time before it can shoot.
    CHECK(r.cooldown > 0.2f);
    s.run(60);
    CHECK(s.wep(s.rifle).cooldown <= 0.0f);
}

TEST_CASE("loadout3d: a weapon in the pack cannot fire and the loadout mirrors the one in hand") {
    Scene s;
    s.run();
    s.wep(s.shotgun).fire = 1;
    s.run();
    CHECK(s.wep(s.shotgun).fire == 0);
    CHECK(s.wep(s.shotgun).shots == 0);
    CHECK(s.lo().ammo == 10);
    CHECK(s.lo().magazine == 10);
    CHECK(s.lo().reserve == 40);
    s.run(1, {"Weapon2"});
    s.run();
    CHECK(s.lo().ammo == 4);
    CHECK(s.lo().reserve == 8);
}

TEST_CASE("loadout3d: a destroyed weapon in hand hands over to the next one; no weapons mirror zero") {
    Scene s;
    s.run();
    s.w.destroy(s.rifle);
    s.run(2);
    CHECK(s.lo().active == 1);
    CHECK(s.wep(s.shotgun).use_input == 1);
    CHECK(held_weapon(s.w, s.holder) == &s.wep(s.shotgun));
    Weapon3D* all[8];
    CHECK(weapons_of(s.w, s.holder, all, 8) == 1);
    s.w.destroy(s.shotgun);
    s.run(2);
    CHECK(held_weapon(s.w, s.holder) == nullptr);
    CHECK(s.lo().ammo == 0);
    CHECK(s.lo().reserve == 0);
}

TEST_CASE("loadout3d: a holder with no loadout uses its own weapon") {
    Scene s;
    const Entity solo = s.w.spawn_with(at(5, 1, 0), Weapon3D{});
    Weapon3D* all[8];
    CHECK(held_weapon(s.w, solo) == &s.wep(solo));
    CHECK(weapons_of(s.w, solo, all, 8) == 1);
    CHECK(weapons_of(s.w, s.holder, all, 8) == 2);
    CHECK(weapons_of(s.w, s.holder, all, 1) == 1);
}

TEST_CASE("loadout3d: the active slot is part of the world hash and two runs agree") {
    const auto play = [](bool switched) {
        Scene s;
        s.run(5);
        if (switched) s.run(1, {"Weapon2"});
        s.run(60);
        return s.w.state_hash();
    };
    CHECK(play(true) == play(true));
    CHECK(play(true) != play(false));
}
