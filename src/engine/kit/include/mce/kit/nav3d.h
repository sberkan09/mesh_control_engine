#pragma once
// 3D navigation as components (M-GAMEPLAY3D step 1): a navmesh over the scene's colliders, areas
// with path costs, off-mesh links, carved obstacles, and agents that walk to a target around each
// other, on physics::NavMesh3D (Recast & Detour, ADR-0040). kit::install_stock installs them.
//
//   NavMeshSurface3D  the navmesh: the bake settings, and the baked .mcnav (the editor's or MCP's
//                     navmesh_bake); without one it is baked from the scene at the first tick
//   NavArea3D         a box whose floor takes an area id with a path cost (mud, water, a road);
//                     area 0 cuts a hole
//   NavLink3D         an off-mesh link from the entity to `end` (a jump, a ladder, a drop)
//   NavObstacle3D     a box carved out of the mesh while it exists (a door, a crate); it follows
//                     its entity
//   NavAgent3D        walks to `target` when `move_to` is set, steering around other agents
//
// The bake reads BoxCollider3D, MeshCollider3D and TerrainCollider3D (kit3d.h; a terrain's holes
// stay open) in the collision layers of `layer_mask`, plus the MeshRef meshes of entities without a collider when `include_meshes` is
// set; NavArea3D and NavLink3D are baked in, so changing them needs a re-bake (a runtime bake
// re-runs when `version` changes). NavObstacle3D and the agents are live.
//
// An agent with a CharacterController3D (use_input off) steers it: the agent sets the controller's
// `move` and the controller walks (slopes, steps, pushes). Without one the agent's LocalTransform
// is put on the mesh directly. `face_motion` turns the entity to face where it walks.
//
// Systems: "kit3d_nav" (FixedUpdate_Pre, before "kit3d_bodies": the mesh, obstacles, the crowd
// step and the agents' writes, so a controller's `move` lands this tick).

#include "mce/asset/handle.h"
#include "mce/containers/array.h"
#include "mce/ecs/entity.h"
#include "mce/math/math.h"
#include "mce/physics3d/navmesh3d.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

struct NavMeshSurface3D {
    asset::AssetRef<asset::NavMesh> baked;  // the baked mesh (.mcnav); empty = baked at the first tick
    math::vec3 bounds_half;                 // a box around the entity to bake; zero = everything
    f32 cell_size = 0.2f;
    f32 cell_height = 0.1f;
    f32 agent_height = 1.8f;
    f32 agent_radius = 0.4f;
    f32 agent_max_climb = 0.4f;
    f32 agent_max_slope_deg = 45.0f;
    u32 tile_size = 48;
    u32 max_agents = 128;
    u32 max_obstacles = 128;
    u32 layer_mask = 0xFFFF;  // collider layers the bake reads
    u32 include_meshes = 0;   // 1: MeshRef meshes of entities without a collider are baked too
    u32 version = 0;          // bump to re-bake at runtime (only without `baked`)
    u32 show = 1;             // 1: the editor draws the mesh
    u32 polys = 0;            // engine-written: polygons in the loaded mesh
    u32 ready = 0;            // engine-written: 1 once a mesh is loaded
};
MCE_COMPONENT(NavMeshSurface3D, (baked, Help{"the editor's or navmesh_bake's .mcnav; empty = baked when the scene starts"}),
              (bounds_half, Help{"bake only inside this box around the entity; zero = the whole scene"}),
              (cell_size, Range{0.02f, 2}), (cell_height, Range{0.02f, 2}), (agent_height, Range{0.1f, 20}),
              (agent_radius, Range{0, 10}), (agent_max_climb, Range{0, 10}), (agent_max_slope_deg, Range{0, 89}),
              (tile_size, Range{8, 200}, Help{"cells per tile side; obstacles rebuild whole tiles"}), (max_agents, Range{1, 4096}),
              (max_obstacles, Range{1, 4096}), (layer_mask, Help{"collider layers (bits 0..15) the bake reads"}),
              (include_meshes, Help{"1: also bake MeshRef meshes of entities without a collider"}),
              (version, Help{"bump to re-bake (runtime bake only)"}), (show, Help{"1: the editor draws the mesh"}),
              (polys, ReadOnly, Transient), (ready, ReadOnly, Transient))

// The navmesh floor inside this box (axis-aligned, scaled by the entity) takes `area`; paths pay
// `cost` per metre there. Area 0 is not walkable; 1 is the default ground (cost 1); 2..31 are the
// game's. Two volumes with one area id share the first one's cost.
struct NavArea3D {
    math::vec3 half{1.0f, 1.0f, 1.0f};
    math::vec3 offset;
    u32 area = 2;
    f32 cost = 2.0f;
};
MCE_COMPONENT(NavArea3D, (half, Help{"box half extents, metres (axis-aligned)"}), (offset, Help{"entity space"}),
              (area, Range{0, 31}, Help{"0 = hole, 1 = ground, 2..31 = yours"}), (cost, Range{1, 1000}, Help{"path cost per metre"}))

// An off-mesh link from the entity's position to `end` (entity space): agents use it as part of a
// path. Both ends must be within `radius` of the baked floor.
struct NavLink3D {
    math::vec3 end{0.0f, 0.0f, 2.0f};
    f32 radius = 0.5f;
    u32 bidirectional = 1;
    u32 area = 1;
};
MCE_COMPONENT(NavLink3D, (end, Help{"the far end, entity space"}), (radius, Range{0.05f, 10}), bidirectional, (area, Range{1, 31}))

// A box (scaled by the entity, turned by its yaw) carved out of the navmesh while enabled. Moving
// the entity re-carves it; agents re-plan around it.
struct NavObstacle3D {
    math::vec3 half{0.5f, 1.0f, 0.5f};
    math::vec3 offset;
    u32 enabled = 1;
    u32 pad_ = 0;
};
MCE_COMPONENT(NavObstacle3D, (half, Help{"box half extents, metres"}), (offset, Help{"entity space"}), enabled)

enum class NavAgentState : u8 { Idle, Moving, Arrived, NoPath, OffMesh };
MCE_ENUM(NavAgentState, Idle, Moving, Arrived, NoPath, OffMesh)

// Walks to `target` while `move_to` is set (clear it to stop), following a navmesh path and
// steering around other agents. Changing `target` re-plans. With `follow` set (an entity: the
// player, a leader) the engine writes that entity's position into `target` every tick and re-plans
// once it has moved `follow_repath` metres from the last plan, so an enemy chases without code.
struct NavAgent3D {
    Entity follow;
    f32 follow_repath = 0.5f;  // metres
    math::vec3 target;
    f32 speed = 3.5f;          // m/s
    f32 acceleration = 8.0f;   // m/s^2
    f32 radius = 0.4f;
    f32 height = 1.8f;
    f32 separation = 2.0f;     // how hard agents keep apart (0 = off)
    f32 arrive_distance = 0.2f;
    u32 avoidance = 2;         // 0 cheapest .. 3 best
    u8 move_to = 0;            // 1: walk to target
    u8 avoid_agents = 1;
    u8 face_motion = 1;        // 1: turn the entity to face where it walks
    NavAgentState state = NavAgentState::Idle;  // engine-written
    math::vec3 velocity;       // engine-written
    f32 distance = 0.0f;       // engine-written: straight distance to the target
    math::vec3 next_corner;    // engine-written: the next point on the path
    u32 arrivals = 0;          // engine-written: times the agent arrived
};
MCE_COMPONENT(NavAgent3D, (follow, Help{"an entity to chase: its position becomes the target every tick"}),
              (follow_repath, Range{0.01f, 100}, Help{"metres the followed entity moves before a re-plan"}), target, (speed, Range{0, 100}, Help{"m/s"}), (acceleration, Range{0, 1000}), (radius, Range{0.05f, 10}),
              (height, Range{0.1f, 20}), (separation, Range{0, 20}), (arrive_distance, Range{0.01f, 10}),
              (avoidance, Range{0, 3}, Help{"0 cheapest .. 3 best"}), (move_to, Help{"1: walk to target"}), avoid_agents,
              (face_motion, Help{"1: face the walking direction"}), (state, ReadOnly, Transient), (velocity, ReadOnly, Transient),
              (distance, ReadOnly, Transient), (next_corner, ReadOnly, Transient), (arrivals, ReadOnly, Transient))

// World resource: the loaded navmesh (one per world: the first NavMeshSurface3D in ECS order).
// Games may query it (find_path, raycast, nearest_point) any time after `ready`.
struct Nav3D {
    physics::NavMesh3D mesh;
    Entity surface;  // the surface it was built for
};

// Everything the bake reads for `surface` (its settings, the colliders, areas and links) as bake
// input. The editor and MCP bake through it; so does the runtime bake. `assets` (may be null)
// loads MeshCollider3D and MeshRef meshes.
void gather_nav_geometry(World& world, Entity surface, asset::AssetSystem* assets, physics::NavGeometry3D& out,
                         physics::NavBakeSettings3D& settings);
// gather_nav_geometry + physics::bake_navmesh.
bool bake_scene_navmesh(World& world, Entity surface, asset::AssetSystem* assets, Array<byte>& out,
                        physics::NavBakeStats3D* stats = nullptr);

// Registers the components and adds "kit3d_nav" (after install_bodies3d, which it calls). Idempotent.
void install_nav3d(World& world, asset::AssetSystem* assets);

// Debug-draw geometry of the world's navmesh (when loaded): the floor as triangles with their
// areas, the links as line pairs, and every agent's remaining corners as line pairs.
void nav3d_debug_geometry(World& world, Array<math::vec3>& tris, Array<u8>& areas, Array<math::vec3>& lines);

}  // namespace mce::kit
