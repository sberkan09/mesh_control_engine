#pragma once
// NavMesh3D — 3D navigation over embedded Recast & Detour (ADR-0040; ROADMAP M-GAMEPLAY3D step 1).
//
//  - bake_navmesh: walkable surfaces voxelized from triangles (colliders, meshes) into a tiled
//    navmesh, with area volumes (per-area path costs, blocked boxes) and off-mesh links (jumps,
//    ladders, drops). The bake is a byte blob (a .mcnav file, or kept in memory): same input and
//    settings, same bytes, on one platform and binary (ADR-0007).
//  - NavMesh3D: loads a bake; nearest point, path and surface-raycast queries; box obstacles carved
//    out of the mesh at runtime (the tiles under them are rebuilt from the bake); a crowd of agents
//    following paths with local avoidance (Detour's crowd).
//
// Nothing in this header names a Recast or Detour type (the Recast wall in check_walls.py).
//
// Areas: every navmesh polygon carries an area id in [0, kNavAreaCount). 0 is not walkable (a
// volume with area 0 cuts a hole), 1 is the default ground, 2.. are the game's own (water, mud,
// road) with a path cost per area (set_area_cost). The cost multiplies the distance walked.
//
// Determinism: single-threaded, every loop in a fixed order, Recast built without FP contraction.
// A given bake, the same obstacle and agent calls, and the same dt give the same agent states.

#include <optional>

#include "mce/containers/array.h"
#include "mce/math/math.h"
#include "mce/memory/allocator.h"

namespace mce::physics {

using math::vec3;

inline constexpr u32 kNavAreaCount = 32;
inline constexpr u8 kNavAreaBlocked = 0;
inline constexpr u8 kNavAreaGround = 1;

// How the bake voxelizes the scene and what the agents it is baked for can do. Lengths in metres.
struct NavBakeSettings3D {
    f32 cell_size = 0.2f;            // xz voxel size; smaller = more accurate, slower, bigger
    f32 cell_height = 0.1f;          // y voxel size
    f32 agent_height = 1.8f;         // clearance an agent needs under a ceiling
    f32 agent_radius = 0.4f;         // the mesh keeps this far from walls
    f32 agent_max_climb = 0.4f;      // steps up to this height are walkable
    f32 agent_max_slope_deg = 45.0f; // steeper surfaces are not
    f32 region_min_size = 8.0f;      // cells: islands smaller than this squared are dropped
    f32 region_merge_size = 20.0f;   // cells: regions smaller than this squared merge into neighbours
    f32 edge_max_len = 12.0f;        // longest polygon edge along a wall (0 = unlimited)
    f32 edge_max_error = 1.3f;       // cells: how far a simplified wall edge may stray
    f32 detail_sample_dist = 6.0f;   // cells: height detail sample spacing (< 0.9 = none)
    f32 detail_sample_max_error = 1.0f;  // cell heights
    u32 tile_size = 48;              // cells per tile side (8..200); obstacles rebuild whole tiles
    u32 max_obstacles = 128;         // live obstacles at once
    math::aabb3 bounds{{1.0f, 1.0f, 1.0f}, {-1.0f, -1.0f, -1.0f}};  // min > max: the geometry's bounds
};

// An axis-aligned box whose walkable surface takes `area` (0 = blocked).
struct NavAreaVolume3D {
    vec3 center;
    vec3 half{0.5f, 0.5f, 0.5f};
    u8 area = 2;
};

// An off-mesh link: agents may cross from `start` to `end` (and back when bidirectional) though
// the mesh does not connect them. Both ends must lie within `radius` of the baked surface.
struct NavLinkDesc3D {
    vec3 start;
    vec3 end;
    f32 radius = 0.5f;
    u8 area = kNavAreaGround;
    bool bidirectional = true;
    u32 user_id = 0;  // the game's id for the link (e.g. its entity index)
};

// Bake input: world-space triangles with an area per triangle, plus the volumes and links.
// Triangles of area 0 are solid but never walkable (an obstacle's sides and top); others are
// walkable where flat enough, with their area.
struct NavGeometry3D {
    Array<vec3> vertices;
    Array<u32> indices;  // 3 per triangle, counter-clockwise seen from the walkable side (+Y)
    Array<u8> areas;     // one per triangle
    Array<NavAreaVolume3D> volumes;
    Array<NavLinkDesc3D> links;

    // `vertices` transformed by `transform`, every triangle with `area`. Out-of-range indices drop
    // the triangle.
    void add_mesh(Span<const vec3> verts, Span<const u32> tris, const math::mat4& transform, u8 area = kNavAreaGround);
    // The box of `half` extents under `transform` (12 triangles).
    void add_box(const math::mat4& transform, vec3 half, u8 area = kNavAreaGround);
    u32 triangle_count() const { return u32(indices.size() / 3); }
    // Bounds of the vertices (min > max when empty).
    math::aabb3 bounds() const;
};

struct NavBakeStats3D {
    u32 tiles_x = 0;
    u32 tiles_z = 0;
    u32 layers = 0;     // tile layers stored (a tile has one per storey)
    u32 triangles = 0;  // input triangles
    usize bytes = 0;    // the blob's size
};

// Bakes `geometry` into `out` (cleared first). False (with a log line) on bad settings, empty
// geometry, or a world too large for the tile grid (> 4096 tiles: raise tile_size or cell_size).
bool bake_navmesh(const NavGeometry3D& geometry, const NavBakeSettings3D& settings, Array<byte>& out,
                  NavBakeStats3D* stats = nullptr);

// Checks a blob's header and sizes without loading it (the cook's validation).
bool navmesh_blob_valid(Span<const byte> blob);

enum class NavPath3D : u8 {
    None,      // start or goal not near the mesh, or nothing reachable
    Complete,  // the path reaches the goal
    Partial,   // the goal is unreachable: the path ends at the closest point that is reachable
};

enum class NavAgentState3D : u8 {
    Idle,     // no target
    Moving,   // following a path
    Arrived,  // within arrive_distance of the target; the agent stopped
    NoPath,   // the target is unreachable (the agent walks to the closest reachable point)
    OffMesh,  // the agent is not on the mesh (added or moved off it); it does not move
};

// One crowd agent's movement rules.
struct NavAgentParams3D {
    f32 radius = 0.4f;
    f32 height = 1.8f;
    f32 max_speed = 3.5f;         // m/s
    f32 max_acceleration = 8.0f;  // m/s^2
    f32 separation = 2.0f;        // how hard agents keep apart (0 = off)
    f32 arrive_distance = 0.2f;   // xz distance to the target that counts as arrived (or within
                                  // 2 x radius once the agent has all but stopped: a crowd at the goal)
    u8 avoidance_quality = 2;     // 0 (cheapest) .. 3 (best) velocity sampling for avoidance
    bool avoid_agents = true;     // steer around other agents
    bool anticipate_turns = true; // round corners instead of stopping at them
    bool optimize_path = true;    // re-plan the path locally as the agent moves (costs respected)
};

struct NavAgentStatus3D {
    vec3 position;
    vec3 velocity;
    vec3 target;
    vec3 next_corner;  // the next point the agent walks to (its position when idle)
    f32 distance = 0.0f;  // straight distance to the target
    NavAgentState3D state = NavAgentState3D::Idle;
    bool partial = false;  // the path to the target ends short of it
};

class NavMesh3D {
public:
    NavMesh3D();
    ~NavMesh3D();
    NavMesh3D(NavMesh3D&&) noexcept;
    NavMesh3D& operator=(NavMesh3D&&) noexcept;
    NavMesh3D(const NavMesh3D&) = delete;
    NavMesh3D& operator=(const NavMesh3D&) = delete;

    // Loads a bake, replacing what was loaded (agents and obstacles are dropped). `max_agents`
    // sizes the crowd. False on a malformed blob (then nothing is loaded).
    bool load(Span<const byte> blob, u32 max_agents = 128);
    void clear();
    bool loaded() const;
    const NavBakeSettings3D& settings() const;
    math::aabb3 bounds() const;

    // ---- path costs (used by queries and agents) -----------------------------------------------------
    void set_area_cost(u8 area, f32 cost);  // >= 1 keeps paths optimal; area 0 ignored
    f32 area_cost(u8 area) const;

    // ---- queries -----------------------------------------------------------------------------------------
    // The closest point on the mesh within `extents` (half box) of `p`.
    std::optional<vec3> nearest_point(vec3 p, vec3 extents = {2.0f, 4.0f, 2.0f}) const;
    // The area of the polygon under `p` (nearest within `extents`); 0 when none.
    u8 area_at(vec3 p, vec3 extents = {2.0f, 4.0f, 2.0f}) const;
    // Corner points from `from` to `to` into `out` (cleared first): the start point snapped to the
    // mesh first, the goal (or the closest reachable point) last; off-mesh link ends included.
    NavPath3D find_path(vec3 from, vec3 to, Array<vec3>& out) const;
    // Walks the surface from `from` toward `to` (xz); the point where it meets a wall, or nullopt
    // when the way is clear.
    std::optional<vec3> raycast(vec3 from, vec3 to) const;

    // ---- obstacles: carved out of the mesh by the next update() -------------------------------------
    // A box of `half` extents at `center`, turned by `yaw` radians about +Y. 0 when the obstacle
    // limit is reached or nothing is loaded.
    u32 add_obstacle(vec3 center, vec3 half, f32 yaw = 0.0f);
    void remove_obstacle(u32 id);
    u32 obstacle_count() const;

    // ---- crowd ---------------------------------------------------------------------------------------------
    // An agent at the mesh point nearest `position`. -1 when the crowd is full or nothing is loaded.
    // An agent added off the mesh is OffMesh until teleported onto it.
    i32 add_agent(vec3 position, const NavAgentParams3D& params = {});
    void remove_agent(i32 agent);
    void set_agent_params(i32 agent, const NavAgentParams3D& params);
    // Plans a path to the mesh point nearest `target`; false when none is near (state NoPath).
    bool set_agent_target(i32 agent, vec3 target);
    void stop_agent(i32 agent);
    // Puts the agent at `position` (snapped to the mesh), keeping its target. Small moves slide the
    // agent's path along (a character controller moved it); large ones re-plan.
    void move_agent(i32 agent, vec3 position);
    bool agent_status(i32 agent, NavAgentStatus3D& out) const;
    // The agent's remaining corners (up to the few the crowd steers by) into `out`.
    void agent_corners(i32 agent, Array<vec3>& out) const;
    u32 agent_count() const;  // live agents

    // Rebuilds tiles under added/removed obstacles, then steps the crowd by dt.
    void update(f32 dt);

    // FNV-1a over the agents (position, velocity, state) and obstacle count.
    u64 hash_state(u64 seed) const;

    // The walkable surface as triangles (3 vertices each, lifted `lift` metres) with the area of
    // each into `areas`; off-mesh links as line pairs into `links` (when given). For debug draw.
    void debug_triangles(Array<vec3>& verts, Array<u8>& areas, f32 lift = 0.02f, Array<vec3>* links = nullptr) const;

    struct Stats {
        u32 tiles = 0;
        u32 polys = 0;
        u32 links = 0;
        u32 obstacles = 0;
        u32 agents = 0;
    };
    Stats stats() const;

    struct Impl;

private:
    mem::UniquePtr<Impl> impl_;
};

}  // namespace mce::physics
