// Pedestrian crowds on a road network's pavements (M-OPENWORLD B3): they stay on the pavements, gather
// around the focus and leave behind it, step less often when far, wait for the lights before they cross,
// and replay exactly.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

#include "mce/asset/container.h"
#include "mce/cook/cook.h"
#include "mce/cook/importer.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/crowd.h"
#include "mce/kit/roads.h"
#include "mce/kit/vehicles3d.h"

using namespace mce;
using namespace mce::kit;
using math::vec3;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
const asset::Guid kNet{0x7e7a000000004000ull, 0x80000000000000c2ull};

// The crossroads of test_roads with a 2.5 m pavement on every road: east-west green for 20 s, then north-south for 10 s.
constexpr const char* kCross = R"({ "format": "mce-roads", "version": 1, "name": "cross",
  "nodes": [{"name": "c", "at": [0, 0, 0]}, {"name": "n", "at": [0, 0, -60]}, {"name": "s", "at": [0, 0, 60]},
            {"name": "e", "at": [60, 0, 0]}, {"name": "w", "at": [-60, 0, 0]}],
  "roads": [{"name": "north", "from": "c", "to": "n", "lanes": 1, "sidewalk": 2.5}, {"name": "south", "from": "c", "to": "s", "lanes": 1, "sidewalk": 2.5},
            {"name": "east", "from": "c", "to": "e", "lanes": [2, 2], "sidewalk": 2.5}, {"name": "west", "from": "c", "to": "w", "lanes": [2, 2], "sidewalk": 2.5}],
  "signals": [{"node": "c", "amber": 3, "all_red": 1,
               "phases": [{"green": ["east", "west"], "seconds": 20}, {"green": ["north", "south"], "seconds": 10}]}] })";

Array<byte> cook_roads(const char* text) {
    cook::ImporterRegistry importers;
    cook::ImporterRegistry::add_builtins(importers);
    const cook::IImporter* imp = importers.for_extension(".mcroads");
    REQUIRE(imp);
    cook::AssetRegistry registry;
    cook::DiagList diags;
    Array<byte> cooked;
    REQUIRE(cook::cook_document(registry, *imp, kNet, "net.mcroads", text, cooked, diags));
    auto view = asset::ContainerView::open(cooked.span(), "net.mcroads");
    REQUIRE(view);
    const u32 chunk = view.value().find(asset::ChunkType::RoadNetwork, kNet);
    REQUIRE(chunk < view.value().chunk_count());
    const Span<const byte> p = view.value().payload(chunk);
    Array<byte> out;
    out.resize(p.size());
    std::memcpy(out.data(), p.data(), p.size());
    return out;
}

struct Scene {
    World w;
    u64 t = 0;
    Entity net, spawner, focus;
    explicit Scene(CrowdSpawner3D sp = {}, bool with_focus = false, vec3 focus_at = {}) {
        w.register_components<LocalTransform>();
        install_roads(w, nullptr);
        install_crowd(w, nullptr);
        const Array<byte> payload = cook_roads(kCross);
        REQUIRE(w.resource<RoadRegistry>().define(kNet, payload.span()));
        RoadNetwork3D n;
        n.roads.guid = kNet;
        n.lights = 0;  // the tests pick the light by `time`
        net = w.spawn_with(LocalTransform{{}, math::quat::identity(), {1, 1, 1}}, n);
        if (with_focus) {
            focus = w.spawn_with(LocalTransform{focus_at, math::quat::identity(), {1, 1, 1}});
            sp.focus = focus;
        }
        spawner = w.spawn_with(sp);
    }
    void run(u32 ticks) {
        for (u32 i = 0; i < ticks; ++i) w.tick(kDt, t++);
    }
    void set_time(f32 seconds) { (*w.try_get_mut<RoadNetwork3D>(net)).time = seconds; }
    void move_focus(vec3 p) { (*w.try_get_mut<LocalTransform>(focus)).position = p; }
    const CrowdStats& stats() { return *crowd_stats(w); }
    template <class F>
    void each(F&& fn) {
        w.query<const Pedestrian3D&, const LocalTransform&>().each([&](Entity, const Pedestrian3D& p, const LocalTransform& lt) { fn(p, lt); });
    }
};

CrowdSpawner3D spawner(u16 count, u32 seed = 1) {
    CrowdSpawner3D s;
    s.max_count = count;
    s.per_tick = 4;
    s.seed = seed;
    return s;
}

// How far the north arm's pavements stop short of the junction centre (the crossing line is at z = -trim).
f32 north_trim(Scene& s) {
    RoadNet net;
    REQUIRE(road_net(s.w, s.net, net));
    const u32 r = net.def->find_road("north");
    REQUIRE(r != asset::kRoadNone);
    const asset::RoadLane& l = net.def->lanes[net.def->roads[r].first_lane];
    const asset::RoadPoint& a = net.def->points[l.first_point];
    const asset::RoadPoint& b = net.def->points[l.first_point + l.point_count - 1];
    return std::fabs(a.z) < std::fabs(b.z) ? std::fabs(a.z) : std::fabs(b.z);
}

}  // namespace

TEST_CASE("crowd: pedestrians fill up and keep to the pavements") {
    Scene s(spawner(40));
    s.run(120);
    CHECK(s.stats().pedestrians == 40);
    RoadNet net;
    REQUIRE(road_net(s.w, s.net, net));
    u32 walking = 0;
    s.each([&](const Pedestrian3D& p, const LocalTransform& t) {
        CHECK(p.crowd == s.spawner.index + 1);
        if (p.target != 0xFFFFFFFFu) return;  // mid-hop at a junction
        ++walking;
        RoadSpot spot;
        if (road_nearest(net, t.position, spot)) CHECK(spot.distance > 2.5f);  // beside the carriageway, not on it
        CHECK(p.pace == doctest::Approx(p.speed));
        CHECK(t.position.y == doctest::Approx(0.0f));
    });
    CHECK(walking > 20);
    // They walk: a minute on, everyone has moved on and the population holds.
    s.run(3600);
    CHECK(s.stats().pedestrians == 40);
}

TEST_CASE("crowd: a hand-placed pedestrian snaps to the nearest pavement and walks it") {
    Scene s(spawner(0));
    Pedestrian3D p;
    p.speed = 1.5f;
    const Entity e = s.w.spawn_with(LocalTransform{{4.0f, 0.0f, -30.0f}, math::quat::identity(), {1, 1, 1}}, p);
    s.run(2);
    const vec3 a = (*s.w.try_get_mut<LocalTransform>(e)).position;
    CHECK(a.x == doctest::Approx(4.75f).epsilon(0.01));  // the north road's pavement: 3.5 m of carriageway + 1.25
    s.run(60);
    const vec3 b = (*s.w.try_get_mut<LocalTransform>(e)).position;
    CHECK(std::fabs(b.z - a.z) == doctest::Approx(1.5f).epsilon(0.1));
}

TEST_CASE("crowd: the same seed replays to the same hash, another seed does not") {
    Scene a(spawner(30, 7)), b(spawner(30, 7)), c(spawner(30, 8));
    a.run(900);
    b.run(900);
    c.run(900);
    CHECK(a.w.state_hash() == b.w.state_hash());
    CHECK(a.w.state_hash() != c.w.state_hash());
}

TEST_CASE("crowd: the crowd gathers around the focus and leaves behind it") {
    CrowdSpawner3D sp = spawner(30);
    sp.spawn_min = 20.0f;
    sp.spawn_max = 50.0f;
    sp.despawn_radius = 70.0f;
    Scene s(sp, true, {60.0f, 0.0f, 0.0f});
    s.run(300);
    CHECK(s.stats().pedestrians > 5);
    s.each([&](const Pedestrian3D&, const LocalTransform& t) { CHECK(std::hypot(t.position.x - 60.0f, t.position.z) < 71.0f); });
    // The focus moves far away: the crowd is removed and nobody spawns on a pavement that is not there.
    s.move_focus({600.0f, 0.0f, 600.0f});
    s.run(5);
    CHECK(s.stats().pedestrians == 0);
    s.run(120);
    CHECK(s.stats().pedestrians == 0);
}

TEST_CASE("crowd: far pedestrians step less often") {
    CrowdSpawner3D sp = spawner(40);
    sp.spawn_min = 0.0f;
    sp.spawn_max = 70.0f;
    sp.near_radius = 10.0f;
    sp.far_radius = 25.0f;
    sp.despawn_radius = 200.0f;
    Scene s(sp, true, {});
    s.run(300);
    u32 stepped = 0, pedestrians = 0;
    for (u32 i = 0; i < 16; ++i) {
        s.run(1);
        stepped += s.stats().stepped;
        pedestrians += s.stats().pedestrians;
    }
    CHECK(s.stats().mid + s.stats().far > 0);
    CHECK(s.stats().near + s.stats().mid + s.stats().far == s.stats().pedestrians);
    CHECK(stepped < pedestrians);
    // A far pedestrian still gets where it is going: its position keeps changing.
    s.each([&](const Pedestrian3D& p, const LocalTransform&) { CHECK(p.lag < 16 * kDt + 1e-4f); });
}

TEST_CASE("crowd: nobody crosses a road whose through traffic has the green") {
    const f32 ew_green = 5.0f, ns_green = 28.0f;  // phases: EW 0-20 (+3 amber +1 red), then NS 24-34
    u32 on_north_line_ew = 0;
    for (f32 time : {ew_green, ns_green}) {
        CrowdSpawner3D sp = spawner(60);
        sp.cross = 1.0f;
        Scene s(sp);
        const f32 trim = north_trim(s);
        s.set_time(time);
        u32 on_line = 0;
        for (u32 i = 0; i < 40; ++i) {
            s.run(60);
            s.each([&](const Pedestrian3D&, const LocalTransform& t) {
                if (std::fabs(t.position.x) < 3.0f && std::fabs(t.position.z + trim) < 1.0f) ++on_line;
            });
        }
        if (time == ew_green) on_north_line_ew = on_line;  // north-south is red: crossing the north road is free
        else CHECK(on_line == 0);                          // north-south has the green: nobody steps onto the road
    }
    CHECK(on_north_line_ew > 0);
}

TEST_CASE("crowd: pedestrians wait at the kerb for the light") {
    CrowdSpawner3D sp = spawner(60);
    sp.cross = 1.0f;
    Scene s(sp);
    (*s.w.try_get_mut<RoadNetwork3D>(s.net)).lights = 1;
    u32 waited = 0, crossed = 0;
    for (u32 i = 0; i < 90; ++i) {
        s.run(60);
        waited += s.stats().waiting;
        crossed += s.stats().crossing;
    }
    CHECK(waited > 0);
    CHECK(crossed > 0);
}

TEST_CASE("crowd: a dead pedestrian stops, lies about for ten seconds and is replaced (M-PEDDEAD)") {
    Scene s(spawner(0));
    Pedestrian3D p;
    p.speed = 1.5f;
    Health3D hp;
    hp.max = 50.0f;
    hp.current = 50.0f;
    const Entity e = s.w.spawn_with(LocalTransform{{4.0f, 0.0f, -30.0f}, math::quat::identity(), {1, 1, 1}}, p, hp);
    s.run(30);
    const vec3 before = s.w.try_get<LocalTransform>(e)->position;
    s.w.try_get_mut<Health3D>(e)->dead = 1;
    s.run(2);
    CHECK(s.w.try_get<Pedestrian3D>(e)->mode == PedestrianMode::Down);
    CHECK(s.stats().down == 1);
    CHECK(s.stats().pedestrians == 0);
    const vec3 fell = s.w.try_get<LocalTransform>(e)->position;
    s.run(60 * 5);
    const vec3 later = s.w.try_get<LocalTransform>(e)->position;
    CHECK(later.x == doctest::Approx(fell.x));
    CHECK(later.z == doctest::Approx(fell.z));
    CHECK(std::fabs(fell.z - before.z) < 1.0f);
    s.run(60 * 6);
    CHECK_FALSE(s.w.alive(e));  // gone after the corpse time
}

TEST_CASE("crowd: a revived pedestrian walks again (M-PEDDEAD)") {
    Scene s(spawner(0));
    Health3D hp;
    hp.max = 50.0f;
    hp.current = 50.0f;
    hp.dead = 1;
    const Entity e = s.w.spawn_with(LocalTransform{{4.0f, 0.0f, -30.0f}, math::quat::identity(), {1, 1, 1}}, Pedestrian3D{}, hp);
    s.run(10);
    CHECK(s.w.try_get<Pedestrian3D>(e)->mode == PedestrianMode::Down);
    s.w.try_get_mut<Health3D>(e)->dead = 0;
    s.run(30);
    CHECK(s.w.try_get<Pedestrian3D>(e)->mode != PedestrianMode::Down);
    CHECK(s.w.try_get<Pedestrian3D>(e)->pace > 0.0f);
}

TEST_CASE("crowd: a hurt pedestrian runs from its attacker, ignores the light and calms down (M-PEDFLEE)") {
    for (const f32 attacker_z : {-10.0f, -50.0f}) {
        Scene s(spawner(0));
        Pedestrian3D p;
        p.speed = 1.5f;
        Health3D hp;
        hp.max = 50.0f;
        hp.current = 50.0f;
        const Entity e = s.w.spawn_with(LocalTransform{{4.0f, 0.0f, -30.0f}, math::quat::identity(), {1, 1, 1}}, p, hp);
        const Entity foe = s.w.spawn_with(LocalTransform{{4.0f, 0.0f, attacker_z}, math::quat::identity(), {1, 1, 1}});
        s.run(30);
        CHECK(s.w.try_get<Pedestrian3D>(e)->mode != PedestrianMode::Flee);  // never hurt: walks
        {
            Health3D* h = s.w.try_get_mut<Health3D>(e);
            h->hits = 1;
            h->since_hit = 0.0f;
            h->last_attacker = foe;
        }
        s.run(2);
        const Pedestrian3D* pe = s.w.try_get<Pedestrian3D>(e);
        CHECK(pe->mode == PedestrianMode::Flee);
        CHECK(pe->pace == doctest::Approx(1.5f * kPedestrianFleeScale));
        const vec3 start = s.w.try_get<LocalTransform>(e)->position;
        s.run(20);
        const vec3 now = s.w.try_get<LocalTransform>(e)->position;
        const f32 d0 = std::fabs(start.z - attacker_z), d1 = std::fabs(now.z - attacker_z);
        CHECK(d1 > d0);  // farther from the attacker than a moment ago
        s.w.try_get_mut<Health3D>(e)->since_hit = kPedestrianFleeSeconds + 1.0f;
        s.run(60 * 6);  // the warning it gave the others has run out too
        CHECK(s.w.try_get<Pedestrian3D>(e)->mode != PedestrianMode::Flee);
        CHECK(s.w.try_get<Pedestrian3D>(e)->pace == doctest::Approx(1.5f));
    }
}

TEST_CASE("crowd: a shot nearby sends pedestrians running, a far one does not, and they calm down (M-PEDFLEE 2)") {
    for (const f32 shot_z : {-20.0f, -40.0f}) {
        Scene s(spawner(0));
        Pedestrian3D p;
        p.speed = 1.5f;
        const Entity e = s.w.spawn_with(LocalTransform{{4.0f, 0.0f, -30.0f}, math::quat::identity(), {1, 1, 1}}, p);
        s.run(30);
        CHECK(s.w.try_get<Pedestrian3D>(e)->mode != PedestrianMode::Flee);
        WeaponFired3D shot;
        shot.origin = {4.0f, 1.0f, shot_z};
        shot.end = {4.0f, 1.0f, shot_z + 50.0f};
        EventAccess(s.w).push(shot);
        s.run(3);
        CHECK(s.w.try_get<Pedestrian3D>(e)->mode == PedestrianMode::Flee);
        const f32 d0 = std::fabs(s.w.try_get<LocalTransform>(e)->position.z - shot_z);
        s.run(20);
        CHECK(std::fabs(s.w.try_get<LocalTransform>(e)->position.z - shot_z) > d0);  // away from the muzzle, whichever side it is on
        s.run(60 * 6);
        CHECK(s.w.try_get<Pedestrian3D>(e)->mode != PedestrianMode::Flee);
    }
    Scene far(spawner(0));
    Pedestrian3D p;
    const Entity e = far.w.spawn_with(LocalTransform{{4.0f, 0.0f, -30.0f}, math::quat::identity(), {1, 1, 1}}, p);
    WeaponFired3D shot;
    shot.origin = {4.0f, 1.0f, -30.0f + kPedestrianFearRadius + 20.0f};
    EventAccess(far.w).push(shot);
    far.run(5);
    CHECK(far.w.try_get<Pedestrian3D>(e)->mode != PedestrianMode::Flee);
}

TEST_CASE("crowd: a hard vehicle crash nearby sends pedestrians running, a soft one or a far one does not (M-PEDCRASH)") {
    struct Case {
        f32 speed, z;
        bool flees;
    };
    for (const Case k : {Case{9.0f, -20.0f, true}, Case{2.0f, -20.0f, false}, Case{9.0f, -30.0f + kPedestrianFearRadius + 20.0f, false}}) {
        Scene s(spawner(0));
        Pedestrian3D p;
        p.speed = 1.5f;
        const Entity e = s.w.spawn_with(LocalTransform{{4.0f, 0.0f, -30.0f}, math::quat::identity(), {1, 1, 1}}, p);
        const Entity car = s.w.spawn_with(LocalTransform{{4.0f, 0.0f, k.z}, math::quat::identity(), {1, 1, 1}}, Vehicle3D{});
        s.run(30);
        CHECK(s.w.try_get<Pedestrian3D>(e)->mode != PedestrianMode::Flee);
        ContactBegin3D hit;
        hit.a = car;
        hit.point = {4.0f, 1.0f, k.z};
        hit.approach_speed = k.speed;
        EventAccess(s.w).push(hit);
        s.run(3);
        CHECK((s.w.try_get<Pedestrian3D>(e)->mode == PedestrianMode::Flee) == k.flees);
        if (k.flees) {
            const f32 d0 = std::fabs(s.w.try_get<LocalTransform>(e)->position.z - k.z);
            s.run(20);
            CHECK(std::fabs(s.w.try_get<LocalTransform>(e)->position.z - k.z) > d0);  // away from the crash
            s.run(60 * 6);
            CHECK(s.w.try_get<Pedestrian3D>(e)->mode != PedestrianMode::Flee);
        }
    }
    // Two things that are not vehicles hitting each other are not a crash.
    Scene s(spawner(0));
    const Entity e = s.w.spawn_with(LocalTransform{{4.0f, 0.0f, -30.0f}, math::quat::identity(), {1, 1, 1}}, Pedestrian3D{});
    ContactBegin3D hit;
    hit.point = {4.0f, 1.0f, -25.0f};
    hit.approach_speed = 20.0f;
    EventAccess(s.w).push(hit);
    s.run(5);
    CHECK(s.w.try_get<Pedestrian3D>(e)->mode != PedestrianMode::Flee);
}

TEST_CASE("crowd: a fast vehicle bearing down on a pedestrian sends it running, a slow, a departing or a passing one does not (M-PEDCRASH 2)") {
    struct Case {
        f32 speed, car_z, car_x;
        bool flees;
    };
    // The vehicle drives towards -z (identity rotation) from (car_x, car_z); the pedestrian stands at (4, -30).
    for (const Case k : {Case{12.0f, -10.0f, 4.0f, true}, Case{12.0f, -10.0f, 10.0f, false}, Case{3.0f, -10.0f, 4.0f, false}, Case{12.0f, -50.0f, 4.0f, false},
                         Case{-12.0f, -10.0f, 4.0f, false}, Case{12.0f, 60.0f, 4.0f, false}}) {
        Scene s(spawner(0));
        Pedestrian3D p;
        p.speed = 1.5f;
        const Entity e = s.w.spawn_with(LocalTransform{{4.0f, 0.0f, -30.0f}, math::quat::identity(), {1, 1, 1}}, p);
        Vehicle3D v;
        v.speed = k.speed;
        s.w.spawn_with(LocalTransform{{k.car_x, 0.0f, k.car_z}, math::quat::identity(), {1, 1, 1}}, v);
        s.run(3);
        CHECK((s.w.try_get<Pedestrian3D>(e)->mode == PedestrianMode::Flee) == k.flees);
    }
}

TEST_CASE("crowd: panic spreads to close neighbours of a running pedestrian and not to far ones (M-PEDFLEE 3)") {
    Scene s(spawner(0));
    Pedestrian3D p;
    p.speed = 1.5f;
    const auto walker = [&](f32 z) { return s.w.spawn_with(LocalTransform{{4.0f, 0.0f, z}, math::quat::identity(), {1, 1, 1}}, p); };
    const Entity a = walker(-30.0f), b = walker(-37.0f), c = walker(-70.0f);
    s.run(10);
    WeaponFired3D shot;
    shot.origin = {4.0f, 1.0f, -5.0f};  // 25 m from a: inside the fear radius; b is 32 m away, c 65 m
    shot.end = {4.0f, 1.0f, -55.0f};
    EventAccess(s.w).push(shot);
    s.run(3);
    CHECK(s.w.try_get<Pedestrian3D>(a)->mode == PedestrianMode::Flee);
    s.run(3);
    CHECK(s.w.try_get<Pedestrian3D>(b)->mode == PedestrianMode::Flee);  // caught from a
    CHECK(s.w.try_get<Pedestrian3D>(c)->mode != PedestrianMode::Flee);
    s.run(60 * 8);
    CHECK(s.w.try_get<Pedestrian3D>(a)->mode != PedestrianMode::Flee);
    CHECK(s.w.try_get<Pedestrian3D>(b)->mode != PedestrianMode::Flee);  // a ripple that dies out
}
