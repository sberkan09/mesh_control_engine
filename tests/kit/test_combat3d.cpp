// Weapons, health and damage (M-OPENWORLD C2): hitscan and projectile Weapon3D against Health3D
// targets - damage, armor, teams, rate of fire, magazines and reloads, death and ragdoll, stagger,
// regen, lingering corpses, splash, the Fire input - and the replay hash.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/input/ecs.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/ragdolls3d.h"
#include "mce/string_id.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

struct Scene {
    World w;
    u64 t = 0;
    bool fire_held = false, reload_pressed = false;
    u32 fired = 0, hits = 0, damages = 0, deaths = 0;
    f32 damage_total = 0.0f;
    Entity last_killer;
    Scene() {
        w.register_components<LocalTransform>();
        install_combat3d(w, nullptr);
        BoxCollider3D ground;
        ground.half = {300.0f, 0.5f, 300.0f};
        w.spawn_with(at(0, -0.5f, 0), ground);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) {
            input::InputState in;
            in.tick = t;
            u32 k = 0;
            if (fire_held) {
                in.action_names[k] = StringId("Fire");
                in.actions[k].held = in.actions[k].pressed = true;
                ++k;
            }
            if (reload_pressed) {
                in.action_names[k] = StringId("Reload");
                in.actions[k].held = in.actions[k].pressed = true;
                ++k;
            }
            in.action_count = k;
            input::publish(w, in);
            w.tick(kDt, t++);
            tally();
        }
    }
    void tally() {
        auto ev = EventAccess(w);
        fired += static_cast<u32>(ev.read<WeaponFired3D>().size());
        hits += static_cast<u32>(ev.read<WeaponHit3D>().size());
        for (const Damage3D& d : ev.read<Damage3D>()) {
            ++damages;
            damage_total += d.amount;
        }
        for (const Died3D& d : ev.read<Died3D>()) {
            ++deaths;
            last_killer = d.killer;
        }
    }
    // A standing character 10 m down -Z with health.
    Entity target(f32 x = 0.0f, f32 z = -10.0f, f32 hp = 100.0f) {
        Health3D h;
        h.max = h.current = hp;
        CharacterController3D c;
        return w.spawn_with(at(x, 1.0f, z), c, h);
    }
    Entity gun(Weapon3D g = {}) { return w.spawn_with(at(0, 1.0f, 0), g); }
    Health3D& hp(Entity e) { return *w.try_get_mut<Health3D>(e); }
    Weapon3D& wep(Entity e) { return *w.try_get_mut<Weapon3D>(e); }
};

}  // namespace

TEST_CASE("combat3d: a hitscan shot hurts what it hits and says so") {
    Scene s;
    const Entity tgt = s.target();
    Weapon3D g;
    g.damage = 30.0f;
    const Entity gun = s.gun(g);
    s.run(30);  // the target settles
    s.wep(gun).fire = 1;
    s.run(1);
    CHECK(s.fired == 1);
    CHECK(s.hits == 1);
    CHECK(s.damages == 1);
    CHECK(s.hp(tgt).current == doctest::Approx(70.0f));
    CHECK(s.hp(tgt).hits == 1);
    CHECK(s.wep(gun).fire == 0);
    CHECK(s.wep(gun).last_hit == tgt);
    CHECK(s.wep(gun).last_point.z < -9.0f);
    CHECK(s.hp(tgt).last_attacker == gun);
}

TEST_CASE("combat3d: a weapon aims from another entity and falls back to its own pose when that is gone") {
    Scene s;
    const Entity tgt = s.target(-10.0f, 0.0f);  // 10 m down -X
    Weapon3D g;
    g.damage = 30.0f;
    const Entity gun = s.gun(g);  // faces -Z: would miss the target
    // The aim entity looks along -X (a quarter turn left).
    LocalTransform cam = at(0, 1.0f, 0);
    cam.rotation = math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 1.5707963f);
    const Entity aim = s.w.spawn_with(cam);
    s.run(30);
    s.wep(gun).aim_from = aim;
    s.wep(gun).fire = 1;
    s.run(1);
    CHECK(s.hp(tgt).current == doctest::Approx(70.0f));
    CHECK(s.wep(gun).last_hit == tgt);
    // The aim entity is destroyed (the player left the car and the camera changed): the shot uses the weapon's own pose.
    s.w.destroy(aim);
    s.wep(gun).cooldown = 0.0f;
    s.wep(gun).fire = 1;
    s.run(1);
    CHECK(s.fired == 2);
    CHECK(s.hp(tgt).current == doctest::Approx(70.0f));  // faced -Z: missed
}

TEST_CASE("combat3d: an aim entity with no transform, or a handle whose slot was reused, is not used") {
    Scene s;
    const Entity tgt = s.target(-10.0f, 0.0f);  // 10 m down -X: only a turned aim would hit it
    Weapon3D g;
    g.damage = 30.0f;
    const Entity gun = s.gun(g);
    const Entity bare = s.w.spawn();  // alive, but no LocalTransform
    s.run(30);
    s.wep(gun).aim_from = bare;
    s.wep(gun).fire = 1;
    s.run(1);
    CHECK(s.fired == 1);
    CHECK(s.hp(tgt).current == doctest::Approx(100.0f));  // its own pose: faced -Z, missed
    // A turned aim entity is destroyed and its slot reused by an entity that faces the other way: the old handle must not match.
    LocalTransform cam = at(0, 1.0f, 0);
    cam.rotation = math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 1.5707963f);
    const Entity aim = s.w.spawn_with(cam);
    s.w.destroy(aim);
    const Entity reused = s.w.spawn_with(cam);
    CHECK(reused != aim);
    s.wep(gun).aim_from = aim;
    s.wep(gun).cooldown = 0.0f;
    s.wep(gun).fire = 1;
    s.run(1);
    CHECK(s.fired == 2);
    CHECK(s.hp(tgt).current == doctest::Approx(100.0f));
}

TEST_CASE("combat3d: the owner is never hit and a wall stops the ray") {
    Scene s;
    const Entity tgt = s.target();
    Weapon3D g;
    BoxCollider3D wall;
    wall.half = {5.0f, 5.0f, 0.2f};
    s.w.spawn_with(at(0, 2, -5), wall);
    const Entity gun = s.gun(g);
    s.run(30);
    s.wep(gun).fire = 1;
    s.run(1);
    CHECK(s.hits == 1);
    CHECK(s.hp(tgt).current == doctest::Approx(100.0f));
    CHECK(s.wep(gun).last_point.z > -6.0f);
}

TEST_CASE("combat3d: armor, invulnerable, damage from other sources") {
    Scene s;
    const Entity tgt = s.target(0, -10, 100);
    s.run(10);
    s.hp(tgt).armor = 0.5f;
    DamageInfo d;
    d.amount = 40.0f;
    d.direction = {0, 0, -1};
    CHECK(apply_damage(s.w, tgt, d));
    CHECK(s.hp(tgt).current == doctest::Approx(80.0f));
    s.hp(tgt).invulnerable = 1;
    CHECK(apply_damage(s.w, tgt, d));
    CHECK(s.hp(tgt).current == doctest::Approx(80.0f));
    CHECK_FALSE(apply_damage(s.w, s.w.spawn(), d));
}

TEST_CASE("combat3d: rate of fire, magazine and reload") {
    Scene s;
    s.target();
    Weapon3D g;
    g.rate = 10.0f;
    g.magazine = 6;
    g.reserve = 8;
    g.reload_time = 0.5f;
    g.damage = 1.0f;
    g.use_input = 1;
    const Entity gun = s.gun(g);
    s.run(30);
    s.fire_held = true;
    s.run(40);  // a shot every 6-7 ticks: the 6 rounds in the magazine, then it reloads on its own
    CHECK(s.wep(gun).shots == 6);
    CHECK(s.wep(gun).reloading == 1);
    s.fire_held = false;
    s.run(40);
    CHECK(s.wep(gun).reloading == 0);
    CHECK(s.wep(gun).ammo == 6);
    CHECK(s.wep(gun).reserve == 2);
    // Semi-automatic: a held button shoots once.
    s.wep(gun).automatic = 0;
    s.fire_held = true;
    s.run(30);
    CHECK(s.wep(gun).shots == 11);  // the helper presses every tick; 10 shots/s over 30 ticks is 5 more
}

TEST_CASE("combat3d: a kill sends Died3D, takes control, and goes limp with a ragdoll") {
    Scene s;
    const Entity tgt = s.target(0, -10, 50);
    s.hp(tgt).ragdoll_on_death = 1;
    s.w.add<Ragdoll3D>(tgt, Ragdoll3D{});
    s.w.try_get_mut<CharacterController3D>(tgt)->use_input = true;
    Weapon3D g;
    g.damage = 30.0f;
    g.impulse = 100.0f;
    g.rate = 60.0f;
    const Entity gun = s.gun(g);
    s.run(30);
    s.wep(gun).fire = 1;
    s.run(1);
    CHECK(s.hp(tgt).dead == 0);
    s.run(2);
    s.wep(gun).fire = 1;
    s.run(1);
    CHECK(s.hp(tgt).dead == 1);
    CHECK(s.hp(tgt).current == doctest::Approx(0.0f));
    CHECK(s.deaths == 1);
    CHECK(s.last_killer == gun);
    CHECK_FALSE(s.w.try_get<CharacterController3D>(tgt)->use_input);
    CHECK(s.w.try_get<Ragdoll3D>(tgt)->active == 1);
    // The dead take no more hits.
    const u32 damages = s.damages;
    s.wep(gun).fire = 1;
    s.run(3);
    CHECK(s.damages == damages);
}

TEST_CASE("combat3d: a big hit staggers, then it gets up; regen; the dead linger and are destroyed") {
    Scene s;
    const Entity tgt = s.target(0, -10, 100);
    Health3D& h = s.hp(tgt);
    h.stagger_damage = 10.0f;
    h.stagger_time = 0.5f;
    h.regen = 10.0f;
    h.regen_delay = 1.0f;
    s.w.add<Ragdoll3D>(tgt, Ragdoll3D{});
    s.run(10);
    DamageInfo d;
    d.amount = 20.0f;
    apply_damage(s.w, tgt, d);
    CHECK(s.w.try_get<Ragdoll3D>(tgt)->active == 1);
    s.run(40);  // 0.67 s: staggering is over
    CHECK(s.w.try_get<Ragdoll3D>(tgt)->active == 0);
    CHECK(s.hp(tgt).current == doctest::Approx(80.0f));  // no regen yet
    s.run(120);
    CHECK(s.hp(tgt).current > 85.0f);
    s.run(600);
    CHECK(s.hp(tgt).current == doctest::Approx(100.0f));

    s.hp(tgt).destroy_on_death = 1;
    s.hp(tgt).linger = 1.0f;
    d.amount = 500.0f;
    apply_damage(s.w, tgt, d);
    s.run(30);
    CHECK(s.w.alive(tgt));
    s.run(60);
    CHECK_FALSE(s.w.alive(tgt));
}

TEST_CASE("combat3d: teams - no friendly fire unless asked") {
    Scene s;
    const Entity ally = s.target(0, -10, 100);
    s.hp(ally).team = 2;
    Health3D mine;
    mine.team = 2;
    Weapon3D g;
    g.damage = 10.0f;
    const Entity shooter = s.w.spawn_with(at(0, 1.0f, 0), g, mine);
    s.wep(shooter).owner = shooter;
    s.run(30);
    s.wep(shooter).fire = 1;
    s.run(1);
    CHECK(s.hits == 1);
    CHECK(s.hp(ally).current == doctest::Approx(100.0f));
    s.hp(ally).team = 3;
    s.run(15);
    s.wep(shooter).fire = 1;
    s.run(20);
    CHECK(s.hp(ally).current == doctest::Approx(90.0f));
    s.hp(ally).team = 2;
    s.run(15);
    s.wep(shooter).friendly_fire = 1;
    s.wep(shooter).fire = 1;
    s.run(20);
    CHECK(s.hp(ally).current == doctest::Approx(80.0f));
}

TEST_CASE("combat3d: a projectile flies under gravity and explodes with falloff") {
    Scene s;
    const Entity direct = s.target(0, -12, 100);
    const Entity near = s.target(2.0f, -12, 100);
    const Entity far = s.target(8.0f, -12, 100);
    Weapon3D g;
    g.kind = WeaponKind3D::Projectile;
    g.projectile_speed = 30.0f;
    g.projectile_gravity = 0.0f;
    g.damage = 50.0f;
    g.splash_radius = 4.0f;
    const Entity gun = s.gun(g);
    s.run(30);
    s.wep(gun).fire = 1;
    s.run(1);
    CHECK(s.fired == 1);
    CHECK(s.w.query<Projectile3D>().count() == 1);
    s.run(40);  // 12 m at 30 m/s: 0.4 s
    CHECK(s.w.query<Projectile3D>().count() == 0);
    CHECK(s.hp(direct).current == doctest::Approx(50.0f));
    CHECK(s.hp(near).current < 100.0f);
    CHECK(s.hp(near).current > 50.0f);
    CHECK(s.hp(far).current == doctest::Approx(100.0f));

    // Gravity bends the flight: a slow shot at long range drops under the target.
    Scene t2;
    const Entity tgt = t2.target(0, -30, 100);
    Weapon3D slow;
    slow.kind = WeaponKind3D::Projectile;
    slow.projectile_speed = 15.0f;
    slow.projectile_gravity = 1.0f;
    const Entity gun2 = t2.gun(slow);
    t2.run(30);
    t2.wep(gun2).fire = 1;
    t2.run(240);
    CHECK(t2.hp(tgt).current == doctest::Approx(100.0f));
    CHECK(t2.w.query<Projectile3D>().count() == 0);
}

TEST_CASE("combat3d: the Fire action drives a weapon with use_input") {
    Scene s;
    const Entity tgt = s.target();
    Weapon3D g;
    g.use_input = 1;
    g.rate = 4.0f;
    g.damage = 5.0f;
    const Entity gun = s.gun(g);
    s.run(30);
    s.run(30);
    CHECK(s.wep(gun).shots == 0);
    s.fire_held = true;
    s.run(30);  // half a second at 4/s: shots on ticks 0 and 15
    CHECK(s.wep(gun).shots == 2);
    CHECK(s.hp(tgt).current == doctest::Approx(90.0f));
}

TEST_CASE("combat3d: the replay hash holds with spread, pellets and projectiles") {
    auto run = [] {
        Scene s;
        s.target(0, -10);
        s.target(1, -12);
        Weapon3D g;
        g.pellets = 8;
        g.spread_deg = 6.0f;
        g.damage = 4.0f;
        g.rate = 20.0f;
        const Entity gun = s.gun(g);
        Weapon3D rl;
        rl.kind = WeaponKind3D::Projectile;
        rl.splash_radius = 3.0f;
        rl.projectile_gravity = 0.3f;
        rl.rate = 2.0f;
        const Entity launcher = s.gun(rl);
        s.run(30);
        for (u32 i = 0; i < 20; ++i) {
            s.wep(gun).fire = 1;
            s.wep(launcher).fire = 1;
            s.run(10);
        }
        return s.w.state_hash();
    };
    CHECK(run() == run());
}

TEST_CASE("combat3d: an explosion hurts a target with several colliders once") {
    Scene s;
    const Entity body = s.target(0, -12, 100);
    BoxCollider3D extra;
    extra.half = {0.3f, 0.3f, 0.3f};
    const Entity child = s.w.spawn_with(at(1.0f, 1.0f, -12.0f), extra);
    s.w.add<ChildOf>(child, ChildOf{body});
    Weapon3D g;
    g.kind = WeaponKind3D::Projectile;
    g.projectile_gravity = 0.0f;
    g.damage = 40.0f;
    g.splash_radius = 4.0f;
    const Entity gun = s.gun(g);
    s.run(30);
    s.wep(gun).fire = 1;
    s.run(60);
    CHECK(s.hp(body).hits == 1);
}
