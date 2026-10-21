// Wanted level and police (M-OPENWORLD C3): crimes and witnesses, levels and decay, police states
// (Idle, Pursue, Attack, Search), shooting and facing, the NavAgent3D hand-off, and the replay hash.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/nav3d.h"
#include "mce/kit/wanted3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

struct Scene {
    World w;
    u64 t = 0;
    u32 crimes = 0, changes = 0;
    u32 last_level = 0;
    Scene() {
        w.register_components<LocalTransform>();
        install_wanted3d(w);
        BoxCollider3D ground;
        ground.half = {300.0f, 0.5f, 300.0f};
        w.spawn_with(at(0, -0.5f, 0), ground);
        run(2);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) {
            w.tick(kDt, t++);
            auto ev = EventAccess(w);
            crimes += static_cast<u32>(ev.read<Crime3D>().size());
            for (const WantedChanged3D& c : ev.read<WantedChanged3D>()) {
                ++changes;
                last_level = c.to;
            }
        }
    }
    Entity player(Wanted3D wd = {}, f32 x = 0.0f, f32 z = 0.0f) {
        Health3D h;
        h.team = 1;
        return w.spawn_with(at(x, 1.0f, z), wd, h, CharacterController3D{});
    }
    Entity civilian(f32 x, f32 z) {
        Health3D h;
        h.team = 3;
        return w.spawn_with(at(x, 1.0f, z), h);
    }
    Entity cop(f32 x, f32 z, Police3D p = {}) {
        Health3D h;
        h.team = 2;
        return w.spawn_with(at(x, 1.0f, z), p, h);
    }
    void hit(Entity who, Entity victim, f32 amount = 1.0f) {
        DamageInfo d;
        d.amount = amount;
        d.instigator = who;
        CHECK(apply_damage(w, victim, d));
    }
    Wanted3D& wanted(Entity e) { return *w.try_get_mut<Wanted3D>(e); }
    Police3D& police(Entity e) { return *w.try_get_mut<Police3D>(e); }
};

}  // namespace

TEST_CASE("wanted3d: a civilian hit counts only when police see it") {
    Scene s;
    const Entity p = s.player();
    const Entity c = s.civilian(5, 0);
    s.hit(p, c);
    s.run(2);  // events are read a tick after they are pushed
    CHECK(s.wanted(p).heat == doctest::Approx(0.0f));
    CHECK(s.crimes == 0);

    s.cop(5, 20);  // 20 m from the victim, clear view
    s.hit(p, c);
    s.run(2);  // events are read a tick after they are pushed
    CHECK(s.wanted(p).heat == doctest::Approx(5.0f));
    CHECK(s.wanted(p).level == 1);
    CHECK(s.crimes == 1);
    CHECK(s.changes == 1);
    CHECK(s.last_level == 1);
    CHECK(s.wanted(p).crimes == 1);
}

TEST_CASE("wanted3d: a wall hides the crime, a far cop does not see it") {
    Scene s;
    const Entity p = s.player();
    const Entity c = s.civilian(5, 0);
    BoxCollider3D wall;
    wall.half = {10.0f, 5.0f, 0.3f};
    s.w.spawn_with(at(5, 2, 10), wall);
    s.cop(5, 20);
    s.run(3);
    s.hit(p, c);
    s.run(2);  // events are read a tick after they are pushed
    CHECK(s.wanted(p).heat == doctest::Approx(0.0f));
    s.cop(5, -100);  // clear view but 100 m away
    s.hit(p, c);
    s.run(2);  // events are read a tick after they are pushed
    CHECK(s.wanted(p).heat == doctest::Approx(0.0f));
    s.wanted(p).require_witness = 0;
    s.hit(p, c);
    s.run(2);  // events are read a tick after they are pushed
    CHECK(s.wanted(p).heat == doctest::Approx(5.0f));
}

TEST_CASE("wanted3d: hurting police is always known, a kill raises the level and the heat is capped") {
    Scene s;
    const Entity p = s.player();
    const Entity c1 = s.cop(0, -50);
    s.hit(p, c1, 10.0f);
    s.run(2);  // events are read a tick after they are pushed
    CHECK(s.wanted(p).heat == doctest::Approx(15.0f));
    CHECK(s.wanted(p).level == 1);
    s.hit(p, c1, 1000.0f);  // dies: another hit and the kill
    s.run(2);
    CHECK(s.wanted(p).heat == doctest::Approx(60.0f));
    CHECK(s.wanted(p).level == 3);
    for (u32 i = 0; i < 6; ++i) {
        const Entity c = s.cop(0, -50);
        s.hit(p, c, 1000.0f);
    }
    s.run(2);
    CHECK(s.wanted(p).level == 5);
    CHECK(s.wanted(p).heat == doctest::Approx(100.0f));
}

TEST_CASE("wanted3d: a unit sees, pursues, attacks and faces the target") {
    Scene s;
    Wanted3D wd;
    wd.heat = 40.0f;  // level 2
    const Entity p = s.player(wd);
    Police3D pol;
    Weapon3D gun;
    gun.damage = 1.0f;
    const Entity c = s.cop(0, -25, pol);
    s.w.add(c, gun);
    s.run(2);
    CHECK(s.police(c).state == PoliceState::Pursue);
    CHECK(s.police(c).target == p);
    CHECK(s.wanted(p).seen_by == 1);
    CHECK(s.wanted(p).last_known.z == doctest::Approx(0.0f));
    s.w.try_get_mut<LocalTransform>(c)->position = {0, 1, -15};
    s.run(2);
    CHECK(s.police(c).state == PoliceState::Attack);
    // It turned to face +Z (the player is behind it): the shot leaves along its -Z.
    CHECK(s.w.try_get<Weapon3D>(c)->shots >= 1);
    CHECK(s.w.try_get<Health3D>(p)->hits >= 1);
}

TEST_CASE("wanted3d: below attack_level a unit only follows; min_level keeps it out") {
    Scene s;
    Wanted3D wd;
    wd.heat = 10.0f;  // level 1
    const Entity p = s.player(wd);
    const Entity near = s.cop(0, -5);
    Police3D lazy;
    lazy.min_level = 2;
    const Entity idle = s.cop(5, 0, lazy);
    s.run(2);
    CHECK(s.police(near).state == PoliceState::Pursue);
    CHECK(s.police(idle).state == PoliceState::Idle);
    CHECK(s.wanted(p).seen_by == 1);
    s.wanted(p).heat = 30.0f;
    s.run(2);
    CHECK(s.police(near).state == PoliceState::Attack);
    CHECK(s.police(idle).state == PoliceState::Attack);
}

TEST_CASE("wanted3d: losing the target is Search, then Idle; heat decays once unseen") {
    Scene s;
    Wanted3D wd;
    wd.heat = 20.0f;
    wd.cooldown_delay = 2.0f;
    wd.decay = 10.0f;
    const Entity p = s.player(wd);
    Police3D pol;
    pol.search_time = 3.0f;
    const Entity c = s.cop(0, -10, pol);
    s.run(2);
    CHECK(s.police(c).state != PoliceState::Idle);
    BoxCollider3D wall;
    wall.half = {10.0f, 5.0f, 0.3f};
    s.w.spawn_with(at(0, 2, -5), wall);
    s.run(5);
    CHECK(s.police(c).state == PoliceState::Search);
    CHECK(s.police(c).target == p);
    CHECK(s.wanted(p).level == 1);  // the 2 s wait has not run out
    s.run(60 * 3 + 5);              // search_time
    CHECK(s.police(c).state == PoliceState::Idle);
    CHECK(s.police(c).target.is_null());
    s.run(60 * 2);  // 2 s of decay at 10/s, enough for 20 heat
    CHECK(s.wanted(p).heat == doctest::Approx(0.0f));
    CHECK(s.wanted(p).level == 0);
    CHECK(s.last_level == 0);
}

TEST_CASE("wanted3d: a crime restarts the wait; seen, heat does not decay") {
    Scene s;
    Wanted3D wd;
    wd.heat = 20.0f;
    wd.cooldown_delay = 1.0f;
    wd.decay = 10.0f;
    const Entity p = s.player(wd);
    s.run(30);
    CHECK(s.wanted(p).heat == doctest::Approx(20.0f));
    s.run(60);
    CHECK(s.wanted(p).heat < 20.0f);
    report_crime(s.w, p, 5.0f, {});
    CHECK(s.wanted(p).unseen_time == doctest::Approx(0.0f));
    const f32 before = s.wanted(p).heat;
    s.run(30);
    CHECK(s.wanted(p).heat == doctest::Approx(before));
    // A watching cop freezes the decay.
    s.cop(0, -5);
    s.run(120);
    CHECK(s.wanted(p).seen_by == 1);
    CHECK(s.wanted(p).heat == doctest::Approx(before));
}

TEST_CASE("wanted3d: report_crime, clear_wanted and a dead unit") {
    Scene s;
    const Entity p = s.player();
    CHECK_FALSE(report_crime(s.w, p, 5.0f, {}, {}, false));
    CHECK(report_crime(s.w, p, 25.0f, {1, 0, 2}));
    CHECK(s.wanted(p).level == 2);
    CHECK(s.crimes == 0);  // the event is read after a tick
    s.run(1);
    CHECK(s.crimes == 1);
    const Entity c = s.cop(0, -5);
    s.run(2);
    CHECK(s.police(c).state != PoliceState::Idle);
    s.hit(p, c, 1000.0f);
    s.run(2);
    CHECK(s.police(c).state == PoliceState::Idle);
    clear_wanted(s.w, p);
    CHECK(s.wanted(p).level == 0);
    CHECK_FALSE(report_crime(s.w, Entity{}, 5.0f, {}));
}

TEST_CASE("wanted3d: drive_nav sets the NavAgent3D") {
    Scene s;
    Wanted3D wd;
    wd.heat = 10.0f;
    const Entity p = s.player(wd);
    const Entity c = s.cop(0, -10);
    s.w.add(c, NavAgent3D{});
    s.run(2);
    const NavAgent3D& a = *s.w.try_get<NavAgent3D>(c);
    CHECK(a.follow == p);
    CHECK(a.move_to == 1);
    BoxCollider3D wall;
    wall.half = {10.0f, 5.0f, 0.3f};
    s.w.spawn_with(at(0, 2, -5), wall);
    s.run(3);
    CHECK(s.police(c).state == PoliceState::Search);
    CHECK(a.follow.is_null());
    CHECK(a.target.z == doctest::Approx(0.0f));
    CHECK(a.move_to == 1);
    s.police(c).drive_nav = 0;
    s.police(c).search_time = 0.0f;
    s.run(2);
    CHECK(s.police(c).state == PoliceState::Idle);
    CHECK(a.move_to == 1);  // untouched with drive_nav off
}

TEST_CASE("wanted3d: replays hash the same") {
    auto run = [] {
        Scene s;
        Wanted3D wd;
        wd.heat = 40.0f;
        s.player(wd);
        Police3D pol;
        Weapon3D gun;
        const Entity c = s.cop(0, -15, pol);
        s.w.add(c, gun);
        s.cop(8, 3);
        s.run(240);
        return s.w.state_hash();
    };
    CHECK(run() == run());
}
