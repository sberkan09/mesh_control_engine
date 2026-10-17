// M-OPENWORLD B1: a .mcroads cooks to CHNK_ROADS (asset::RoadView): lanes keep to the drive side,
// junctions get connectors with the right turns, conflicts, yields and signal phases; authoring
// mistakes are cook errors naming the file.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/asset/roads_format.h"
#include "mce/cook/cook.h"
#include "mce/string_id.h"

using namespace mce;
using namespace cook_test;
using namespace mce::asset;

namespace {

std::string diags_of(const cook::CookReport& r) {
    std::string s;
    for (const auto& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += line.c_str();
        s += "\n";
    }
    return s;
}

std::string doc(const std::string& fields) {
    return std::string(R"({ "format": "mce-roads", "version": 1, "guid": "7e7a0000-0000-4000-8000-0000000000b1", )") + fields + " }";
}

// Cooks `text` as assets/net.mcroads and copies the payload out.
struct Cooked {
    bool ok = false;
    std::string diags;
    Array<byte> payload;
    RoadView view;
};

Cooked cook_roads(const std::string& text) {
    Cooked c;
    const std::string root = fresh_dir("roads_cook");
    write_file(root + "/assets/net.mcroads", text);
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    c.diags = diags_of(r);
    if (!r.ok()) return c;
    AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    if (!assets.mount(Span<const StringView>(roots, 1))) return c;
    const AssetKey key = assets.find(parse_guid("7e7a0000-0000-4000-8000-0000000000b1").value());
    if (key == kInvalidAssetKey || assets.info(key).type != ChunkType::RoadNetwork) return c;
    auto h = assets.load_sync<RoadNetwork>(key);
    if (!h) return c;
    const Span<const byte> p = assets.resolve(h.value());
    c.payload.resize(p.size());
    std::memcpy(c.payload.data(), p.data(), p.size());
    auto v = RoadView::read(c.payload.span());
    if (!v) return c;
    c.view = v.value();
    c.ok = true;
    return c;
}

const RoadPoint& first(const RoadView& v, const RoadLane& l) { return v.points[l.first_point]; }
const RoadPoint& last(const RoadView& v, const RoadLane& l) { return v.points[l.first_point + l.point_count - 1]; }

u32 road_index(const RoadView& v, const char* name) {
    for (u32 i = 0; i < v.roads.size(); ++i)
        if (v.roads[i].name == StringId(name).value) return i;
    return kRoadNone;
}

// The connector at `node` from road `a` into road `b` (the first found).
const RoadLane* connector(const RoadView& v, u32 node, u32 a, u32 b) {
    const RoadNode& n = v.nodes[node];
    for (u32 c = n.first_connector; c < n.first_connector + n.connector_count; ++c) {
        const RoadLane& l = v.lanes[c];
        if (v.lanes[l.prev].road == a && v.lanes[l.first_next].road == b) return &l;
    }
    return nullptr;
}

bool conflicts_with(const RoadView& v, const RoadLane& a, const RoadLane& b) {
    const u32 bi = static_cast<u32>(&b - v.lanes.data());
    for (u32 k = 0; k < a.conflict_count; ++k)
        if (v.conflicts[a.first_conflict + k] == bi) return true;
    return false;
}

// A crossroads: centre c, arms n (-z), s (+z), e (+x), w (-x), 60 m out, one lane each way.
std::string crossroads(const std::string& centre_extra, const std::string& extra) {
    return doc(R"("name": "cross", "nodes": [
        {"name": "c", "at": [0, 0, 0])" + centre_extra + R"(},
        {"name": "n", "at": [0, 0, -60]}, {"name": "s", "at": [0, 0, 60]},
        {"name": "e", "at": [60, 0, 0]}, {"name": "w", "at": [-60, 0, 0]}],
      "roads": [
        {"name": "north", "from": "c", "to": "n", "lanes": 1},
        {"name": "south", "from": "c", "to": "s", "lanes": 1},
        {"name": "east", "from": "c", "to": "e", "lanes": 1, "priority": 1},
        {"name": "west", "from": "c", "to": "w", "lanes": 1, "priority": 1}])" +
               extra);
}

}  // namespace

TEST_CASE("roads: a two-way road keeps right, its dead ends turn around") {
    const Cooked c = cook_roads(doc(R"("name": "strip", "nodes": [{"name": "a", "at": [0, 0, 0]}, {"name": "b", "at": [100, 0, 0]}],
        "roads": [{"name": "main", "from": "a", "to": "b", "lanes": 1, "speed": 10}])"));
    INFO(c.diags);
    REQUIRE(c.ok);
    const RoadView& v = c.view;
    CHECK(v.name() == StringView("strip"));
    REQUIRE(v.roads.size() == 1);
    CHECK(v.roads[0].length == doctest::Approx(100.0f));
    REQUIRE(v.lanes.size() == 4);  // two road lanes and a U-turn at each dead end
    const RoadLane& fwd = v.lanes[0];
    const RoadLane& back = v.lanes[1];
    CHECK(fwd.kind == RoadLaneKind::Road);
    CHECK((back.flags & RoadLaneFlags::Backward) != 0);
    // Facing +x the right is +z: the forward lane lies at z = +1.75, the backward one at -1.75.
    CHECK(first(v, fwd).z == doctest::Approx(1.75f));
    CHECK(first(v, fwd).x == doctest::Approx(0.0f));
    CHECK(last(v, fwd).x == doctest::Approx(100.0f));
    CHECK(first(v, back).z == doctest::Approx(-1.75f));
    CHECK(first(v, back).x == doctest::Approx(100.0f));
    CHECK(fwd.length == doctest::Approx(100.0f));
    CHECK(fwd.speed == doctest::Approx(10.0f));
    // The forward lane ends at b, where a U-turn takes it onto the backward lane.
    CHECK(fwd.node == 1);
    REQUIRE(fwd.next_count == 1);
    const RoadLane& u = v.lanes[fwd.first_next];
    CHECK(u.kind == RoadLaneKind::Connector);
    CHECK(u.index == u8(RoadTurn::UTurn));
    CHECK(u.prev == 0);
    CHECK(u.first_next == 1);
    CHECK(v.nodes[1].control == RoadControl::None);
    CHECK(v.header->bounds_max[0] >= 100.0f);
}

TEST_CASE("roads: drive left mirrors the lanes") {
    const Cooked c = cook_roads(doc(R"("drive": "left", "nodes": [{"name": "a", "at": [0, 0, 0]}, {"name": "b", "at": [100, 0, 0]}],
        "roads": [{"name": "main", "from": "a", "to": "b", "lanes": [2, 0], "lane_width": 3}])"));
    INFO(c.diags);
    REQUIRE(c.ok);
    const RoadView& v = c.view;
    CHECK((v.header->flags & RoadFlags::LeftHand) != 0);
    REQUIRE(v.lanes.size() == 2);  // one way: no U-turn
    // One-way lanes straddle the centreline; lane 0 is the innermost, on the right when driving left.
    CHECK(first(v, v.lanes[0]).z == doctest::Approx(1.5f));
    CHECK(first(v, v.lanes[1]).z == doctest::Approx(-1.5f));
    CHECK(v.lanes[0].next_count == 0);  // a one-way dead end
}

TEST_CASE("roads: a crossroads gets every turn, its conflicts and the priority road's right of way") {
    const Cooked c = cook_roads(crossroads("", ""));
    INFO(c.diags);
    REQUIRE(c.ok);
    const RoadView& v = c.view;
    const RoadNode& centre = v.nodes[0];
    CHECK(centre.control == RoadControl::Priority);
    CHECK(centre.road_count == 4);
    CHECK(centre.connector_count == 12);  // four approaches: straight, left and right each
    const u32 north = road_index(v, "north"), south = road_index(v, "south"), east = road_index(v, "east"), west = road_index(v, "west");
    // From the south (heading -z): north is straight on, east is a right turn, west a left.
    const RoadLane* s_n = connector(v, 0, south, north);
    const RoadLane* s_e = connector(v, 0, south, east);
    const RoadLane* s_w = connector(v, 0, south, west);
    const RoadLane* n_s = connector(v, 0, north, south);
    const RoadLane* e_w = connector(v, 0, east, west);
    const RoadLane* w_e = connector(v, 0, west, east);
    REQUIRE(s_n);
    REQUIRE(s_e);
    REQUIRE(s_w);
    REQUIRE(n_s);
    REQUIRE(e_w);
    REQUIRE(w_e);
    CHECK(s_n->index == u8(RoadTurn::Straight));
    CHECK(s_e->index == u8(RoadTurn::Right));
    CHECK(s_w->index == u8(RoadTurn::Left));
    // The east-west road has priority: its connectors outrank the minor road's, which yield.
    CHECK((s_n->flags & RoadLaneFlags::Yield) != 0);
    CHECK((e_w->flags & RoadLaneFlags::Yield) == 0);
    CHECK(e_w->rank > s_n->rank);
    // Crossing paths conflict; a left turn crosses the oncoming straight; opposite rights do not meet.
    CHECK(conflicts_with(v, *s_n, *e_w));
    CHECK(conflicts_with(v, *s_n, *w_e));
    CHECK(conflicts_with(v, *s_w, *n_s));
    CHECK_FALSE(conflicts_with(v, *s_e, *connector(v, 0, north, west)));
    // Forks from one lane never conflict.
    CHECK_FALSE(conflicts_with(v, *s_n, *s_e));
    // A right turn is slower than the road: its bend caps the speed.
    CHECK(s_e->speed < s_n->speed);
    // The lanes stop short of the junction: one lane each way (3.5 m half width) + 1 m.
    const RoadLane& in = v.lanes[s_n->prev];
    CHECK(std::fabs(last(v, in).z) == doctest::Approx(4.5f).epsilon(0.01));
}

TEST_CASE("roads: a signal turns its phases into green bits on the connectors") {
    const Cooked c = cook_roads(crossroads("", R"(, "signals": [{"node": "c", "amber": 2, "all_red": 1, "offset": 5,
        "phases": [{"green": ["east", "west"], "seconds": 20}, {"green": ["north", "south"], "seconds": 10}]}])"));
    INFO(c.diags);
    REQUIRE(c.ok);
    const RoadView& v = c.view;
    REQUIRE(v.signals.size() == 1);
    const RoadSignal& s = v.signals[0];
    CHECK(v.nodes[0].control == RoadControl::Signal);
    CHECK(v.nodes[0].signal == 0);
    CHECK(s.phase_count == 2);
    CHECK(s.cycle == doctest::Approx(36.0f));  // 20 + 2 + 1 + 10 + 2 + 1
    CHECK(s.offset == doctest::Approx(5.0f));
    CHECK(v.phases[s.first_phase + 1].start == doctest::Approx(23.0f));
    const u32 north = road_index(v, "north"), south = road_index(v, "south"), east = road_index(v, "east"), west = road_index(v, "west");
    CHECK(connector(v, 0, east, west)->green == 1u);
    CHECK(connector(v, 0, south, west)->green == 2u);
    CHECK(connector(v, 0, north, east)->green == 2u);
    // Signalled connectors do not carry the priority road's Yield flag.
    CHECK((connector(v, 0, south, north)->flags & RoadLaneFlags::Yield) == 0);
}

TEST_CASE("roads: a stop junction makes every approach stop and yield; no_turns are left out") {
    const Cooked c = cook_roads(crossroads(R"(, "control": "stop")", R"(, "no_turns": [{"node": "c", "from": "south", "to": "west"}])"));
    INFO(c.diags);
    REQUIRE(c.ok);
    const RoadView& v = c.view;
    CHECK(v.nodes[0].connector_count == 11);
    CHECK(connector(v, 0, road_index(v, "south"), road_index(v, "west")) == nullptr);
    const RoadLane* e_w = connector(v, 0, road_index(v, "east"), road_index(v, "west"));
    REQUIRE(e_w);
    CHECK((e_w->flags & (RoadLaneFlags::Stop | RoadLaneFlags::Yield)) == (RoadLaneFlags::Stop | RoadLaneFlags::Yield));
}

TEST_CASE("roads: two lanes into one lane straight on merge, the turns use the outer and inner lanes") {
    const Cooked c = cook_roads(doc(R"("nodes": [{"name": "a", "at": [0, 0, 100]}, {"name": "c", "at": [0, 0, 0]},
          {"name": "n", "at": [0, 0, -100]}, {"name": "e", "at": [100, 0, 0]}, {"name": "w", "at": [-100, 0, 0]}],
        "roads": [{"name": "in", "from": "a", "to": "c", "lanes": [2, 2]},
                  {"name": "out", "from": "c", "to": "n", "lanes": [1, 1]},
                  {"name": "east", "from": "c", "to": "e", "lanes": 1},
                  {"name": "west", "from": "c", "to": "w", "lanes": 1}])"));
    INFO(c.diags);
    REQUIRE(c.ok);
    const RoadView& v = c.view;
    const RoadRoad& in = v.roads[road_index(v, "in")];
    const RoadLane& inner = v.lanes[in.first_lane];
    const RoadLane& outer = v.lanes[in.first_lane + 1];
    const u32 out = road_index(v, "out"), east = road_index(v, "east"), west = road_index(v, "west");
    const auto turns = [&](const RoadLane& l, u32 road) {
        for (u32 k = 0; k < l.next_count; ++k)
            if (v.lanes[v.lanes[l.first_next + k].first_next].road == road) return true;
        return false;
    };
    CHECK(turns(inner, out));
    CHECK(turns(outer, out));
    CHECK(turns(inner, west));  // across traffic: the inner lane
    CHECK_FALSE(turns(outer, west));
    CHECK(turns(outer, east));  // towards the kerb: the outer lane
    CHECK_FALSE(turns(inner, east));
    // The two straight connectors merge into one lane, so they conflict.
    const RoadLane* a = nullptr;
    const RoadLane* b = nullptr;
    for (u32 k = 0; k < inner.next_count; ++k)
        if (v.lanes[v.lanes[inner.first_next + k].first_next].road == out) a = &v.lanes[inner.first_next + k];
    for (u32 k = 0; k < outer.next_count; ++k)
        if (v.lanes[v.lanes[outer.first_next + k].first_next].road == out) b = &v.lanes[outer.first_next + k];
    REQUIRE(a);
    REQUIRE(b);
    CHECK(conflicts_with(v, *a, *b));
}

TEST_CASE("roads: authoring mistakes are cook errors naming the file") {
    const char* two = R"("nodes": [{"name": "a", "at": [0, 0, 0]}, {"name": "b", "at": [100, 0, 0]}], )";
    const std::string bad[] = {
        doc(std::string(two) + R"("roads": [{"name": "r", "from": "a", "to": "x"}])"),                    // unknown node
        doc(std::string(two) + R"("roads": [{"name": "r", "from": "a", "to": "a"}])"),                    // a loop
        doc(std::string(two) + R"("roads": [{"name": "r", "from": "a", "to": "b", "lanes": [0, 0]}])"),   // no lanes
        doc(std::string(two) + R"("roads": [{"name": "r", "from": "a", "to": "b", "lanes": 9}])"),        // too many lanes
        doc(std::string(two) + R"("roads": [{"name": "r", "from": "a", "to": "b", "lanse": 1}])"),        // a typo'd key
        doc(std::string(two) + R"("roads": [{"name": "r", "from": "a", "to": "b"}, {"name": "r", "from": "b", "to": "a"}])"),
        doc(std::string(two) + R"("drive": "middle", "roads": [{"name": "r", "from": "a", "to": "b"}])"),
        // A road shorter than its junctions need.
        doc(R"("nodes": [{"name": "a", "at": [0, 0, 0]}, {"name": "b", "at": [5, 0, 0], "radius": 4}, {"name": "c", "at": [5, 0, 50]},
            {"name": "d", "at": [5, 0, -50]}], "roads": [{"name": "r", "from": "a", "to": "b"}, {"name": "s", "from": "b", "to": "c"},
            {"name": "t", "from": "b", "to": "d"}, {"name": "u", "from": "a", "to": "c"}])"),
        crossroads(R"(, "control": "signal")", ""),  // a signal node without a signal
        crossroads("", R"(, "signals": [{"node": "c", "phases": [{"green": ["east", "west"], "seconds": 20}]}])"),  // north never green
        crossroads("", R"(, "signals": [{"node": "c", "phases": [{"green": ["nowhere"], "seconds": 20}]}])"),
        crossroads(R"(, "control": "stop")", R"(, "signals": [{"node": "c", "phases": [{"green": ["east"], "seconds": 20}]}])"),
    };
    for (const std::string& text : bad) {
        const Cooked c = cook_roads(text);
        INFO(text);
        INFO(c.diags);
        CHECK_FALSE(c.ok);
        CHECK(c.diags.find("net.mcroads") != std::string::npos);
    }
}

TEST_CASE("roads: RoadView rejects payloads that do not hold together") {
    const Cooked c = cook_roads(crossroads("", ""));
    REQUIRE(c.ok);
    CHECK(RoadView::read(c.payload.span()));
    CHECK_FALSE(RoadView::read(c.payload.span().subspan(0, c.payload.size() / 2)));
    // A connector pointing past the lane table.
    Array<byte> broken;
    broken.resize(c.payload.size());
    std::memcpy(broken.data(), c.payload.data(), c.payload.size());
    RoadLane* lanes = const_cast<RoadLane*>(RoadView::read(broken.span()).value().lanes.data());
    const u32 conn = c.view.nodes[0].first_connector;
    lanes[conn].first_next = c.view.header->lane_count + 3;
    CHECK_FALSE(RoadView::read(broken.span()));
    lanes[conn].first_next = c.view.lanes[conn].first_next;
    lanes[conn].prev = conn;  // a connector leaving a connector
    CHECK_FALSE(RoadView::read(broken.span()));
    lanes[conn].prev = c.view.lanes[conn].prev;
    CHECK(RoadView::read(broken.span()));
    // A lane whose distances run backwards.
    RoadPoint* pts = const_cast<RoadPoint*>(RoadView::read(broken.span()).value().points.data());
    pts[c.view.lanes[0].first_point + 1].s = -1.0f;
    CHECK_FALSE(RoadView::read(broken.span()));
}
