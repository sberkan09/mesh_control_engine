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
