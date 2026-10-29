// Wanted level and police (M-OPENWORLD C3): crimes and witnesses, levels and decay, police states
// (Idle, Pursue, Attack, Search), shooting and facing, the NavAgent3D hand-off, and the replay hash.

#include <doctest/doctest.h>

#include <cmath>

#include <cstring>

#include "mce/asset/container.h"
#include "mce/cook/cook.h"
#include "mce/cook/importer.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/captions.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/missions.h"
#include "mce/kit/nav3d.h"
#include "mce/kit/roads.h"
#include "mce/kit/seats3d.h"
#include "mce/kit/vehicles3d.h"
#include "mce/kit/wanted3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

struct Scene {
    World w;
    u64 t = 0;
    u32 crimes = 0, changes = 0, arrests = 0, respawns = 0, last_reason = 0;
    u32 last_level = 0;
    u32 sprays = 0;
    u32 ammo_sales = 0;
    u32 heals = 0;
    u32 pickups = 0;
    f32 last_heal = 0.0f;
    u32 last_rounds = 0;
    i32 last_paid = 0;
    u32 cash_events = 0;
    i32 last_delta = 0;
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
            arrests += static_cast<u32>(ev.read<Arrested3D>().size());
            for (const Respawned3D& r : ev.read<Respawned3D>()) {
                ++respawns;
                last_reason = r.reason;
            }
            for (const Sprayed3D& sp : ev.read<Sprayed3D>()) {
                ++sprays;
                last_paid = sp.paid;
            }
            pickups += static_cast<u32>(ev.read<CashPickedUp3D>().size());
            for (const HealBought3D& h : ev.read<HealBought3D>()) {
                ++heals;
                last_heal = h.hp;
            }
            for (const AmmoBought3D& a : ev.read<AmmoBought3D>()) {
                ++ammo_sales;
                last_rounds = a.rounds;
            }
            for (const CashChanged3D& c : ev.read<CashChanged3D>()) {
                ++cash_events;
                last_delta = c.delta;
            }
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

TEST_CASE("wanted3d: a unit arrests an offender who stands still within reach at a low level (M-ARREST)") {
    Wanted3D wd;
    wd.heat = 5.0f;  // level 1
    {
        Scene s;
        const Entity p = s.player(wd, 0.0f, 0.0f);
        const Entity c = s.cop(2.0f, 0.0f);
        s.run(60);
        CHECK(s.wanted(p).level == 1);
        CHECK(s.arrests == 0);
        CHECK(s.police(c).arrest_progress > 0.0f);
        s.run(120);
        CHECK(s.arrests == 1);
        CHECK(s.wanted(p).level == 0);
        CHECK(s.wanted(p).heat == 0.0f);
        CHECK(s.police(c).state == PoliceState::Idle);
    }
    {  // keeps moving: no arrest, and the progress falls back
        Scene s;
        const Entity p = s.player(wd, 0.0f, 0.0f);
        const Entity c = s.cop(2.0f, 0.0f);
        s.run(60);
        for (u32 i = 0; i < 240; ++i) {
            s.w.try_get_mut<LocalTransform>(p)->position.z += 3.0f * kDt * ((i / 20) % 2 ? -1.0f : 1.0f);
            s.run(1);
        }
        CHECK(s.arrests == 0);
        CHECK(s.wanted(p).level == 1);
        (void)c;
    }
    {  // out of reach, above the arrest level, or a unit that does not arrest
        for (u32 k = 0; k < 3; ++k) {
            Scene s;
            Wanted3D w2 = wd;
            Police3D pol;
            f32 dist = 2.0f;
            if (k == 0) dist = 5.0f;
            if (k == 1) w2.heat = 30.0f;  // level 2
            if (k == 2) pol.arrest_range = 0.0f;
            const Entity p = s.player(w2, 0.0f, 0.0f);
            s.cop(dist, 0.0f, pol);
            s.run(60 * 4);
            CHECK(s.arrests == 0);
            CHECK(s.wanted(p).level >= 1);
        }
    }
}

TEST_CASE("wanted3d: an idle unit within dispatch range is sent to the last known place (M-ARREST 2)") {
    Wanted3D wd;
    wd.heat = 25.0f;  // level 2
    wd.crimes = 1;
    wd.last_known = {0.0f, 1.0f, 0.0f};
    for (u32 k = 0; k < 4; ++k) {
        Scene s;
        const Entity p = s.player(wd, 0.0f, 0.0f);
        Police3D pol;
        f32 dist = 80.0f;  // far beyond sight_range
        if (k >= 1) pol.dispatch_range = 100.0f;
        if (k == 2) dist = 150.0f;  // beyond the range
        if (k == 3) pol.min_level = 3;  // does not answer to level 2
        const Entity c = s.cop(dist, 0.0f, pol);
        s.run(10);
        const bool sent = s.police(c).state == PoliceState::Search;
        CHECK(sent == (k == 1));
        CHECK(s.police(c).target.is_null() == (k != 1));
        (void)p;
    }
}

TEST_CASE("wanted3d: a police car steers and drives at the target (M-ARREST 3)") {
    Wanted3D wd;
    wd.heat = 25.0f;
    wd.crimes = 1;
    for (u32 k = 0; k < 4; ++k) {
        Scene s;
        const f32 side = k == 1 ? -20.0f : 20.0f;  // right (+X) or left of a car facing -Z
        s.player(wd, side, -30.0f);
        Police3D pol;
        pol.drive_car = k == 3 ? 0 : 1;
        pol.sight_range = 100.0f;
        Health3D h;
        h.team = 2;
        const Entity c = s.w.spawn_with(at(0, 1, 0), pol, h, Vehicle3D{});
        s.run(5);
        const Vehicle3D& v = *s.w.try_get<Vehicle3D>(c);
        if (k == 3) {
            CHECK(v.throttle == 0.0f);
            CHECK(v.steer == 0.0f);
        } else if (k == 2) {  // a unit at the offender: stops
            s.w.try_get_mut<LocalTransform>(c)->position = {side, 1.0f, -29.0f};
            s.run(5);
            CHECK(v.throttle == 0.0f);
            CHECK(v.brake == 1.0f);
        } else {
            CHECK(v.throttle > 0.0f);
            CHECK(v.brake == 0.0f);
            if (k == 0) CHECK(v.steer > 0.0f);
            if (k == 1) CHECK(v.steer < 0.0f);
        }
    }
}

TEST_CASE("wanted3d: a police car with drive_car 2 follows the road, not the straight line (M-ARREST 4)") {
    // An L of road: from the origin 100 m north (-Z), then 100 m east. The target stands 90 m east and 80 m north of the car.
    constexpr const char* kL = R"({ "format": "mce-roads", "version": 1, "name": "ell",
      "nodes": [{"name": "a", "at": [0, 0, 0]}, {"name": "b", "at": [0, 0, -100]}, {"name": "c", "at": [100, 0, -100]}],
      "roads": [{"name": "up", "from": "a", "to": "b", "lanes": 1}, {"name": "over", "from": "b", "to": "c", "lanes": 1}] })";
    const asset::Guid guid{0x7e7a000000004000ull, 0x80000000000000d1ull};
    cook::ImporterRegistry importers;
    cook::ImporterRegistry::add_builtins(importers);
    const cook::IImporter* imp = importers.for_extension(".mcroads");
    REQUIRE(imp);
    cook::AssetRegistry registry;
    cook::DiagList diags;
    Array<byte> cooked;
    REQUIRE(cook::cook_document(registry, *imp, guid, "ell.mcroads", kL, cooked, diags));
    auto view = asset::ContainerView::open(cooked.span(), "ell.mcroads");
    REQUIRE(view);
    const Span<const byte> payload = view.value().payload(view.value().find(asset::ChunkType::RoadNetwork, guid));

    Wanted3D wd;
    wd.heat = 25.0f;
    wd.crimes = 1;
    f32 steer[3] = {};
    for (u32 k = 0; k < 3; ++k) {  // 0: straight line, 1: along the road, 2: along the road with no lane near the car
        Scene s;
        install_roads(s.w, nullptr);
        REQUIRE(s.w.resource<RoadRegistry>().define(guid, payload));
        RoadNetwork3D n;
        n.roads.guid = guid;
        s.w.spawn_with(at(0, 0, 0), n);
        s.player(wd, 90.0f, -80.0f);
        Police3D pol;
        pol.drive_car = k == 0 ? 1 : 2;
        pol.sight_range = 300.0f;
        Health3D h;
        h.team = 2;
        const Entity c = s.w.spawn_with(at(k == 2 ? -200.0f : 0.0f, 1, -10), pol, h, Vehicle3D{});
        s.run(5);
        const Vehicle3D& v = *s.w.try_get<Vehicle3D>(c);
        CHECK(v.throttle > 0.0f);
        CHECK(v.brake == 0.0f);
        steer[k] = v.steer;
    }
    CHECK(steer[0] > 0.5f);                 // straight at the target: hard right
    CHECK(steer[1] < steer[0] - 0.3f);      // the road runs on ahead: only the lane's offset to steer for
    CHECK(steer[2] > 0.5f);                 // 200 m from any lane: falls back to the straight line
}

TEST_CASE("wanted3d: a dead or arrested player wakes up at a hospital or station (M-WASTED)") {
    Wanted3D wd;
    wd.heat = 5.0f;  // level 1
    wd.crimes = 1;
    const auto point = [](Scene& s, f32 x, RespawnKind k) {
        RespawnPoint3D p;
        p.kind = k;
        s.w.spawn_with(at(x, 1.0f, 0.0f), p);
    };
    const auto pos_x = [](Scene& s, Entity e) { return s.w.try_get<LocalTransform>(e)->position.x; };
    {  // wasted: after `delay`, at the hospital, alive, no wanted level
        Scene s;
        const Entity p = s.player(wd, 0.0f, 0.0f);
        Respawn3D r;
        r.delay = 1.0f;
        s.w.add(p, r);
        point(s, 50.0f, RespawnKind::Hospital);
        point(s, -50.0f, RespawnKind::Station);
        DamageInfo d;
        d.amount = 1000.0f;
        CHECK(apply_damage(s.w, p, d));
        s.run(30);
        CHECK(s.w.try_get<Health3D>(p)->dead == 1);
        CHECK(s.respawns == 0);
        s.run(60);
        CHECK(s.respawns == 1);
        CHECK(s.last_reason == 2);
        CHECK(pos_x(s, p) == doctest::Approx(50.0f));
        CHECK(s.w.try_get<Health3D>(p)->dead == 0);
        CHECK(s.w.try_get<Health3D>(p)->current == doctest::Approx(s.w.try_get<Health3D>(p)->max));
        CHECK(s.wanted(p).level == 0);
        s.run(120);
        CHECK(s.respawns == 1);  // once
    }
    {  // busted: an arrest goes to the station
        Scene s;
        const Entity p = s.player(wd, 0.0f, 0.0f);
        Respawn3D r;
        r.delay = 1.0f;
        s.w.add(p, r);
        point(s, 50.0f, RespawnKind::Hospital);
        point(s, -50.0f, RespawnKind::Station);
        s.cop(2.0f, 0.0f);
        s.run(60 * 6);
        CHECK(s.arrests == 1);
        CHECK(s.respawns == 1);
        CHECK(s.last_reason == 1);
        CHECK(pos_x(s, p) == doctest::Approx(-50.0f));
    }
    {  // only a station: a dead player goes there; no point at all: it stays and still wakes up
        for (u32 k = 0; k < 2; ++k) {
            Scene s;
            const Entity p = s.player(wd, 3.0f, 0.0f);
            Respawn3D r;
            r.delay = 0.5f;
            s.w.add(p, r);
            if (k == 0) point(s, -50.0f, RespawnKind::Station);
            DamageInfo d;
            d.amount = 1000.0f;
            CHECK(apply_damage(s.w, p, d));
            s.run(90);
            CHECK(s.respawns == 1);
            CHECK(pos_x(s, p) == doctest::Approx(k == 0 ? -50.0f : 3.0f));
            CHECK(s.w.try_get<Health3D>(p)->dead == 0);
        }
    }
}

TEST_CASE("wanted3d: the respawn wait shows BUSTED or WASTED as a caption (M-WASTED 2)") {
    Wanted3D wd;
    wd.heat = 5.0f;
    wd.crimes = 1;
    for (u32 k = 0; k < 3; ++k) {
        Scene s;
        if (k != 2) install_captions(s.w);  // k 2: no captions installed, nothing breaks
        const Entity p = s.player(wd, 0.0f, 0.0f);
        Respawn3D r;
        r.delay = 1.0f;
        if (k == 1) r.caption = 0;
        s.w.add(p, r);
        DamageInfo d;
        d.amount = 1000.0f;
        CHECK(apply_damage(s.w, p, d));
        s.run(20);
        bool shown = false;
        if (const Captions* c = s.w.try_resource<Captions>())
            for (const CaptionLine& l : c->lines)
                if (l.text == "WASTED") shown = true;
        CHECK(shown == (k == 0));
        s.run(80);
        CHECK(s.respawns == 1);
    }
}

TEST_CASE("wanted3d: a killed CashDrop3D pays the killer's wallet; take_cash never goes below zero (M-CASH)") {
    const auto kill_victim = [](Scene& s, Entity killer, bool with_drop) {
        const Entity v = s.civilian(5.0f, 0.0f);
        if (with_drop) {
            CashDrop3D d;
            d.min = 10;
            d.max = 20;
            s.w.add(v, d);
        }
        s.hit(killer, v, 1000.0f);
        s.run(4);
    };
    {  // pays inside [min, max]
        Scene s;
        const Entity p = s.player();
        s.w.add(p, Wallet3D{});
        kill_victim(s, p, true);
        const i32 cash = s.w.try_get<Wallet3D>(p)->cash;
        CHECK(cash >= 10);
        CHECK(cash <= 20);
        CHECK(s.cash_events == 1);
        CHECK(s.last_delta == cash);
    }
    {  // no wallet, or no drop: nothing
        Scene s;
        const Entity p = s.player();
        kill_victim(s, p, true);
        CHECK(s.cash_events == 0);
        s.w.add(p, Wallet3D{});
        kill_victim(s, p, false);
        CHECK(s.cash_events == 0);
        CHECK(s.w.try_get<Wallet3D>(p)->cash == 0);
    }
    {  // take_cash stops at zero and reports what it took
        Scene s;
        const Entity p = s.player();
        CHECK(take_cash(s.w, p, 5) == 0);
        CHECK_FALSE(give_cash(s.w, p, 5));
        s.w.add(p, Wallet3D{50});
        CHECK(take_cash(s.w, p, 30) == 30);
        CHECK(take_cash(s.w, p, 30) == 20);
        CHECK(take_cash(s.w, p, 30) == 0);
        CHECK(s.w.try_get<Wallet3D>(p)->cash == 0);
        CHECK(give_cash(s.w, p, 7));
        CHECK_FALSE(give_cash(s.w, p, -7));
        CHECK(s.w.try_get<Wallet3D>(p)->cash == 7);
    }
    {  // two identical runs pay the same
        i32 got[2] = {0, 0};
        for (i32& g : got) {
            Scene s;
            const Entity p = s.player();
            s.w.add(p, Wallet3D{});
            kill_victim(s, p, true);
            g = s.w.try_get<Wallet3D>(p)->cash;
        }
        CHECK(got[0] == got[1]);
    }
}

TEST_CASE("wanted3d: waking up costs the fine, never below zero, and shows it (M-CASH 2)") {
    Wanted3D wd;
    wd.heat = 5.0f;
    wd.crimes = 1;
    const i32 start[3] = {500, 50, 0};
    const i32 left[3] = {300, 0, 0};  // wasted fine is 200
    for (u32 k = 0; k < 3; ++k) {
        Scene s;
        install_captions(s.w);
        const Entity p = s.player(wd, 0.0f, 0.0f);
        Respawn3D r;
        r.delay = 1.0f;
        s.w.add(p, r);
        s.w.add(p, Wallet3D{start[k]});
        DamageInfo d;
        d.amount = 1000.0f;
        CHECK(apply_damage(s.w, p, d));
        s.run(120);
        CHECK(s.respawns == 1);
        CHECK(s.w.try_get<Wallet3D>(p)->cash == left[k]);
        CHECK(s.cash_events == (k == 2 ? 0u : 1u));
        bool shown = false;
        for (const CaptionLine& l : s.w.try_resource<Captions>()->lines)
            if (l.text.view().substr(0, 2) == "-$") shown = true;
        CHECK(shown == (k != 2));
    }
    {  // no wallet: nothing taken, nothing shown
        Scene s;
        const Entity p = s.player(wd, 0.0f, 0.0f);
        s.w.add(p, Respawn3D{});
        DamageInfo d;
        d.amount = 1000.0f;
        CHECK(apply_damage(s.w, p, d));
        s.run(300);
        CHECK(s.respawns == 1);
        CHECK(s.cash_events == 0);
    }
}

TEST_CASE("wanted3d: a garage clears the wanted level for a price (M-PAYSPRAY)") {
    Wanted3D wd;
    wd.heat = 45.0f;  // level 3 at 20 heat per level
    wd.crimes = 1;
    const auto garage = [](Scene& s, f32 x, u16 price = 100) {
        PaySpray3D g;
        g.radius = 3.0f;
        g.price_per_level = price;
        return s.w.spawn_with(at(x, 1.0f, 0.0f), g);
    };
    {  // at the garage with enough cash: pays level x price, level 0
        Scene s;
        const Entity p = s.player(wd, 1.0f, 0.0f);
        s.w.add(p, Wallet3D{1000});
        s.run(2);
        CHECK(s.wanted(p).level == 3);
        garage(s, 0.0f);
        s.run(4);
        CHECK(s.sprays == 1);
        CHECK(s.last_paid == 300);
        CHECK(s.w.try_get<Wallet3D>(p)->cash == 700);
        CHECK(s.wanted(p).level == 0);
        s.run(60);
        CHECK(s.sprays == 1);  // level 0: nothing more to pay for
    }
    {  // too poor, too far, switched off, no wallet: nothing happens
        for (u32 k = 0; k < 4; ++k) {
            Scene s;
            const Entity p = s.player(wd, k == 1 ? 50.0f : 1.0f, 0.0f);
            if (k != 3) s.w.add(p, Wallet3D{k == 0 ? 299 : 1000});
            const Entity g = garage(s, 0.0f);
            if (k == 2) s.w.try_get_mut<PaySpray3D>(g)->enabled = 0;
            s.run(30);
            CHECK(s.sprays == 0);
            CHECK(s.wanted(p).level == 3);
        }
    }
}

TEST_CASE("wanted3d: an arrest costs spare ammunition, dying does not (M-ARRESTLOSS)") {
    Wanted3D wd;
    wd.heat = 5.0f;
    wd.crimes = 1;
    for (u32 k = 0; k < 3; ++k) {  // k 0: busted with Confiscate3D; k 1: wasted with it; k 2: busted without it
        Scene s;
        const Entity p = s.player(wd, 0.0f, 0.0f);
        Respawn3D r;
        r.delay = 1.0f;
        s.w.add(p, r);
        Weapon3D wp;
        wp.magazine = 10;
        wp.reserve = 100;
        s.w.add(p, wp);
        if (k != 2) s.w.add(p, Confiscate3D{0.25f});
        if (k == 1) {
            DamageInfo d;
            d.amount = 1000.0f;
            CHECK(apply_damage(s.w, p, d));
        } else {
            s.cop(2.0f, 0.0f);
        }
        s.run(60 * 6);
        CHECK(s.respawns == 1);
        CHECK(s.last_reason == (k == 1 ? 2u : 1u));
        CHECK(s.w.try_get<Weapon3D>(p)->reserve == (k == 0 ? 25u : 100u));
    }
}

TEST_CASE("wanted3d: a shop tops up the reserve for a price (M-AMMOSHOP)") {
    const auto buyer = [](Scene& s, f32 x, i32 cash) {
        const Entity p = s.player({}, x, 0.0f);
        Weapon3D wp;
        wp.magazine = 12;
        wp.reserve = 20;
        s.w.add(p, wp);
        s.w.add(p, Wallet3D{cash});
        return p;
    };
    const auto shop = [](Scene& s) {
        AmmoShop3D a;
        a.price_per_round = 2;
        a.stock_to = 100;
        return s.w.spawn_with(at(0.0f, 1.0f, 0.0f), a);
    };
    {  // enough cash: filled to stock_to, paid 80 rounds x 2
        Scene s;
        const Entity p = buyer(s, 1.0f, 1000);
        shop(s);
        s.run(4);
        CHECK(s.ammo_sales == 1);
        CHECK(s.last_rounds == 80);
        CHECK(s.w.try_get<Weapon3D>(p)->reserve == 100);
        CHECK(s.w.try_get<Wallet3D>(p)->cash == 840);
        s.run(60);
        CHECK(s.ammo_sales == 1);  // full: nothing more to sell
    }
    {  // short of cash: buys what it can pay for
        Scene s;
        const Entity p = buyer(s, 1.0f, 25);
        shop(s);
        s.run(4);
        CHECK(s.w.try_get<Weapon3D>(p)->reserve == 32);
        CHECK(s.w.try_get<Wallet3D>(p)->cash == 1);
    }
    {  // too far, switched off, never-dry weapon: nothing sold
        for (u32 k = 0; k < 3; ++k) {
            Scene s;
            const Entity p = buyer(s, k == 0 ? 50.0f : 1.0f, 1000);
            const Entity sh = shop(s);
            if (k == 1) s.w.try_get_mut<AmmoShop3D>(sh)->enabled = 0;
            if (k == 2) s.w.try_get_mut<Weapon3D>(p)->infinite_ammo = 1;
            s.run(30);
            CHECK(s.ammo_sales == 0);
            CHECK(s.w.try_get<Weapon3D>(p)->reserve == 20);
        }
    }
}

TEST_CASE("wanted3d: a medic heals a hurt buyer for a price (M-HEALSHOP)") {
    const auto buyer = [](Scene& s, f32 x, i32 cash) {
        const Entity p = s.player({}, x, 0.0f);
        s.w.try_get_mut<Health3D>(p)->current = 40.0f;
        s.w.add(p, Wallet3D{cash});
        return p;
    };
    const auto medic = [](Scene& s) {
        HealShop3D h;
        h.price_per_hp = 2;
        return s.w.spawn_with(at(0.0f, 1.0f, 0.0f), h);
    };
    {  // enough cash: back to max, 60 hp x 2
        Scene s;
        const Entity p = buyer(s, 1.0f, 500);
        medic(s);
        s.run(4);
        CHECK(s.heals == 1);
        CHECK(s.last_heal == 60.0f);
        CHECK(s.w.try_get<Health3D>(p)->current == 100.0f);
        CHECK(s.w.try_get<Wallet3D>(p)->cash == 380);
        s.run(60);
        CHECK(s.heals == 1);  // healthy: nothing to sell
    }
    {  // short of cash: buys the hit points it can pay for
        Scene s;
        const Entity p = buyer(s, 1.0f, 25);
        medic(s);
        s.run(4);
        CHECK(s.w.try_get<Health3D>(p)->current == 52.0f);
        CHECK(s.w.try_get<Wallet3D>(p)->cash == 1);
    }
    {  // too far, switched off: nothing sold
        for (u32 k = 0; k < 2; ++k) {
            Scene s;
            const Entity p = buyer(s, k == 0 ? 50.0f : 1.0f, 500);
            const Entity m = medic(s);
            if (k == 1) s.w.try_get_mut<HealShop3D>(m)->enabled = 0;
            s.run(30);
            CHECK(s.heals == 0);
            CHECK(s.w.try_get<Health3D>(p)->current == 40.0f);
        }
    }
}

TEST_CASE("wanted3d: a cash pickup pays whoever touches it (M-CASHPICKUP)") {
    {  // one-shot: paid once, the pickup is gone
        Scene s;
        const Entity p = s.player({}, 1.0f, 0.0f);
        s.w.add(p, Wallet3D{});
        const Entity c = s.w.spawn_with(at(0.0f, 1.0f, 0.0f), CashPickup3D{});
        s.run(6);
        CHECK(s.pickups == 1);
        CHECK(s.w.try_get<Wallet3D>(p)->cash == 50);
        CHECK(!s.w.alive(c));
    }
    {  // respawning: waits, then pays again while the holder stays
        Scene s;
        const Entity p = s.player({}, 1.0f, 0.0f);
        s.w.add(p, Wallet3D{});
        CashPickup3D cp;
        cp.respawn = 1.0f;
        const Entity c = s.w.spawn_with(at(0.0f, 1.0f, 0.0f), cp);
        s.run(6);
        CHECK(s.pickups == 1);
        CHECK(s.w.alive(c));
        s.run(30);
        CHECK(s.pickups == 1);  // still waiting
        s.run(60);
        CHECK(s.pickups == 2);
        CHECK(s.w.try_get<Wallet3D>(p)->cash == 100);
    }
    {  // too far, or no wallet: nothing is taken
        for (u32 k = 0; k < 2; ++k) {
            Scene s;
            const Entity p = s.player({}, k == 0 ? 30.0f : 1.0f, 0.0f);
            if (k == 0) s.w.add(p, Wallet3D{});
            const Entity c = s.w.spawn_with(at(0.0f, 1.0f, 0.0f), CashPickup3D{});
            s.run(30);
            CHECK(s.pickups == 0);
            CHECK(s.w.alive(c));
        }
    }
}

TEST_CASE("wanted3d: a completed mission pays its actor the MissionReward3D (M-MISSIONPAY)") {
    for (u32 k = 0; k < 3; ++k) {  // k 0: completes and pays; k 1: no wallet; k 2: fails, pays nothing
        Scene s;
        install_missions(s.w, nullptr);
        const Entity p = s.player();
        if (k != 1) s.w.add(p, Wallet3D{});
        Objective o;
        o.kind = ObjectiveKind::Signal;
        o.signal = "delivered";
        const Entity obj = s.w.spawn_with(o);
        Mission m;
        m.id = "job";
        m.objective = obj;
        m.actor = p;
        m.start_on_load = 1;
        if (k == 2) m.time_limit = 0.5f;
        const Entity mis = s.w.spawn_with(m, MissionReward3D{250});
        s.run(5);
        if (k != 2) {
            MissionSignal sig;
            sig.name = "delivered";
            EventAccess(s.w).push(sig);
        }
        s.run(60);
        if (k == 0) {
            CHECK(s.w.try_get<Mission>(mis)->state == MissionState::Complete);
            CHECK(s.w.try_get<Wallet3D>(p)->cash == 250);
            CHECK(s.cash_events == 1);
        } else {
            const Wallet3D* wl = s.w.try_get<Wallet3D>(p);
            CHECK((wl ? wl->cash : 0) == 0);
            CHECK(s.cash_events == 0);
        }
    }
}

TEST_CASE("wanted3d: a marker starts its mission and the wanted level can fail it (M-MISSIONSTART)") {
    const auto job = [](Scene& s, Entity p) {
        Objective o;
        o.kind = ObjectiveKind::Signal;
        o.signal = "never";
        const Entity obj = s.w.spawn_with(o);
        Mission m;
        m.id = "job";
        m.objective = obj;
        m.actor = p;
        return s.w.spawn_with(m);
    };
    for (u32 k = 0; k < 3; ++k) {  // k 0: actor at the marker; k 1: far; k 2: marker disabled
        Scene s;
        install_missions(s.w, nullptr);
        const Entity p = s.player({}, k == 1 ? 40.0f : 1.0f, 0.0f);
        const Entity mis = job(s, p);
        MissionMarker mk;
        mk.mission = mis;
        mk.enabled = k == 2 ? 0 : 1;
        s.w.spawn_with(at(0.0f, 1.0f, 0.0f), mk);
        s.run(10);
        CHECK((s.w.try_get<Mission>(mis)->state == MissionState::Active) == (k == 0));
    }
    for (u32 k = 0; k < 2; ++k) {  // k 0: wanted level 3 fails a level-3 mission; k 1: level 1 does not
        Scene s;
        install_missions(s.w, nullptr);
        Wanted3D wd;
        wd.heat = k == 0 ? 45.0f : 25.0f;
        const Entity p = s.player(wd, 0.0f, 0.0f);
        const Entity mis = job(s, p);
        s.w.add(mis, MissionWantedFail{3});
        s.w.try_get_mut<Mission>(mis)->start = 1;
        s.run(10);
        CHECK(s.w.try_get<Mission>(mis)->state == (k == 0 ? MissionState::Failed : MissionState::Active));
    }
}

TEST_CASE("wanted3d: a failed mission can be run again (M-MISSIONRETRY)") {
    for (u32 k = 0; k < 2; ++k) {  // k 0: MissionRetry; k 1: none
        Scene s;
        install_missions(s.w, nullptr);
        const Entity p = s.player({}, 0.0f, 0.0f);
        Objective ob;
        ob.kind = ObjectiveKind::Signal;
        const Entity obj = s.w.spawn_with(ob);
        Mission m;
        m.id = "job";
        m.objective = obj;
        m.actor = p;
        m.time_limit = 0.2f;
        const Entity mis = s.w.spawn_with(m);
        if (k == 0) s.w.add(mis, MissionRetry{0.5f, 0.0f});
        s.w.try_get_mut<Mission>(mis)->start = 1;
        s.run(60);
        CHECK(s.w.try_get<Mission>(mis)->state == (k == 0 ? MissionState::Idle : MissionState::Failed));
        CHECK(s.w.try_get<Objective>(obj)->state == (k == 0 ? ObjectiveState::Pending : ObjectiveState::Active));
        if (k == 0) {
            s.w.try_get_mut<Mission>(mis)->start = 1;
            s.run(2);
            CHECK(s.w.try_get<Mission>(mis)->state == MissionState::Active);
        }
    }
}

TEST_CASE("wanted3d: a chop shop buys a stolen car from its driver (M-CHOPSHOP)") {
    for (u32 k = 0; k < 3; ++k) {  // k 0: stolen car at the shop; k 1: a car nobody owns; k 2: the shop is far away
        Scene s;
        install_seats3d(s.w, nullptr);
        const Entity p = s.player({}, 0.0f, 0.0f);
        s.w.add(p, Rider3D{});
        s.w.add(p, Wallet3D{10});
        RigidBody3D b;
        b.half = {0.9f, 0.4f, 2.0f};
        b.density = 1200.0f / (8.0f * 0.9f * 0.4f * 2.0f);
        Vehicle3D v;
        v.front_axle = -1.4f;
        v.rear_axle = 1.4f;
        v.track = 0.9f;
        v.wheel_y = -0.3f;
        const Entity c = s.w.spawn_with(at(3.0f, 1.2f, 0.0f), b, v, VehicleSeat3D{});
        if (k != 1) s.w.add(c, CarOwner3D{});
        s.run(30);
        s.w.try_get_mut<Rider3D>(p)->enter = 1;
        s.run(4);
        CHECK(s.w.try_get<Rider3D>(p)->seated == 1);
        ChopShop3D shop;
        shop.price = 250;
        s.w.spawn_with(at(k == 2 ? 80.0f : 3.0f, 0.0f, 0.0f), shop);
        s.run(10);
        CHECK(s.w.alive(c) == (k != 0));
        CHECK(s.w.try_get<Wallet3D>(p)->cash == (k == 0 ? 260 : 10));
        if (k == 0) {
            s.run(10);
            CHECK(s.w.try_get<Rider3D>(p)->seated == 0);
        }
    }
}

TEST_CASE("wanted3d: boarding someone's car is a crime, once per car (M-CARJACK)") {
    Wanted3D wd;
    const auto car = [](Scene& s, bool owned) {
        RigidBody3D b;
        b.half = {0.9f, 0.4f, 2.0f};
        b.density = 1200.0f / (8.0f * 0.9f * 0.4f * 2.0f);
        Vehicle3D v;
        v.front_axle = -1.4f;
        v.rear_axle = 1.4f;
        v.track = 0.9f;
        v.wheel_y = -0.3f;
        const Entity c = s.w.spawn_with(at(3.0f, 1.2f, 0.0f), b, v, VehicleSeat3D{});
        if (owned) s.w.add(c, CarOwner3D{});
        return c;
    };
    const auto board = [](Scene& s, Entity p) {
        s.w.try_get_mut<Rider3D>(p)->enter = 1;
        s.run(4);
    };
    for (u32 k = 0; k < 3; ++k) {  // k 0: owned car; k 1: unowned car; k 2: no Wanted3D on the rider
        Scene s;
        install_seats3d(s.w, nullptr);
        const Entity p = s.player(wd, 0.0f, 0.0f);
        s.w.add(p, Rider3D{});
        if (k == 2) s.w.remove<Wanted3D>(p);
        const Entity c = car(s, k != 1);
        s.run(30);
        board(s, p);
        CHECK(s.w.try_get<Rider3D>(p)->seated == 1);
        if (k == 0) {
            CHECK(s.crimes == 1);
            CHECK(s.wanted(p).heat == doctest::Approx(8.0f));
            CHECK(s.w.try_get<CarOwner3D>(c)->stolen == 1);
            s.w.try_get_mut<Rider3D>(p)->enter = 1;  // step out and back in: no second crime
            s.run(240);
            s.w.try_get_mut<Rider3D>(p)->enter = 1;
            s.run(4);
            CHECK(s.crimes == 1);
        } else {
            CHECK(s.crimes == 0);
        }
    }
}

TEST_CASE("wanted3d: a theft that needs a witness counts only when police see the car (M-CARJACK 2)") {
    for (u32 k = 0; k < 3; ++k) {  // k 0: a unit nearby; k 1: none; k 2: needs_witness off, none
        Scene s;
        install_seats3d(s.w, nullptr);
        const Entity p = s.player({}, 0.0f, 0.0f);
        s.w.add(p, Rider3D{});
        RigidBody3D b;
        b.half = {0.9f, 0.4f, 2.0f};
        b.density = 1200.0f / (8.0f * 0.9f * 0.4f * 2.0f);
        Vehicle3D v;
        v.front_axle = -1.4f;
        v.rear_axle = 1.4f;
        v.track = 0.9f;
        v.wheel_y = -0.3f;
        CarOwner3D own;
        own.needs_witness = k == 2 ? 0 : 1;
        const Entity c = s.w.spawn_with(at(3.0f, 1.2f, 0.0f), b, v, VehicleSeat3D{}, own);
        if (k == 0) s.cop(3.0f, 12.0f);
        s.run(30);
        s.w.try_get_mut<Rider3D>(p)->enter = 1;
        s.run(4);
        CHECK(s.w.try_get<Rider3D>(p)->seated == 1);
        CHECK(s.w.try_get<CarOwner3D>(c)->stolen == 1);
        CHECK(s.crimes == (k == 1 ? 0u : 1u));
    }
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
