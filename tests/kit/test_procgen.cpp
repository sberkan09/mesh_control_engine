// M-PROCGEN 1: the grid generators are pure functions of (parameters, seed): same bytes twice, a
// level for every seed (one connected floor region in a wall border, start and exit on it), named
// streams that do not disturb each other, and errors that say what to change.

#include <doctest/doctest.h>

#include <algorithm>
#include <set>
#include <string>

#include "mce/kit/procgen.h"

using namespace mce;
using namespace mce::kit::procgen;

namespace {

using Gen = Result<ProcGrid> (*)(u32, u32, u64);

Result<ProcGrid> rooms(u32 w, u32 h, u64 s) { return generate_rooms(w, h, s); }
Result<ProcGrid> caves(u32 w, u32 h, u64 s) { return generate_caves(w, h, s); }
Result<ProcGrid> walk(u32 w, u32 h, u64 s) { return generate_walk(w, h, s); }
Result<ProcGrid> noise(u32 w, u32 h, u64 s) { return generate_noise(w, h, s); }

const struct {
    const char* name;
    Gen fn;
} kGens[] = {{"rooms", rooms}, {"caves", caves}, {"walk", walk}, {"noise", noise}};

// Checks every promise of the header on one grid: border, single region, floor cells, start/exit.
void check_level(const ProcGrid& g, const char* what, u64 seed) {
    INFO(what << " seed " << seed);
    REQUIRE(g.cells.size() == static_cast<usize>(g.width) * g.height);
    for (u32 x = 0; x < g.width; ++x) {
        CHECK(g.at(static_cast<i32>(x), 0) == kWall);
        CHECK(g.at(static_cast<i32>(x), static_cast<i32>(g.height) - 1) == kWall);
    }
    for (u32 y = 0; y < g.height; ++y) {
        CHECK(g.at(0, static_cast<i32>(y)) == kWall);
        CHECK(g.at(static_cast<i32>(g.width) - 1, static_cast<i32>(y)) == kWall);
    }
    CHECK(g.at(g.start.x, g.start.y) == kFloor);
    CHECK(g.at(g.exit.x, g.exit.y) == kFloor);
    CHECK_FALSE(g.start == g.exit);
    // Flood from the start: every floor cell is reached.
    Array<u8> seen;
    seen.resize(g.cells.size());
    for (usize i = 0; i < seen.size(); ++i) seen[i] = 0;
    Array<u32> stack;
    stack.push_back(static_cast<u32>(g.start.y * static_cast<i32>(g.width) + g.start.x));
    seen[stack[0]] = 1;
    u32 reached = 0;
    bool exit_reached = false;
    while (!stack.empty()) {
        const u32 cur = stack.back();
        stack.pop_back();
        ++reached;
        const i32 cx = static_cast<i32>(cur % g.width), cy = static_cast<i32>(cur / g.width);
        exit_reached = exit_reached || (cx == g.exit.x && cy == g.exit.y);
        const i32 nx[4] = {cx + 1, cx - 1, cx, cx}, ny[4] = {cy, cy, cy + 1, cy - 1};
        for (int d = 0; d < 4; ++d) {
            if (g.at(nx[d], ny[d]) != kFloor) continue;
            const u32 ni = static_cast<u32>(ny[d]) * g.width + static_cast<u32>(nx[d]);
            if (seen[ni]) continue;
            seen[ni] = 1;
            stack.push_back(ni);
        }
    }
    CHECK(reached == g.floor_count());
    CHECK(exit_reached);
}

}  // namespace

TEST_CASE("procgen: the same seed gives the same level, a different seed another one") {
    for (const auto& gen : kGens) {
        INFO(gen.name);
        auto a = gen.fn(48, 32, 1234), b = gen.fn(48, 32, 1234), c = gen.fn(48, 32, 1235);
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        REQUIRE(c.has_value());
        CHECK(a.value().hash() == b.value().hash());
        CHECK(a.value().hash() != c.value().hash());
    }
}

TEST_CASE("procgen: pinned levels (a change here is a change of every saved seed)") {
    // Update these only on purpose: a ProcLevel's seed is saved in scenes, so a generator change
    // changes every level a game shipped.
    CHECK(generate_rooms(48, 32, 7).value().hash() == 0xb3470d360295c67eull);
    CHECK(generate_caves(48, 32, 7).value().hash() == 0x837ab4e11a5a120dull);
    CHECK(generate_walk(48, 32, 7).value().hash() == 0x54bb0d7362da77b8ull);
    CHECK(generate_noise(48, 32, 7).value().hash() == 0xb7559dcd217e917bull);
}

TEST_CASE("procgen: a thousand seeds per generator are all connected levels") {
    for (const auto& gen : kGens)
        for (u64 seed = 0; seed < 1000; ++seed) {
            auto g = gen.fn(48, 32, seed);
            REQUIRE_MESSAGE(g.has_value(), gen.name << " seed " << seed << " failed");
            check_level(g.value(), gen.name, seed);
        }
}

TEST_CASE("procgen: sizes from the smallest to a large grid") {
    for (const auto& gen : kGens)
        for (u32 side : {8u, 9u, 16u, 100u, 256u}) {
            if (gen.fn == caves && side < 16) continue;  // caves need room to smooth: see the header
            auto g = gen.fn(side, side + 3 > kMaxSize ? side : side + 3, 5);
            REQUIRE_MESSAGE(g.has_value(), gen.name << " " << side);
            check_level(g.value(), gen.name, 5);
        }
}

TEST_CASE("procgen: the exit is the farthest cell by steps") {
    auto g = generate_rooms(64, 48, 99).value();
    // Walk the shortest-path distance from the start by a flood and compare with the exit's.
    Array<i32> dist;
    dist.resize(g.cells.size());
    for (usize i = 0; i < dist.size(); ++i) dist[i] = -1;
    Array<u32> q;
    q.push_back(static_cast<u32>(g.start.y * static_cast<i32>(g.width) + g.start.x));
    dist[q[0]] = 0;
    i32 far = 0;
    for (usize h = 0; h < q.size(); ++h) {
        const u32 cur = q[h];
        const i32 cx = static_cast<i32>(cur % g.width), cy = static_cast<i32>(cur / g.width);
        const i32 nx[4] = {cx + 1, cx - 1, cx, cx}, ny[4] = {cy, cy, cy + 1, cy - 1};
        for (int d = 0; d < 4; ++d) {
            if (g.at(nx[d], ny[d]) != kFloor) continue;
            const u32 ni = static_cast<u32>(ny[d]) * g.width + static_cast<u32>(nx[d]);
            if (dist[ni] >= 0) continue;
            dist[ni] = dist[cur] + 1;
            far = dist[ni] > far ? dist[ni] : far;
            q.push_back(ni);
        }
    }
    CHECK(dist[static_cast<usize>(g.exit.y) * g.width + static_cast<usize>(g.exit.x)] == far);
}

TEST_CASE("procgen: parameters shape the level") {
    auto sparse = generate_walk(48, 32, 3, {.coverage_percent = 15}).value();
    auto dense = generate_walk(48, 32, 3, {.coverage_percent = 60}).value();
    CHECK(sparse.floor_count() < dense.floor_count());
    auto thick = generate_rooms(64, 48, 3, {.corridor = 2}).value();
    auto thin = generate_rooms(64, 48, 3, {.corridor = 1}).value();
    CHECK(thick.floor_count() > thin.floor_count());
    auto open = generate_noise(48, 32, 3, {.floor_percent = 70}).value();
    auto closed = generate_noise(48, 32, 3, {.floor_percent = 40, .min_floor_percent = 5}).value();
    CHECK(open.floor_count() > closed.floor_count());
}

TEST_CASE("procgen: named streams are independent and repeatable") {
    Rng a = Rng::child(42, "rooms"), b = Rng::child(42, "rooms"), c = Rng::child(42, "caves"), d = Rng::child(42, "rooms", 1);
    std::set<u64> distinct;
    for (int i = 0; i < 64; ++i) {
        const u64 va = a.next_u64();
        CHECK(va == b.next_u64());
        distinct.insert(va);
        distinct.insert(c.next_u64());
        distinct.insert(d.next_u64());
    }
    CHECK(distinct.size() == 192);  // three streams, no value in common
    Rng r(1);
    for (int i = 0; i < 1000; ++i) {
        CHECK(r.below(7) < 7);
        const i32 v = r.range(-3, 3);
        CHECK((v >= -3 && v <= 3));
    }
    CHECK(r.below(0) == 0);
    CHECK(r.range(5, 2) == 5);
}

TEST_CASE("procgen: ASCII picture of a level") {
    auto g = generate_caves(16, 12, 4).value();
    Array<char> text;
    g.to_ascii(text);
    const std::string s(text.data(), text.size());
    CHECK(s.size() == 16 * 12 + 11);
    CHECK(std::count(s.begin(), s.end(), 'S') == 1);
    CHECK(std::count(s.begin(), s.end(), 'E') == 1);
    CHECK(s[0] == '#');
}

TEST_CASE("procgen: bad parameters are errors that say what to change") {
    CHECK_FALSE(generate_rooms(7, 32, 1).has_value());
    CHECK_FALSE(generate_caves(600, 32, 1).has_value());
    CHECK_FALSE(generate_rooms(48, 32, 1, {.leaf_min = 5}).has_value());
    CHECK_FALSE(generate_rooms(48, 32, 1, {.leaf_min = 10, .room_min = 9}).has_value());
    CHECK_FALSE(generate_rooms(48, 32, 1, {.corridor = 3}).has_value());
    CHECK_FALSE(generate_walk(48, 32, 1, {.coverage_percent = 0}).has_value());
    CHECK_FALSE(generate_noise(48, 32, 1, {.scale = 1}).has_value());
    // An impossible floor target is a failure, not a hang.
    auto r = generate_caves(48, 32, 1, {.fill_percent = 100, .min_floor_percent = 50});
    REQUIRE_FALSE(r.has_value());
    CHECK(std::string(r.error().stage).find("procgen.caves") == 0);
}

// ---- wave function collapse (M-PROCGEN 2) --------------------------------------------------------

namespace {

enum : u32 { kGrass = 10, kSand = 20, kWater = 30 };

// Grass touches grass and sand, sand touches everything, water touches sand and water.
WfcRules shore_rules() {
    WfcRules r;
    const u32 pairs[][2] = {{kGrass, kGrass}, {kGrass, kSand}, {kSand, kSand}, {kSand, kWater}, {kWater, kWater}};
    for (const auto& pr : pairs)
        for (WfcDir d : {WfcDir::East, WfcDir::South}) {
            REQUIRE(r.add_pair(pr[0], d, pr[1]).has_value());
            REQUIRE(r.add_pair(pr[1], d, pr[0]).has_value());
        }
    return r;
}

// Every neighbouring pair in the grid is allowed by the rules.
void check_wfc(const WfcGrid& g, const WfcRules& r) {
    REQUIRE(g.tiles.size() == static_cast<usize>(g.width) * g.height);
    for (u32 y = 0; y < g.height; ++y)
        for (u32 x = 0; x < g.width; ++x) {
            if (x + 1 < g.width) CHECK(r.allows(g.at(x, y), WfcDir::East, g.at(x + 1, y)));
            if (y + 1 < g.height) CHECK(r.allows(g.at(x, y), WfcDir::South, g.at(x, y + 1)));
        }
}

}  // namespace

TEST_CASE("wfc: hand-written rules give a grid whose neighbours are all allowed") {
    const WfcRules r = shore_rules();
    CHECK(r.tile_count() == 3);
    CHECK(r.id_of(0) == kGrass);  // ascending ids
    for (u64 seed = 0; seed < 200; ++seed) {
        auto g = generate_wfc(16, 12, seed, r);
        REQUIRE_MESSAGE(g.has_value(), "seed " << seed);
        check_wfc(g.value(), r);
    }
    // Grass never touches water, which the rules only allow through sand.
    CHECK_FALSE(r.allows(kGrass, WfcDir::East, kWater));
    CHECK_FALSE(r.allows(kWater, WfcDir::North, kGrass));
}

TEST_CASE("wfc: the same seed gives the same grid, another seed another one") {
    const WfcRules r = shore_rules();
    auto a = generate_wfc(20, 20, 5, r), b = generate_wfc(20, 20, 5, r), c = generate_wfc(20, 20, 6, r);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(c.has_value());
    CHECK(a.value().hash() == b.value().hash());
    CHECK(a.value().hash() != c.value().hash());
    CHECK(generate_wfc(20, 20, 5, r).value().hash() == 0xc1b99171fdbb078bull);  // pinned: a change moves every saved seed
}

TEST_CASE("wfc: rules learned from an example, in any tile order") {
    // 0 = grass, 1 = sand, 2 = water laid out as a shore: water never touches grass in the example.
    const u32 example[] = {0, 0, 0, 1, 2, 2,   //
                           0, 0, 1, 2, 2, 2,   //
                           0, 1, 1, 2, 2, 2,   //
                           1, 1, 2, 2, 2, 2};
    auto r = WfcRules::from_example(example, 6, 4);
    REQUIRE(r.has_value());
    CHECK(r.value().tile_count() == 3);
    CHECK_FALSE(r.value().allows(0, WfcDir::East, 2));
    CHECK(r.value().allows(1, WfcDir::East, 2));
    for (u64 seed = 0; seed < 100; ++seed) {
        auto g = generate_wfc(14, 10, seed, r.value());
        REQUIRE_MESSAGE(g.has_value(), "seed " << seed);
        check_wfc(g.value(), r.value());
    }
    // The same example with the ids renamed in a different order learns the same dense order.
    const u32 renamed[] = {7, 7, 7, 5, 9, 9, 7, 7, 5, 9, 9, 9, 7, 5, 5, 9, 9, 9, 5, 5, 9, 9, 9, 9};
    auto r2 = WfcRules::from_example(renamed, 6, 4);
    REQUIRE(r2.has_value());
    CHECK(r2.value().id_of(0) == 5);
    CHECK(r2.value().id_of(2) == 9);
    CHECK_FALSE(r2.value().allows(7, WfcDir::East, 9));
}

TEST_CASE("wfc: weights come from how often the example uses a tile") {
    // 90 grass and 10 sand cells in a row-by-row example: grass should dominate the output.
    Array<u32> example;
    for (u32 i = 0; i < 100; ++i) example.push_back(i % 10 == 9 ? 1 : 0);
    // Rows of 10 with sand in the last column would force columns; shuffle by laying a single row.
    auto r = WfcRules::from_example(std::span<const u32>(example.data(), example.size()), 100, 1);
    REQUIRE(r.has_value());
    u32 grass = 0, sand = 0;
    for (u64 seed = 0; seed < 50; ++seed) {
        auto g = generate_wfc(40, 1, seed, r.value());
        REQUIRE(g.has_value());
        for (u32 x = 0; x < 40; ++x) (g.value().at(x, 0) == 0 ? grass : sand)++;
    }
    CHECK(grass > sand * 3);
    WfcRules w = shore_rules();
    REQUIRE(w.set_weight(kWater, 0).has_value());  // 0 becomes 1
    CHECK(w.weight_of(static_cast<u32>(w.index_of(kWater))) == 1);
}

TEST_CASE("wfc: pinned cells are kept and constrain the rest") {
    const WfcRules r = shore_rules();
    const WfcPin pins[] = {{0, 0, kWater}, {15, 11, kGrass}, {8, 6, kSand}};
    for (u64 seed = 0; seed < 100; ++seed) {
        auto g = generate_wfc(16, 12, seed, r, pins);
        REQUIRE_MESSAGE(g.has_value(), "seed " << seed);
        CHECK(g.value().at(0, 0) == kWater);
        CHECK(g.value().at(15, 11) == kGrass);
        CHECK(g.value().at(8, 6) == kSand);
        check_wfc(g.value(), r);
    }
}

TEST_CASE("wfc: contradictions are errors that name the cause, never a hang") {
    const WfcRules r = shore_rules();
    // Grass and water side by side cannot both be pinned.
    const WfcPin touching[] = {{3, 3, kGrass}, {4, 3, kWater}};
    auto a = generate_wfc(8, 8, 1, r, touching);
    REQUIRE_FALSE(a.has_value());
    CHECK(std::string(a.error().stage).find("contradict") != std::string::npos);
    const WfcPin same[] = {{2, 2, kGrass}, {2, 2, kSand}};
    CHECK_FALSE(generate_wfc(8, 8, 1, r, same).has_value());
    const WfcPin outside[] = {{9, 0, kGrass}};
    CHECK_FALSE(generate_wfc(8, 8, 1, r, outside).has_value());
    const WfcPin unknown[] = {{0, 0, 99}};
    CHECK_FALSE(generate_wfc(8, 8, 1, r, unknown).has_value());
    // A chain 10 -> 20 -> 30 that stops: a row of four cannot be filled, on every attempt.
    WfcRules chain;
    REQUIRE(chain.add_pair(10, WfcDir::East, 20).has_value());
    REQUIRE(chain.add_pair(20, WfcDir::East, 30).has_value());
    auto c = generate_wfc(4, 1, 1, chain, {}, 5);
    REQUIRE_FALSE(c.has_value());
    CHECK(std::string(c.error().stage).find("procgen.wfc: contradiction") == 0);
    CHECK_FALSE(generate_wfc(0, 4, 1, r).has_value());
    CHECK_FALSE(generate_wfc(4, 4, 1, WfcRules{}).has_value());
}

TEST_CASE("wfc: more than 64 tiles is an error") {
    Array<u32> example;
    for (u32 i = 0; i < 65; ++i) example.push_back(i);
    auto r = WfcRules::from_example(std::span<const u32>(example.data(), example.size()), 65, 1);
    REQUIRE_FALSE(r.has_value());
    CHECK(std::string(r.error().stage).find("64") != std::string::npos);
    CHECK_FALSE(WfcRules::from_example(std::span<const u32>(example.data(), 3), 2, 2).has_value());
}
