// Issue #152: grid A* — paths around walls, blocked and unreachable goals, no diagonal corner
// cutting, string-pulling, determinism (paths and 100 PathAgents' positions), grids built from a
// cooked .mctilemap and from static colliders, and agents driving transforms and bodies.

#include <doctest/doctest.h>

#include <cmath>
#include <string>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/ecs/transform.h"
#include "mce/kit/pathfinding.h"
#include "mce/physics/module.h"

using namespace mce;
using namespace cook_test;
using kit::GridCell;
using kit::NavGrid;
using kit::PathAgent;
using kit::Pathfinder;
using kit::PathStatus;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

// Rows of text, top row first ('#' wall, '~' cost 4, anything else open), into a grid with
// row 0 at the bottom.
NavGrid grid_from(std::initializer_list<const char*> rows, f32 cell = 1.0f) {
    const u32 h = static_cast<u32>(rows.size());
    const u32 w = static_cast<u32>(std::string(*rows.begin()).size());
    NavGrid g(w, h, cell);
    u32 y = h;
    for (const char* row : rows) {
        --y;
        for (u32 x = 0; x < w; ++x) {
            if (row[x] == '#') g.set_cost({i32(x), i32(y)}, NavGrid::kBlocked);
            if (row[x] == '~') g.set_cost({i32(x), i32(y)}, 4);
        }
    }
    return g;
}

bool adjacent(GridCell a, GridCell b) {
    const i32 dx = std::abs(a.x - b.x), dy = std::abs(a.y - b.y);
    return (dx | dy) != 0 && dx <= 1 && dy <= 1;
}

// Every step is to a neighbour, onto an open cell, and no diagonal squeezes past a wall corner.
void check_valid(const NavGrid& g, const Array<GridCell>& path) {
    for (usize i = 0; i < path.size(); ++i) {
        CHECK(g.walkable(path[i]));
        if (i == 0) continue;
        const GridCell a = path[i - 1], b = path[i];
        CHECK(adjacent(a, b));
        if (a.x != b.x && a.y != b.y) {
            CHECK(g.walkable({b.x, a.y}));
            CHECK(g.walkable({a.x, b.y}));
        }
    }
}

f32 polyline(const NavGrid& g, const Array<GridCell>& cells) {
    f32 len = 0.0f;
    for (usize i = 1; i < cells.size(); ++i) len += math::length(g.cell_center(cells[i]) - g.cell_center(cells[i - 1]));
    return len;
}

// A 40 x 24 maze: horizontal walls every 4 rows, each with a gap at alternating ends, and a
// pillar field between them.
NavGrid maze() {
    NavGrid g(40, 24, 0.5f, {-10.0f, -6.0f});
    for (i32 row = 3; row < 24; row += 4) {
        const bool gap_right = (row / 4) % 2 == 0;
        for (i32 x = 0; x < 40; ++x)
            if (gap_right ? x < 37 : x > 2) g.set_cost({x, row}, NavGrid::kBlocked);
    }
    for (i32 y = 1; y < 24; y += 4)
        for (i32 x = 5; x < 36; x += 6) g.set_cost({x, y}, NavGrid::kBlocked);
    return g;
}

struct Sim {
    World world;
    u64 ticks = 0;
    Sim() {
        world.register_components<LocalTransform>();
        kit::install_path_agents(world, maze());
    }
    Entity agent(math::vec2 at, math::vec2 target, f32 speed = 3.0f) {
        PathAgent a;
        a.target = target;
        a.speed = speed;
        return world.spawn_with(LocalTransform{{at.x, at.y, 0.0f}, math::quat::identity(), {1, 1, 1}}, a);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, ticks++);
    }
    math::vec2 pos(Entity e) { return world.try_get<LocalTransform>(e)->position.xy(); }
    const PathAgent& agent_state(Entity e) { return *world.try_get<PathAgent>(e); }
};

std::string diag_text(const cook::CookReport& r) {
    std::string s;
    for (const cook::Diagnostic& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += std::string(line.view().data(), line.view().size()) + "\n";
    }
    return s;
}

}  // namespace

TEST_CASE("pathfinding: A* finds the shortest way around a wall") {
    const NavGrid g = grid_from({
        "..........",
        "....#.....",
        "....#.....",
        "....#.....",
        "....#.....",
        "....#.....",
        "....#.....",
        "....#.....",
        "....#.....",
        "....#.....",
    });
    Pathfinder pf;
    Array<GridCell> path;
    REQUIRE(pf.find_cells(g, {1, 1}, {8, 1}, true, path));
    CHECK(path.front() == GridCell{1, 1});
    CHECK(path.back() == GridCell{8, 1});
    check_valid(g, path);
    bool over_top = false;
    for (const GridCell& c : path) over_top |= c.y == 9;
    CHECK(over_top);  // the only way past the wall
    // Up to (3, 9), across the wall's top orthogonally (no corner cutting at (4, 8)), down to (8, 1).
    CHECK(path.size() == 19);

    // 4-neighbour: only orthogonal steps, Manhattan length.
    REQUIRE(pf.find_cells(g, {1, 1}, {8, 1}, false, path));
    check_valid(g, path);
    for (usize i = 1; i < path.size(); ++i) CHECK((path[i].x == path[i - 1].x || path[i].y == path[i - 1].y));
    CHECK(path.size() == 8 + 8 + 7 + 1);

    // Costly cells are avoided when a cheaper way exists.
    const NavGrid mud = grid_from({
        ".....",
        ".~~~.",
        ".~~~.",
        ".~~~.",
        ".....",
    });
    REQUIRE(pf.find_cells(mud, {0, 2}, {4, 2}, true, path));
    for (const GridCell& c : path) CHECK(mud.cost(c) == 1);
}

TEST_CASE("pathfinding: a blocked or walled-in goal returns no path") {
    const NavGrid g = grid_from({
        "......",
        "..###.",
        "..#.#.",
        "..###.",
        "......",
    });
    Pathfinder pf;
    Array<GridCell> path;
    path.push_back({9, 9});
    CHECK_FALSE(pf.find_cells(g, {0, 0}, {2, 2}, true, path));  // the goal is a wall
    CHECK(path.empty());
    CHECK_FALSE(pf.find_cells(g, {0, 0}, {3, 2}, true, path));  // enclosed
    CHECK(path.empty());
    CHECK_FALSE(pf.find_cells(g, {0, 0}, {6, 0}, true, path));  // outside the grid
    CHECK_FALSE(pf.find_cells(g, {-1, 0}, {1, 0}, true, path));
    Array<math::vec2> pts;
    CHECK_FALSE(pf.find_path(g, {0.5f, 0.5f}, {3.5f, 2.5f}, {}, pts));
    CHECK(pts.empty());
    // A start inside a wall is allowed: the agent walks out.
    REQUIRE(pf.find_cells(g, {2, 2}, {0, 0}, true, path));
    CHECK(path.front() == GridCell{2, 2});
    // Same cell: a one-cell path, and find_path goes straight to the point.
    REQUIRE(pf.find_cells(g, {0, 0}, {0, 0}, true, path));
    CHECK(path.size() == 1);
    REQUIRE(pf.find_path(g, {0.2f, 0.2f}, {0.7f, 0.6f}, {}, pts));
    REQUIRE(pts.size() == 1);
    CHECK(pts[0] == math::vec2{0.7f, 0.6f});
}

TEST_CASE("pathfinding: diagonals never cut a wall corner") {
    Pathfinder pf;
    Array<GridCell> path;
    // One wall beside the diagonal: the path goes around it orthogonally.
    const NavGrid one = grid_from({
        "...",
        "...",
        ".#.",
    });
    REQUIRE(pf.find_cells(one, {0, 0}, {2, 1}, true, path));
    check_valid(one, path);
    CHECK(path.size() == 4);  // the first step cannot be the diagonal past (1, 0)
    REQUIRE(pf.find_cells(one, {0, 0}, {1, 1}, true, path));
    CHECK(path.size() == 3);  // not the 2-cell diagonal past the corner
    check_valid(one, path);
    // Two walls pinching the diagonal: no way through at all.
    const NavGrid pinch = grid_from({
        "...",
        "#..",
        ".#.",
    });
    CHECK_FALSE(pf.find_cells(pinch, {0, 0}, {2, 2}, true, path));
    // The smoother respects corners too.
    CHECK_FALSE(Pathfinder::line_of_sight(pinch, {0, 0}, {1, 1}));
    CHECK_FALSE(Pathfinder::line_of_sight(one, {0, 0}, {1, 1}));
    CHECK(Pathfinder::line_of_sight(one, {0, 1}, {2, 2}));
    // A maze: every diagonal step of every path has both side cells open.
    const NavGrid m = maze();
    for (i32 sy = 0; sy < 24; sy += 5)
        for (i32 gx = 0; gx < 40; gx += 7) {
            if (!m.walkable({0, sy}) || !m.walkable({gx, 22})) continue;
            REQUIRE(pf.find_cells(m, {0, sy}, {gx, 22}, true, path));
            check_valid(m, path);
        }
}

TEST_CASE("pathfinding: string pulling shortens the path and keeps line of sight") {
    const NavGrid g = grid_from({
        "....................",
        "....................",
        "....................",
        "........####........",
        "........####........",
        "........####........",
        "....................",
        "....................",
    });
    Pathfinder pf;
    Array<GridCell> raw;
    REQUIRE(pf.find_cells(g, {0, 0}, {19, 7}, false, raw));
    Array<GridCell> smooth = raw;
    Pathfinder::smooth(g, smooth);
    CHECK(smooth.size() < raw.size());
    CHECK(smooth.size() >= 3);  // the block is in the way of the straight line
    CHECK(smooth.front() == raw.front());
    CHECK(smooth.back() == raw.back());
    CHECK(polyline(g, smooth) < polyline(g, raw) - 1.0f);
    for (usize i = 1; i < smooth.size(); ++i) CHECK(Pathfinder::line_of_sight(g, smooth[i - 1], smooth[i]));

    // In the open the path collapses to its two ends.
    const NavGrid open(16, 16);
    REQUIRE(pf.find_cells(open, {1, 1}, {14, 9}, true, raw));
    Pathfinder::smooth(open, raw);
    CHECK(raw.size() == 2);
    Array<math::vec2> pts;
    REQUIRE(pf.find_path(open, {1.2f, 1.3f}, {14.4f, 9.9f}, {}, pts));
    REQUIRE(pts.size() == 1);
    CHECK(pts[0] == math::vec2{14.4f, 9.9f});

    // Smoothing never shortcuts through terrain the search went around.
    const NavGrid mud = grid_from({
        ".........",
        "...~~~...",
        "...~~~...",
        "...~~~...",
        ".........",
    });
    REQUIRE(pf.find_cells(mud, {0, 2}, {8, 2}, true, raw));
    Pathfinder::smooth(mud, raw);
    for (usize i = 1; i < raw.size(); ++i)  // neighbour steps are the search's own; longer ones stay on cost 1
        if (!adjacent(raw[i - 1], raw[i])) CHECK(Pathfinder::line_of_sight(mud, raw[i - 1], raw[i], 1));
    CHECK(raw.size() < 7);
}

TEST_CASE("pathfinding: same inputs, same path; 100 agents hash identically after N ticks") {
    const NavGrid m = maze();
    Pathfinder a, b;
    Array<math::vec2> pa, pb;
    REQUIRE(a.find_path(m, {-9.8f, -5.8f}, {9.6f, 5.1f}, {}, pa));
    // A planner whose scratch is dirty from other queries gives the same answer as a fresh one.
    for (i32 i = 0; i < 5; ++i) (void)b.find_path(m, {-9.0f + f32(i), 5.0f}, {9.0f, -5.0f + f32(i)}, {}, pb);
    REQUIRE(b.find_path(m, {-9.8f, -5.8f}, {9.6f, 5.1f}, {}, pb));
    REQUIRE(pa.size() == pb.size());
    for (usize i = 0; i < pa.size(); ++i) CHECK(pa[i] == pb[i]);
    CHECK(pa.size() > 6);  // the maze zig-zags

    auto run = [](u64& hash, u64& positions, u32& arrived, u32& plans) {
        Sim s;
        Array<Entity> agents;
        for (u32 i = 0; i < 100; ++i) {
            const u32 row = (i / 40) * 4;  // rooms 0, 1 and 2
            const math::vec2 at{-9.75f + 0.5f * f32(i % 40), -5.75f + 0.5f * f32(row)};
            const math::vec2 target{9.75f - 0.5f * f32(i % 7), 5.25f - 1.0f * f32(i % 2)};
            agents.push_back(s.agent(at, target, 6.0f + 0.05f * f32(i % 20)));
        }
        s.run(1500);
        hash = s.world.state_hash();
        Hasher64 h;
        arrived = 0;
        for (Entity e : agents) {
            const math::vec2 p = s.pos(e);
            h.bytes(&p, sizeof(p));
            arrived += s.agent_state(e).status == PathStatus::Arrived ? 1u : 0u;
        }
        positions = h.value();
        plans = s.world.resource<kit::PathScratch>().plans;
    };
    u64 h1 = 0, h2 = 0, p1 = 0, p2 = 0;
    u32 arrived1 = 0, arrived2 = 0, plans1 = 0, plans2 = 0;
    run(h1, p1, arrived1, plans1);
    run(h2, p2, arrived2, plans2);
    CHECK(h1 == h2);
    CHECK(p1 == p2);
    CHECK(arrived1 == arrived2);
    CHECK(plans1 == plans2);
    CHECK(arrived1 == 100);  // 25 s is enough for every agent to cross the maze
}

TEST_CASE("pathfinding: a grid from a cooked .mctilemap") {
    const std::string root = fresh_dir("pathfinding_tilemap");
    // 6 x 4, row 0 at the bottom: a wall of tile 2 at x = 2 (open at the top), a decoration (tile 1) at (4, 0).
    write_file(root + "/assets/level.mctilemap", R"({ "format": "mce-tilemap", "version": 1,
        "guid": "9a270000-0000-4000-8000-000000000152", "name": "level", "tileset": null,
        "columns": 2, "rows": 1, "width": 6, "height": 4, "tile_size": 0.5,
        "tiles": [ 0, 0, 2, 0, 1, 0,
                   0, 0, 2, 0, 0, 0,
                   0, 0, 2, 0, 0, 0,
                   0, 0, 0, 0, 0, 0 ] })");
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    INFO(diag_text(r));
    REQUIRE(r.ok());
    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
    const asset::AssetKey key = assets.find(asset::parse_guid("9a270000-0000-4000-8000-000000000152").value());
    REQUIRE(key != asset::kInvalidAssetKey);
    auto h = assets.load_sync<asset::Tilemap>(key);
    REQUIRE(h);
    auto view = asset::TilemapView::read(assets.resolve(h.value()));
    REQUIRE(view);

    // Default: every non-empty tile is a wall.
    const NavGrid g = kit::nav_grid_from_tilemap(view.value(), {10.0f, 20.0f});
    CHECK(g.width() == 6);
    CHECK(g.height() == 4);
    CHECK(g.cell_size() == doctest::Approx(0.5f));
    CHECK_FALSE(g.walkable({2, 0}));
    CHECK_FALSE(g.walkable({2, 2}));
    CHECK(g.walkable({2, 3}));
    CHECK_FALSE(g.walkable({4, 0}));
    CHECK(g.cell_at({10.0f + 1.1f, 20.0f + 0.3f}) == GridCell{2, 0});
    // Only tile 2 solid: the decoration is floor.
    const u32 solid[] = {2};
    const NavGrid walls_only = kit::nav_grid_from_tilemap(view.value(), {10.0f, 20.0f}, Span<const u32>(solid, 1));
    CHECK_FALSE(walls_only.walkable({2, 1}));
    CHECK(walls_only.walkable({4, 0}));
    assets.release(asset::AnyHandle(h.value()));

    // World path from the left side to the right side goes over the wall's top.
    Pathfinder pf;
    Array<math::vec2> pts;
    REQUIRE(pf.find_path(g, {10.25f, 20.25f}, {12.75f, 20.25f}, {}, pts));
    bool high = false;
    for (const math::vec2& p : pts) high |= p.y > 21.4f;
    CHECK(high);
    CHECK(pts.back() == math::vec2{12.75f, 20.25f});
}

TEST_CASE("pathfinding: static colliders mark the grid") {
    World w;
    physics::install_physics2d(w);
    w.register_components<LocalTransform>();
    physics::RigidBody2D rb;  // static
    physics::Collider2D wall;
    wall.shape = physics::ColliderShape::box({0.5f, 2.0f});  // 1 x 4 metres
    w.spawn_with(LocalTransform{{5.0f, 3.0f, 0.0f}, math::quat::identity(), {1, 1, 1}}, rb, wall);
    physics::Collider2D sensor = wall;
    sensor.is_sensor = true;
    w.spawn_with(LocalTransform{{1.5f, 1.5f, 0.0f}, math::quat::identity(), {1, 1, 1}}, rb, sensor);
    physics::RigidBody2D dyn;
    dyn.type = physics::BodyType::Dynamic;
    physics::Collider2D ball;
    ball.shape = physics::ColliderShape::circle(0.4f);
    w.spawn_with(LocalTransform{{8.5f, 8.5f, 0.0f}, math::quat::identity(), {1, 1, 1}}, dyn, ball);

    NavGrid g(10, 10);
    // The box spans x 4.5..5.5 and y 1..5: columns 4 and 5 overlap it, rows 1..4 (y = 5 only touches).
    CHECK(kit::mark_static_colliders(g, w) == 8);
    CHECK_FALSE(g.walkable({4, 1}));
    CHECK_FALSE(g.walkable({5, 4}));
    CHECK(g.walkable({5, 5}));
    CHECK(g.walkable({1, 1}));  // sensor
    CHECK(g.walkable({8, 8}));  // dynamic
}

TEST_CASE("pathfinding: static_colliders_hash follows exactly what blocks the grid") {
    World w;
    physics::install_physics2d(w);
    w.register_components<LocalTransform>();
    physics::RigidBody2D rb;
    physics::Collider2D wall;
    wall.shape = physics::ColliderShape::box({0.5f, 2.0f});
    const u64 empty = kit::static_colliders_hash(w);
    const Entity a = w.spawn_with(LocalTransform{{5.0f, 3.0f, 0.0f}, math::quat::identity(), {1, 1, 1}}, rb, wall);
    const u64 one = kit::static_colliders_hash(w);
    CHECK(one != empty);
    CHECK(kit::static_colliders_hash(w) == one);  // a pure function of the colliders
    // A sensor, a dynamic body and a one-way platform do not block, so they do not change it.
    physics::Collider2D sensor = wall;
    sensor.is_sensor = true;
    w.spawn_with(LocalTransform{{1.0f, 1.0f, 0.0f}, math::quat::identity(), {1, 1, 1}}, rb, sensor);
    CHECK(kit::static_colliders_hash(w) == one);
    w.try_get_mut<LocalTransform>(a)->position.x = 6.0f;
    CHECK(kit::static_colliders_hash(w) != one);
}

TEST_CASE("pathfinding: PathAgent walks a transform around walls and stops at the target") {
    Sim s;
    // From the bottom-left room to the room above: the path goes through the gap at the right.
    const Entity e = s.agent({-9.75f, -5.75f}, {-9.75f, -3.75f}, 4.0f);
    s.run(1);
    CHECK(s.agent_state(e).status == PathStatus::Moving);
    CHECK(s.agent_state(e).waypoint_count >= 2);
    const NavGrid& g = s.world.resource<NavGrid>();
    u32 ticks = 1;
    while (s.agent_state(e).status == PathStatus::Moving && ticks < 2000) {
        s.run(1);
        ++ticks;
        CHECK(g.walkable(g.cell_at(s.pos(e))));
    }
    CHECK(s.agent_state(e).status == PathStatus::Arrived);
    CHECK(math::length(s.pos(e) - math::vec2{-9.75f, -3.75f}) <= s.agent_state(e).arrival_radius + 1e-4f);
    CHECK(ticks > 60 * 4);  // ~19 m around the wall at 4 m/s

    // A new target re-plans; an unreachable one reports NoPath and the agent stays put.
    s.world.try_get_mut<PathAgent>(e)->target = {9.75f, -3.75f};
    s.run(1);
    CHECK(s.agent_state(e).status == PathStatus::Moving);
    NavGrid& mg = s.world.resource<NavGrid>();
    mg.set_cost(mg.cell_at({9.75f, 5.75f}), NavGrid::kBlocked);
    s.world.try_get_mut<PathAgent>(e)->target = {9.75f, 5.75f};
    s.run(1);
    const math::vec2 stuck = s.pos(e);
    CHECK(s.agent_state(e).status == PathStatus::NoPath);
    s.run(10);
    CHECK(s.pos(e) == stuck);
}

TEST_CASE("pathfinding: PathAgent drives a physics body through its velocity") {
    World w;
    w.register_components<LocalTransform>();
    physics::PhysicsConfig cfg;
    cfg.gravity = {0.0f, 0.0f};
    physics::install_physics2d(w, cfg);
    NavGrid g(12, 6);
    for (i32 y = 0; y < 5; ++y) g.set_cost({6, y}, NavGrid::kBlocked);
    kit::install_path_agents(w, static_cast<NavGrid&&>(g));
    PathAgent a;
    a.target = {10.5f, 0.5f};
    a.speed = 5.0f;
    physics::RigidBody2D rb;
    rb.type = physics::BodyType::Kinematic;
    physics::Collider2D col;
    col.shape = physics::ColliderShape::circle(0.3f);
    const Entity e = w.spawn_with(LocalTransform{{1.5f, 0.5f, 0.0f}, math::quat::identity(), {1, 1, 1}}, a, rb, col);
    u64 t = 0;
    for (; t < 600 && w.try_get<PathAgent>(e)->status != PathStatus::Arrived; ++t) w.tick(kDt, t);
    CHECK(w.try_get<PathAgent>(e)->status == PathStatus::Arrived);
    for (u32 i = 0; i < 10; ++i, ++t) w.tick(kDt, t);  // stopped: no drift after arrival
    const math::vec2 p = w.try_get<LocalTransform>(e)->position.xy();
    CHECK(p.x == doctest::Approx(10.5f).epsilon(0.01));
    CHECK(p.y == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(math::length(w.resource<physics::World2D>().linear_velocity(e)) == doctest::Approx(0.0f));
}
