// Traffic on a road network (M-OPENWORLD B2): TrafficDriver3D keeps a Vehicle3D in its lane, follows
// the car ahead, stops at a red light and goes on green, stands at a stop line; TrafficSpawner3D
// keeps cars alive around a focus and removes the far ones; a run replays to the same hash.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

#include "mce/asset/container.h"
#include "mce/cook/cook.h"
#include "mce/cook/importer.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/crowd.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/roads.h"
#include "mce/kit/traffic.h"
#include "mce/kit/vehicles3d.h"

using namespace mce;
using namespace mce::kit;
using math::vec3;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
const asset::Guid kNet{0x7e7a000000004000ull, 0x80000000000000d1ull};

// A 400 m two-lane street along -z (s at z = 200, n at z = -200).
constexpr const char* kStreet = R"({ "format": "mce-roads", "version": 1,
  "nodes": [{"name": "s", "at": [0, 0, 200]}, {"name": "n", "at": [0, 0, -200]}],
  "roads": [{"name": "main", "from": "s", "to": "n", "lanes": 1, "speed": 12}] })";

// The same street with two lanes in each direction.
constexpr const char* kStreet2 = R"({ "format": "mce-roads", "version": 1,
  "nodes": [{"name": "s", "at": [0, 0, 200]}, {"name": "n", "at": [0, 0, -200]}],
  "roads": [{"name": "main", "from": "s", "to": "n", "lanes": 2, "speed": 12}] })";

// A signalled crossroads, arms 120 m; east-west green first (24 s until north-south's green).
constexpr const char* kCross = R"({ "format": "mce-roads", "version": 1,
  "nodes": [{"name": "c", "at": [0, 0, 0]}, {"name": "n", "at": [0, 0, -120]}, {"name": "s", "at": [0, 0, 120]},
            {"name": "e", "at": [120, 0, 0]}, {"name": "w", "at": [-120, 0, 0]}],
  "roads": [{"name": "north", "from": "c", "to": "n", "lanes": 1}, {"name": "south", "from": "c", "to": "s", "lanes": 1},
            {"name": "east", "from": "c", "to": "e", "lanes": 1}, {"name": "west", "from": "c", "to": "w", "lanes": 1}],
  "signals": [{"node": "c", "amber": 3, "all_red": 1,
               "phases": [{"green": ["east", "west"], "seconds": 20}, {"green": ["north", "south"], "seconds": 20}]}] })";

// The same crossroads as an all-way stop.
constexpr const char* kStop = R"({ "format": "mce-roads", "version": 1,
  "nodes": [{"name": "c", "at": [0, 0, 0], "control": "stop"}, {"name": "n", "at": [0, 0, -120]}, {"name": "s", "at": [0, 0, 120]},
            {"name": "e", "at": [120, 0, 0]}, {"name": "w", "at": [-120, 0, 0]}],
  "roads": [{"name": "north", "from": "c", "to": "n", "lanes": 1}, {"name": "south", "from": "c", "to": "s", "lanes": 1},
            {"name": "east", "from": "c", "to": "e", "lanes": 1}, {"name": "west", "from": "c", "to": "w", "lanes": 1}] })";

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

LocalTransform pose(vec3 p, f32 yaw = 0.0f) {
    return LocalTransform{p, math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, yaw), {1, 1, 1}};
}

// A 1.2 t car body (1.8 x 0.8 x 4 m) on four wheels.
Vehicle3D car_vehicle() {
    Vehicle3D v;
    v.front_axle = -1.4f;
    v.rear_axle = 1.4f;
    v.track = 0.9f;
    v.wheel_y = -0.3f;
    v.drive = VehicleDrive3D::All;
    v.anti_roll = 20000.0f;
    return v;
}

RigidBody3D car_body() {
    RigidBody3D b;
    b.half = {0.9f, 0.4f, 2.0f};
    b.density = 1200.0f / (8.0f * 0.9f * 0.4f * 2.0f);
    return b;
}

Entity spawn_car_hook(Frame& f, vec3 position, math::quat rotation, u32) {
    return f.commands().spawn(LocalTransform{position, rotation, {1, 1, 1}}, car_body(), car_vehicle());
}

struct Scene {
    World w;
    u64 t = 0;
    Entity net;
    explicit Scene(const char* roads) {
        w.register_components<LocalTransform>();
        install_traffic(w, nullptr);
        BoxCollider3D ground;
        ground.half = {400.0f, 0.5f, 400.0f};
        ground.friction = 1.0f;
        w.spawn_with(pose({0.0f, -0.5f, 0.0f}), ground);
        const Array<byte> payload = cook_roads(roads);
        REQUIRE(w.resource<RoadRegistry>().define(kNet, payload.span()));
        RoadNetwork3D n;
        n.roads.guid = kNet;
        net = w.spawn_with(pose({}), n);
    }
    void run(u32 ticks) {
        for (u32 i = 0; i < ticks; ++i) w.tick(kDt, t++);
    }
    // A traffic car at (x, z) facing yaw (0: -z).
    Entity car(f32 x, f32 z, f32 yaw = 0.0f, f32 max_speed = 0.0f) {
        TrafficDriver3D d;
        d.max_speed = max_speed;
        return w.spawn_with(pose({x, 1.2f, z}, yaw), car_body(), car_vehicle(), d);
    }
    // A wrecked/parked car: nobody drives it and its handbrake is on (M-TRAFFICAVOID).
    Entity wreck(f32 x, f32 z) {
        TrafficDriver3D d;
        d.drive = 0;
        Vehicle3D v = car_vehicle();
        v.handbrake = 1.0f;
        return w.spawn_with(pose({x, 1.2f, z}), car_body(), v, d);
    }
    vec3 pos(Entity e) { return w.try_get<LocalTransform>(e)->position; }
    const TrafficDriver3D& drv(Entity e) { return *w.try_get<TrafficDriver3D>(e); }
    f32 speed(Entity e) { return w.try_get<Vehicle3D>(e)->speed; }
};

}  // namespace

TEST_CASE("traffic: a car keeps to its lane at the lane's speed") {
    Scene s(kStreet);
    const Entity c = s.car(1.75f, 180.0f);  // the northbound lane (right of -z is +x)
    s.run(60 * 12);
    const TrafficDriver3D& d = s.drv(c);
    CHECK(d.state == TrafficState::Driving);
    CHECK(d.lane != asset::kRoadNone);
    CHECK(s.pos(c).z < 120.0f);
    CHECK(std::fabs(s.pos(c).x - 1.75f) < 0.6f);  // in its lane
    CHECK(s.speed(c) == doctest::Approx(12.0f).epsilon(0.15));
}

TEST_CASE("traffic: a faster car follows a slow one without hitting it") {
    Scene s(kStreet);
    const Entity slow = s.car(1.75f, 140.0f, 0.0f, 5.0f);
    const Entity fast = s.car(1.75f, 185.0f);
    f32 closest = 1e9f;
    for (u32 i = 0; i < 60 * 20; ++i) {
        s.run(1);
        closest = math::min(closest, s.pos(fast).z - s.pos(slow).z);
    }
    CHECK(s.drv(fast).leader == slow);
    CHECK(s.drv(fast).state == TrafficState::Following);
    CHECK(closest > 4.5f + 1.0f);  // never closer than a car length and a metre, rear to front
    CHECK(s.speed(fast) == doctest::Approx(5.0f).epsilon(0.25));
}

TEST_CASE("traffic: a car stops at a red light and goes on green") {
    Scene s(kCross);
    const Entity c = s.car(1.75f, 80.0f);  // the south arm heading north; north-south is red for 24 s
    s.run(60 * 18);
    CHECK(s.drv(c).state == TrafficState::Waiting);
    CHECK(std::fabs(s.speed(c)) < 0.5f);
    CHECK(s.pos(c).z > 3.5f);  // short of the junction (its lanes stop 4.5 m out)
    CHECK(s.pos(c).z < 15.0f);
    s.run(60 * 12);  // 30 s: green since 24
    CHECK(s.pos(c).z < -10.0f);  // through the junction
}

TEST_CASE("traffic: at a stop junction a car stands at the line, then crosses") {
    Scene s(kStop);
    const Entity c = s.car(1.75f, 60.0f);
    bool stood = false;
    for (u32 i = 0; i < 60 * 25; ++i) {
        s.run(1);
        if (s.pos(c).z > 3.5f && s.pos(c).z < 9.0f && std::fabs(s.speed(c)) < 0.5f) stood = true;
    }
    CHECK(stood);
    CHECK(s.pos(c).z < -10.0f);
}

TEST_CASE("traffic: a car stops for a pedestrian crossing in front, then drives on") {
    Scene s(kStreet);
    const Entity c = s.car(1.75f, 185.0f);
    Pedestrian3D p;
    p.mode = PedestrianMode::Cross;
    const Entity walker = s.w.spawn_with(pose({1.75f, 0.0f, 120.0f}), p);
    s.run(60 * 12);
    CHECK(s.drv(c).state == TrafficState::Yielding);
    CHECK(std::fabs(s.speed(c)) < 0.5f);
    CHECK(s.pos(c).z > 122.0f);  // short of the pedestrian
    CHECK(s.pos(c).z < 130.0f);
    s.w.try_get_mut<Pedestrian3D>(walker)->mode = PedestrianMode::Walk;  // across: the road is clear
    s.run(60 * 10);
    CHECK(s.pos(c).z < 100.0f);
}

TEST_CASE("traffic: a car also stops for a fleeing pedestrian on the road (M-PEDFLEE)") {
    Scene s(kStreet);
    const Entity c = s.car(1.75f, 185.0f);
    Pedestrian3D p;
    p.mode = PedestrianMode::Flee;
    const Entity runner = s.w.spawn_with(pose({1.75f, 0.0f, 120.0f}), p);
    s.run(60 * 12);
    CHECK(s.drv(c).state == TrafficState::Yielding);
    CHECK(std::fabs(s.speed(c)) < 0.5f);
    CHECK(s.pos(c).z > 122.0f);  // short of the runner
    s.w.try_get_mut<Pedestrian3D>(runner)->mode = PedestrianMode::Walk;
    s.run(60 * 10);
    CHECK(s.pos(c).z < 100.0f);
}

TEST_CASE("traffic: a spawner keeps cars around the focus and removes the far ones") {
    Scene s(kCross);
    TrafficSpawnHook hook;
    hook.spawn = &spawn_car_hook;
    s.w.set_resource(hook);
    const Entity focus = s.w.spawn_with(pose({0.0f, 0.0f, 0.0f}));
    TrafficSpawner3D sp;
    sp.focus = focus;
    sp.max_cars = 6;
    sp.min_radius = 20.0f;
    sp.spawn_radius = 110.0f;
    sp.despawn_radius = 150.0f;
    sp.seed = 7;
    const Entity se = s.w.spawn_with(sp);
    s.run(60 * 3);
    const TrafficSpawner3D& st = *s.w.try_get<TrafficSpawner3D>(se);
    CHECK(st.alive == 6);
    u32 drivers = 0;
    s.w.query<const TrafficDriver3D>().each([&](Entity e, const TrafficDriver3D& d) {
        ++drivers;
        CHECK(d.spawner == se);
        const vec3 p = s.pos(e);
        CHECK(std::sqrt(p.x * p.x + p.z * p.z) <= 115.0f);
    });
    CHECK(drivers == 6);
    // Move the focus away: the cars beyond 150 m go, new ones come near the focus... which here is
    // off the network, so nothing replaces them.
    s.w.try_get_mut<LocalTransform>(focus)->position = {2000.0f, 0.0f, 0.0f};
    s.run(10);
    CHECK(st.alive == 0);
    CHECK(st.removed == 6);
}

TEST_CASE("traffic: the same traffic replays to the same hash") {
    const auto run = [] {
        Scene s(kCross);
        TrafficSpawnHook hook;
        hook.spawn = &spawn_car_hook;
        s.w.set_resource(hook);
        TrafficSpawner3D sp;
        sp.max_cars = 8;
        sp.min_radius = 10.0f;
        sp.spawn_radius = 110.0f;
        sp.seed = 3;
        s.w.spawn_with(sp);
        s.run(60 * 15);
        return s.w.state_hash();
    };
    CHECK(run() == run());
}

TEST_CASE("traffic avoid: on a two-lane road a car goes around a wreck by taking the other lane") {
    Scene s(kStreet2);
    const Entity wreck = s.wreck(1.75f, 100.0f);
    const Entity c = s.car(1.75f, 180.0f);
    s.run(60 * 25);
    CHECK(s.pos(c).z < 80.0f);                      // it got past
    CHECK(s.drv(c).lane != s.drv(wreck).lane);     // by another lane
    CHECK(s.drv(c).state != TrafficState::Lost);
    CHECK(s.speed(wreck) < 0.5f);
}

TEST_CASE("traffic avoid: on a one-lane road a car queues behind the wreck as before") {
    Scene s(kStreet);
    s.wreck(1.75f, 100.0f);
    const Entity c = s.car(1.75f, 180.0f);
    s.run(60 * 25);
    CHECK(s.pos(c).z > 100.0f + 2.0f);
    CHECK(s.speed(c) < 0.5f);
}

TEST_CASE("traffic avoid: the other lane blocked beside the wreck means waiting") {
    Scene s(kStreet2);
    s.wreck(1.75f, 100.0f);
    s.wreck(5.25f, 98.0f);
    const Entity c = s.car(1.75f, 180.0f);
    s.run(60 * 25);
    CHECK(s.pos(c).z > 100.0f + 2.0f);
}
