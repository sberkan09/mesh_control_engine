// Road networks at run time (M-OPENWORLD B1): a cooked .mcroads placed by RoadNetwork3D, the
// nearest-lane query, lane points, routes through junctions, the traffic lights' clock and states,
// and the debug lines.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

#include "mce/asset/container.h"
#include "mce/cook/cook.h"
#include "mce/cook/importer.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/roads.h"
#include "mce/string_id.h"

using namespace mce;
using namespace mce::kit;
using math::vec3;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
const asset::Guid kNet{0x7e7a000000004000ull, 0x80000000000000c1ull};

// A signalled crossroads at the origin, arms 60 m out; east-west two lanes each way.
constexpr const char* kCross = R"({ "format": "mce-roads", "version": 1, "name": "cross",
  "nodes": [{"name": "c", "at": [0, 0, 0]}, {"name": "n", "at": [0, 0, -60]}, {"name": "s", "at": [0, 0, 60]},
            {"name": "e", "at": [60, 0, 0]}, {"name": "w", "at": [-60, 0, 0]}],
  "roads": [{"name": "north", "from": "c", "to": "n", "lanes": 1}, {"name": "south", "from": "c", "to": "s", "lanes": 1},
            {"name": "east", "from": "c", "to": "e", "lanes": [2, 2]}, {"name": "west", "from": "c", "to": "w", "lanes": [2, 2]}],
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
    Entity net;
    explicit Scene(vec3 origin = {}) {
        w.register_components<LocalTransform>();
        install_roads(w, nullptr);
        const Array<byte> payload = cook_roads(kCross);
        REQUIRE(w.resource<RoadRegistry>().define(kNet, payload.span()));
        RoadNetwork3D n;
        n.roads.guid = kNet;
        net = w.spawn_with(LocalTransform{origin, math::quat::identity(), {1, 1, 1}}, n);
    }
    void run(u32 ticks) {
        for (u32 i = 0; i < ticks; ++i) w.tick(kDt, t++);
    }
    RoadNet get() {
        RoadNet n;
        REQUIRE(road_net(w, net, n));
        return n;
    }
};

}  // namespace

TEST_CASE("roads: a placed network answers the nearest lane, keeping to the heading") {
    Scene s({100.0f, 0.0f, 0.0f});
    const RoadNet net = s.get();
    REQUIRE(net.def);
    CHECK(net.def->name.view() == StringView("cross"));
    CHECK(net.origin.x == doctest::Approx(100.0f));
    // On the south arm, just right of the centre line (x = +1.75 in the network): the lane heading -z.
    RoadSpot spot;
    REQUIRE(road_nearest(net, {101.5f, 0.0f, 30.0f}, spot));
    const asset::RoadLane& l = net.def->lanes[spot.lane];
    CHECK(l.kind == asset::RoadLaneKind::Road);
    CHECK(net.def->roads[l.road].name == StringId("south").value);
    CHECK(spot.direction.z == doctest::Approx(-1.0f));
    CHECK(spot.position.x == doctest::Approx(101.75f));
    CHECK(spot.distance == doctest::Approx(0.25f));
    // Asking with the opposite heading finds the other direction's lane instead.
    RoadNearestQuery q;
    q.heading = {0.0f, 0.0f, 1.0f};
    REQUIRE(road_nearest(net, {101.5f, 0.0f, 30.0f}, spot, q));
    CHECK(spot.direction.z == doctest::Approx(1.0f));
    CHECK(spot.position.x == doctest::Approx(98.25f));
    // Out of reach.
    q = {};
    q.max_distance = 5.0f;
    CHECK_FALSE(road_nearest(net, {130.0f, 0.0f, 30.0f}, spot, q));
    // The point along the lane, with s.
    vec3 dir;
    const vec3 p = road_lane_point(net, spot.lane, 0.0f, &dir);
    REQUIRE(road_nearest(net, {101.5f, 0.0f, 30.0f}, spot));
    const vec3 p10 = road_lane_point(net, spot.lane, 10.0f, &dir);
    CHECK(dir.z == doctest::Approx(-1.0f));
    CHECK(p.z - p10.z == doctest::Approx(10.0f));
    CHECK(net.def->find_road("east") != asset::kRoadNone);
    CHECK(net.def->find_node("c") == 0);
}

TEST_CASE("roads: a route crosses the junction on the connector for the turn") {
    Scene s;
    const RoadNet net = s.get();
    const RoadRegistry::Def& d = *net.def;
    RoadSpot from, to;
    REQUIRE(road_nearest(net, {1.75f, 0.0f, 40.0f}, from));  // south arm, heading north
    RoadNearestQuery west;
    west.heading = {-1.0f, 0.0f, 0.0f};
    REQUIRE(road_nearest(net, {-40.0f, 0.0f, -3.0f}, to, west));  // west arm, heading west
    Array<u32> route;
    REQUIRE(road_route(d, from.lane, to.lane, route));
    REQUIRE(route.size() == 3);
    CHECK(route[0] == from.lane);
    CHECK(route[2] == to.lane);
    const asset::RoadLane& c = d.lanes[route[1]];
    CHECK(c.kind == asset::RoadLaneKind::Connector);
    CHECK(c.index == u8(asset::RoadTurn::Left));
    // Every step is a successor of the one before.
    for (usize i = 0; i + 1 < route.size(); ++i) {
        const asset::RoadLane& l = d.lanes[route[i]];
        CHECK(route[i + 1] >= l.first_next);
        CHECK(route[i + 1] < l.first_next + l.next_count);
    }
    // A lane cannot reach itself backwards through nothing: the north arm's dead end turns around, so
    // going from the south arm to the lane heading back south is three junction hops away.
    RoadNearestQuery south;
    south.heading = {0.0f, 0.0f, 1.0f};
    RoadSpot back;
    REQUIRE(road_nearest(net, {-1.75f, 0.0f, 40.0f}, back, south));
    REQUIRE(road_route(d, from.lane, back.lane, route));
    CHECK(route.size() >= 5);
}

TEST_CASE("roads: the lights run on the network's clock") {
    Scene s;
    RoadNet net = s.get();
    const RoadRegistry::Def& d = *net.def;
    const u32 east = d.find_road("east"), north = d.find_road("north"), west = d.find_road("west"), south = d.find_road("south");
    // A connector from east to west (phase 0) and one from south to north (phase 1).
    u32 ew = asset::kRoadNone, sn = asset::kRoadNone;
    const asset::RoadNode& c = d.nodes[0];
    for (u32 i = c.first_connector; i < c.first_connector + c.connector_count; ++i) {
        const asset::RoadLane& l = d.lanes[i];
        const u16 a = d.lanes[l.prev].road, b = d.lanes[l.first_next].road;
        if (a == east && b == west) ew = i;
        if (a == south && b == north) sn = i;
    }
    REQUIRE(ew != asset::kRoadNone);
    REQUIRE(sn != asset::kRoadNone);
    f32 left = 0.0f;
    CHECK(road_light(net, ew, &left) == RoadLight::Green);
    CHECK(left == doctest::Approx(20.0f));
    CHECK(road_light(net, sn, &left) == RoadLight::Red);
    CHECK(left == doctest::Approx(24.0f));  // 20 green + 3 amber + 1 all red
    // 21 s in: east-west amber, north-south still red.
    s.run(60 * 21);
    net = s.get();
    CHECK(net.time == doctest::Approx(21.0f).epsilon(1e-3));
    CHECK(road_light(net, ew, &left) == RoadLight::Amber);
    CHECK(left == doctest::Approx(2.0f).epsilon(1e-2));
    CHECK(road_light(net, sn) == RoadLight::Red);
    // 30 s in: north-south green, east-west red until the cycle comes round (38 s).
    s.run(60 * 9);
    net = s.get();
    CHECK(road_light(net, sn) == RoadLight::Green);
    CHECK(road_light(net, ew, &left) == RoadLight::Red);
    CHECK(left == doctest::Approx(8.0f).epsilon(1e-2));
    // A road lane and an unsignalled connector are always green.
    CHECK(road_light(net, d.lanes[ew].prev) == RoadLight::Green);
    // Holding the lights stops the clock.
    s.w.try_get_mut<RoadNetwork3D>(s.net)->lights = 0;
    s.run(60);
    CHECK(s.get().time == doctest::Approx(net.time));
}

TEST_CASE("roads: debug lines trace every lane, connectors in their light's colour") {
    Scene s({0.0f, 2.0f, 0.0f});
    Array<RoadLine> lines;
    road_debug_lines(s.w, lines);
    const RoadNet net = s.get();
    usize segments = 0;
    for (const asset::RoadLane& l : net.def->lanes) segments += l.point_count - 1;
    CHECK(lines.size() == segments);
    bool green = false, red = false;
    for (const RoadLine& l : lines) {
        CHECK(l.a.y > 2.0f);
        green |= l.color.g > 0.8f && l.color.r < 0.3f;
        red |= l.color.r > 0.9f && l.color.g < 0.3f;
    }
    CHECK(green);
    CHECK(red);
}

TEST_CASE("roads: an unresolved network answers nothing") {
    World w;
    w.register_components<LocalTransform>();
    install_roads(w, nullptr);
    RoadNetwork3D n;
    n.roads.guid = asset::Guid{1, 2};
    const Entity e = w.spawn_with(LocalTransform{}, n);
    RoadNet net;
    CHECK_FALSE(road_net(w, e, net));
    CHECK_FALSE(road_net_first(w, net));
    RoadSpot spot;
    CHECK_FALSE(road_nearest(net, {}, spot));
    CHECK(road_light(net, 0) == RoadLight::Green);
}
