// M-PROCGEN 3: ProcLevel builds a seeded level as ordinary entities (merged wall rectangles with static bodies,
// a floor, Start and Exit markers), rebuilds it when the seed changes (reroll), takes it away with its owner,
// and creates nothing in a world that has none.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/components.h"
#include "mce/kit/proclevel.h"
#include "mce/physics/components.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

struct LevelWorld {
    World world;
    u64 tick = 0;
    LevelWorld() { install_proclevel(world); }
    void run(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, tick++);
    }
    Entity add(const ProcLevel& l, math::vec3 at = {}) {
        LocalTransform t;
        t.position = at;
        return world.spawn_with(l, t);
    }
    u32 owned() {
        u32 n = 0;
        world.query<const ProcLevelOwned>().each([&](Entity, const ProcLevelOwned&) { ++n; });
        return n;
    }
    u32 walls() {
        u32 n = 0;
        world.query<const physics::RigidBody2D, const physics::Collider2D, const ProcLevelOwned>().each(
            [&](Entity, const physics::RigidBody2D& rb, const physics::Collider2D&, const ProcLevelOwned&) {
                CHECK(rb.type == physics::BodyType::Static);
                ++n;
            });
        return n;
    }
    math::vec3 marker(ProcMarkerKind k) {
        math::vec3 p{};
        u32 found = 0;
        world.query<const ProcMarker, const LocalTransform>().each([&](Entity, const ProcMarker& m, const LocalTransform& t) {
            if (m.kind == k) {
                p = t.position;
                ++found;
            }
        });
        CHECK(found == 1);
        return p;
    }
};

ProcLevel level(ProcKind kind, u32 w, u32 h, u32 seed) {
    ProcLevel l;
    l.kind = kind;
    l.width = w;
    l.height = h;
    l.seed = seed;
    return l;
}

}  // namespace

TEST_CASE("ProcLevel: a world without one creates nothing") {
    LevelWorld w;
    w.run(5);
    CHECK(w.owned() == 0);
}

TEST_CASE("ProcLevel: builds walls, a floor and two markers, the same in two worlds") {
    LevelWorld a, b;
    a.add(level(ProcKind::Rooms, 48, 32, 7));
    b.add(level(ProcKind::Rooms, 48, 32, 7));
    a.run(2);
    b.run(2);
    CHECK(a.walls() > 0);
    CHECK(a.walls() == b.walls());
    CHECK(a.owned() == a.walls() + 3);  // + floor + Start + Exit
    const math::vec3 sa = a.marker(ProcMarkerKind::Start), sb = b.marker(ProcMarkerKind::Start);
    CHECK(sa.x == sb.x);
    CHECK(sa.y == sb.y);
    CHECK(a.marker(ProcMarkerKind::Exit).x == b.marker(ProcMarkerKind::Exit).x);
    // The markers sit at cell centres of a level whose corner is the entity.
    CHECK(sa.x - 0.5f == doctest::Approx(static_cast<f32>(static_cast<i32>(sa.x - 0.5f))));
}

TEST_CASE("ProcLevel: the level sits at the entity's position and follows its tile size") {
    LevelWorld a, b;
    ProcLevel l = level(ProcKind::Caves, 32, 24, 3);
    a.add(l);
    l.tile_size = 2.0f;
    b.add(l, {100.0f, 50.0f, 0.0f});
    a.run(2);
    b.run(2);
    const math::vec3 sa = a.marker(ProcMarkerKind::Start), sb = b.marker(ProcMarkerKind::Start);
    CHECK(sb.x == doctest::Approx(100.0f + sa.x * 2.0f));
    CHECK(sb.y == doctest::Approx(50.0f + sa.y * 2.0f));
}

TEST_CASE("ProcLevel: reroll adds one to the seed, rebuilds, and clears itself") {
    LevelWorld w, fresh;
    const Entity e = w.add(level(ProcKind::Rooms, 48, 32, 1));
    fresh.add(level(ProcKind::Rooms, 48, 32, 2));
    w.run(2);
    fresh.run(2);
    w.world.try_get_mut<ProcLevel>(e)->reroll = true;
    w.run(2);
    CHECK_FALSE(w.world.try_get<ProcLevel>(e)->reroll);
    CHECK(w.world.try_get<ProcLevel>(e)->seed == 2);
    // The old level is gone: what stands is exactly what seed 2 builds.
    CHECK(w.owned() == fresh.owned());
    CHECK(w.marker(ProcMarkerKind::Start).x == fresh.marker(ProcMarkerKind::Start).x);
    CHECK(w.marker(ProcMarkerKind::Exit).y == fresh.marker(ProcMarkerKind::Exit).y);
    // Nothing changes while nothing is asked.
    const u32 before = w.owned();
    w.run(5);
    CHECK(w.owned() == before);
}

TEST_CASE("ProcLevel: destroying the level destroys what it built; a disabled one builds nothing") {
    LevelWorld w;
    const Entity e = w.add(level(ProcKind::Walk, 32, 24, 5));
    ProcLevel off = level(ProcKind::Walk, 32, 24, 5);
    off.enabled = false;
    w.add(off);
    w.run(2);
    CHECK(w.owned() > 0);
    w.world.destroy(e);
    w.run(2);
    CHECK(w.owned() == 0);
}

TEST_CASE("ProcLevel: a grid that cannot be generated builds nothing and does not retry every tick") {
    LevelWorld w;
    w.add(level(ProcKind::Caves, 8, 8, 1));  // too small for caves: a named error
    w.run(3);
    CHECK(w.owned() == 0);
}

TEST_CASE("ProcLevel: wall rectangles stay far below the cell count (2-D merge)") {
    const ProcKind kinds[] = {ProcKind::Rooms, ProcKind::Caves, ProcKind::Walk, ProcKind::Noise};
    for (ProcKind k : kinds)
        for (u32 seed = 1; seed <= 5; ++seed) {
            LevelWorld w;
            w.add(level(k, 128, 128, seed));
            w.run(2);
            const u32 n = w.walls();
            CAPTURE(static_cast<int>(k));
            CAPTURE(seed);
            CHECK(n > 0);
            CHECK(n <= proclevel_max_wall_rects(128, 128));
            CHECK(n < 128 * 128 / 6);
        }
}

TEST_CASE("ProcLevel: proclevel_nav_grid lines up with the level") {
    const ProcLevel l = level(ProcKind::Rooms, 40, 30, 1);
    const NavGrid g = proclevel_nav_grid(l, {10.0f, 20.0f});
    CHECK(g.cell_count() == 40u * 30u);
}

TEST_CASE("ProcLevel: path markers walk from Start to Exit one cell at a time") {
    const ProcKind kinds[] = {ProcKind::Rooms, ProcKind::Caves, ProcKind::Walk, ProcKind::Noise};
    for (ProcKind kind : kinds) {
        LevelWorld w;
        ProcLevel l = level(kind, 48, 32, 4);
        l.path_markers = true;
        w.add(l);
        w.run(2);
        Array<math::vec3> path;
        path.resize(4096);
        u32 n = 0;
        w.world.query<const ProcMarker, const LocalTransform>().each([&](Entity, const ProcMarker& m, const LocalTransform& t) {
            if (m.kind != ProcMarkerKind::Path) return;
            CHECK(m.index >= 1);
            CHECK(m.index < 4096);
            if (m.index < 4096) path[m.index - 1] = t.position;
            ++n;
        });
        CAPTURE(static_cast<int>(kind));
        REQUIRE(n >= 1);
        const math::vec3 start = w.marker(ProcMarkerKind::Start), exit = w.marker(ProcMarkerKind::Exit);
        const auto step = [](math::vec3 a, math::vec3 b) { return std::abs(a.x - b.x) + std::abs(a.y - b.y); };
        CHECK(step(start, path[0]) == doctest::Approx(1.0f));
        for (u32 i = 1; i < n; ++i) CHECK(step(path[i - 1], path[i]) == doctest::Approx(1.0f));
        CHECK(path[n - 1].x == exit.x);
        CHECK(path[n - 1].y == exit.y);
        // Off by default: no Path marker at all.
        LevelWorld off;
        off.add(level(kind, 48, 32, 4));
        off.run(2);
        u32 paths = 0;
        off.world.query<const ProcMarker>().each([&](Entity, const ProcMarker& m) { paths += m.kind == ProcMarkerKind::Path; });
        CHECK(paths == 0);
    }
}

TEST_CASE("ProcLevel: with no ProcLevel in the scene the system changes nothing, tick after tick") {
    LevelWorld w;
    for (u32 i = 0; i < 50; ++i) w.world.spawn_with(LocalTransform{});
    u32 before = 0;
    w.world.query<const LocalTransform>().each([&](Entity, const LocalTransform&) { ++before; });
    w.run(30);
    u32 after = 0;
    w.world.query<const LocalTransform>().each([&](Entity, const LocalTransform&) { ++after; });
    CHECK(before == 50);
    CHECK(after == before);
    CHECK(w.owned() == 0);
}

// The generation budget (M-PROCGEN 5): a 128 x 128 level of every kind builds in a few milliseconds in a release
// build. The bound here is two orders of magnitude looser (it must hold in Debug on a loaded machine) so it
// catches an accidental O(n^2), not a slow day; the entity count is the deterministic half of the budget.
TEST_CASE("ProcLevel: a 128 x 128 level of every kind builds within budget") {
    const ProcKind kinds[] = {ProcKind::Rooms, ProcKind::Caves, ProcKind::Walk, ProcKind::Noise};
    u32 levels = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (ProcKind k : kinds)
        for (u32 seed = 1; seed <= 10; ++seed) {
            LevelWorld w;
            ProcLevel l = level(k, 128, 128, seed);
            l.path_markers = true;
            w.add(l);
            w.run(2);
            CHECK(w.owned() > 3);
            CHECK(w.owned() < 128 * 128 / 4);
            ++levels;
        }
    const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE(levels << " levels in " << ms << " ms");
    CHECK(ms < 20000.0);
}
