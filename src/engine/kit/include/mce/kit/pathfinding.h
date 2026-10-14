#pragma once
// Grid pathfinding (issue #152): a walkability grid, a deterministic A* over it with string-pulling
// smoothing, and the PathAgent runner (the component is in components.h).
//
//   kit::NavGrid grid = kit::nav_grid_from_tilemap(view, map_origin);   // walls from a .mctilemap
//   kit::mark_static_colliders(grid, world, /*inflate=*/0.2f);          // plus static colliders
//   kit::install_path_agents(world, static_cast<kit::NavGrid&&>(grid)); // resource + "path_agents"
//   ... an entity with LocalTransform + PathAgent{target} walks there
//
// The grid: width x height square cells of `cell_size` metres, cell (0, 0) bottom-left with its
// corner at `origin`; each cell holds a traversal cost, 0 = blocked, 1 = normal, up to 255.
//
// The search is integer A*: an orthogonal step into a cell costs 10 x its cost, a diagonal one 14
// x, and the heuristic is octile distance (admissible: costs are at least 1). Diagonal moves never
// cut a corner (both orthogonal neighbours must be walkable). Ties in the open list break on the
// larger g, then the smaller cell index; neighbours expand in a fixed order. With no float in the
// search and no unordered container anywhere, the same grid and endpoints give the same path on
// every machine. The scratch lives in the Pathfinder and is reused, so a query allocates only
// while the scratch grows to the grid's size.
//
// Smoothing pulls the cell path taut: from each kept corner it skips to the farthest later cell
// in grid line of sight. The line of sight is a supercover walk between cell centres (every cell
// the segment touches, both cells at an exact corner crossing) that fails on a blocked cell or on
// one costlier than the most expensive cell of the stretch it replaces, so smoothing never cuts
// a corner nor shortcuts through terrain the search avoided.
//
// The runner ("path_agents", FixedUpdate_Pre so a body's velocity lands in this tick's physics
// step) plans when the target changes, every `repath_interval` ticks while moving or blocked,
// and when a truncated path runs out; then moves the entity `speed x dt` along the waypoints.

#include "mce/asset/formats.h"
#include "mce/ecs/world.h"
#include "mce/hash.h"
#include "mce/kit/components.h"

namespace mce::physics {
class DebugDrawSink;
}

namespace mce::kit {

struct GridCell {
    i32 x = 0;
    i32 y = 0;
    friend bool operator==(GridCell, GridCell) = default;
};

class NavGrid {
public:
    static constexpr u8 kBlocked = 0;

    NavGrid() = default;
    // Every cell starts at `cost` (1 = all open).
    NavGrid(u32 width, u32 height, f32 cell_size = 1.0f, math::vec2 origin = {}, u8 cost = 1);

    u32 width() const { return width_; }
    u32 height() const { return height_; }
    f32 cell_size() const { return cell_size_; }
    math::vec2 origin() const { return origin_; }
    u32 cell_count() const { return width_ * height_; }
    Span<const u8> costs() const { return costs_.span(); }

    bool contains(GridCell c) const { return c.x >= 0 && c.y >= 0 && u32(c.x) < width_ && u32(c.y) < height_; }
    u32 index(GridCell c) const { return u32(c.y) * width_ + u32(c.x); }
    GridCell cell_of_index(u32 i) const { return {i32(i % width_), i32(i / width_)}; }
    // Outside the grid reads as blocked.
    u8 cost(GridCell c) const { return contains(c) ? costs_[index(c)] : kBlocked; }
    bool walkable(GridCell c) const { return cost(c) != kBlocked; }
    void set_cost(GridCell c, u8 cost);
    void fill(u8 cost);

    // The cell under a world point (may be outside the grid), a cell's centre and its bounds.
    GridCell cell_at(math::vec2 p) const;
    math::vec2 cell_center(GridCell c) const;
    math::aabb2 cell_bounds(GridCell c) const;
    // Sets every cell whose area overlaps `box` (touching an edge is not overlapping) to `cost`;
    // returns how many cells that covered.
    u32 mark_aabb(math::aabb2 box, u8 cost = kBlocked);

    // FNV-1a over the dimensions and costs (tests, determinism checks).
    u64 hash(u64 seed = kFnv64Offset) const;

private:
    u32 width_ = 0;
    u32 height_ = 0;
    f32 cell_size_ = 1.0f;
    math::vec2 origin_;
    Array<u8> costs_;
};

// A grid matching a tile layer: one cell per tile, `tile_size` cells, (0, 0) at `origin` (the
// TilemapLayer entity's position). Empty tiles (0) are open. With `solid_tiles` empty every
// non-empty tile is a wall (a layer painted with walls only); otherwise only the listed tile
// values are walls and the other tiles are open floor.
NavGrid nav_grid_from_tilemap(const asset::TilemapView& map, math::vec2 origin = {}, Span<const u32> solid_tiles = {});

// Blocks the cells under every static body's collider (RigidBody2D type Static + Collider2D, at its
// LocalTransform; sensors and one-way platforms are skipped), using the shape's world AABB grown
// by `inflate` metres (an agent radius keeps paths off walls). Reads components, so it works
// before physics has created the bodies. Returns the cells covered.
u32 mark_static_colliders(NavGrid& grid, World& world, f32 inflate = 0.0f);

// The union of the static colliders' world AABBs that mark_static_colliders would block (min > max
// when there are none).
math::aabb2 static_collider_bounds(World& world);

struct PathOptions {
    bool diagonal = true;  // 8-neighbour; false = 4-neighbour
    bool smooth = true;    // string-pull the cell path
};

// The A* planner with its reusable scratch. Not thread-safe; one per thread.
class Pathfinder {
public:
    // Cell path from `start` to `goal`, both included, into `out` (cleared first). False (and
    // `out` empty) when the goal is outside, blocked or unreachable. A blocked start is allowed
    // (an agent standing in a wall walks out of it). The path is never smoothed here.
    bool find_cells(const NavGrid& grid, GridCell start, GridCell goal, bool diagonal, Array<GridCell>& out);

    // World waypoints from `from` to `to` into `out` (cleared first): the corners to walk through
    // after leaving `from`, ending exactly at `to`. Smoothed per `opt`.
    bool find_path(const NavGrid& grid, math::vec2 from, math::vec2 to, const PathOptions& opt, Array<math::vec2>& out);

    // In-place string pulling of a cell path (first and last cells kept).
    static void smooth(const NavGrid& grid, Array<GridCell>& cells);
    // Grid line of sight between two cell centres, failing on blocked cells and on cells whose
    // cost exceeds `max_cost`.
    static bool line_of_sight(const NavGrid& grid, GridCell a, GridCell b, u8 max_cost = 255);

    u32 last_expanded() const { return expanded_; }  // cells closed by the last search

private:
    struct Node {  // 64-bit costs: a 4096 x 4096 grid of cost 255 overflows 32 bits
        u64 f;
        u64 g;
        u32 index;
        u32 pad_;
    };
    void heap_push(Node n);
    Node heap_pop();

    Array<u64> g_;
    Array<u32> parent_;
    Array<u32> open_stamp_;    // == stamp_: g_/parent_ valid this search
    Array<u32> closed_stamp_;  // == stamp_: expanded this search
    Array<Node> heap_;
    Array<GridCell> cells_;
    u32 stamp_ = 0;
    u32 expanded_ = 0;
};

// The runner's resource: the planner and its waypoint scratch.
struct PathScratch {
    Pathfinder finder;
    Array<math::vec2> points;
    u32 plans = 0;  // plans made since install (profiling, tests)
};

// Registers PathAgent, sets the NavGrid (when given) and PathScratch resources and adds
// "path_agents" to FixedUpdate_Pre. Idempotent: a second call only replaces the grid. Without a
// NavGrid resource the agents stay idle; the game sets or replaces it (world.set_resource) any
// time, and changed walls are picked up on the next re-plan.
void install_path_agents(World& world);
void install_path_agents(World& world, NavGrid&& grid);

// The runner itself (exposed for tests and custom pipelines).
void path_agents(Frame& f);

// Debug draw through the physics debug-draw interface: blocked cells crossed out, costly cells
// boxed (`costly` color), the grid outline; and every agent's remaining path.
void debug_draw_grid(const NavGrid& grid, physics::DebugDrawSink& sink, math::Color blocked = {1.0f, 0.3f, 0.3f, 1.0f},
                     math::Color costly = {1.0f, 0.8f, 0.2f, 1.0f});
void debug_draw_agents(World& world, physics::DebugDrawSink& sink, math::Color color = {0.3f, 0.9f, 1.0f, 1.0f});

}  // namespace mce::kit
