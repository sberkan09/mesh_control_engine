#pragma once
// Pedestrian crowds on a road network's pavements (M-OPENWORLD B3, ADR-0048). A CrowdSpawner3D keeps up
// to `max_count` Pedestrian3D walking the sidewalks of a RoadNetwork3D (kit/roads.h) in a ring around a
// focus (the player, or the camera), and removes the ones that fall behind. kit::install_stock installs it.
//
//   - CrowdSpawner3D: on any entity. `network` is the entity with the RoadNetwork3D (null: the first
//     one), `focus` the entity the crowd gathers around (null: the first Camera3DComponent; with none
//     the crowd fills the whole network and never despawns). `prefab` is spawned per pedestrian (it
//     needs a LocalTransform and a Pedestrian3D); null spawns a bare entity with just those two.
//   - Pedestrian3D: the walker. It has no behaviour tree: a pedestrian is a few numbers (which pavement
//     edge, how far along, which way), so thousands cost less than one tree. The system writes its
//     LocalTransform (flat on the network's plane, facing the way it walks) and `pace`/`mode`, which an
//     animation graph or timeline can read.
//
// The pavements. Every road with `sidewalk > 0` has one edge beside each kerb, the road's centreline
// offset by the carriageway's half width plus half the sidewalk, trimmed back to the junction. At a
// junction a pedestrian takes a hop from the end of its edge to the end of another edge there: round the
// corner (no road in the way), or across a road. A crossing waits at the kerb until every road it
// crosses has no straight-on traffic with a green or amber light and enough red left to get across
// (unsignalled junctions never make anyone wait; B2 traffic is responsible for giving way there).
//
// Level of detail. A pedestrian within `near_radius` of the focus steps every tick; within `far_radius`
// every 4th tick; beyond it every 16th, with the time that passed, staggered by entity index so the
// cost is level. Spawning picks points on pavements between `spawn_min` and `spawn_max` of the focus
// from a candidate list that depends only on the focus's 32 m cell, so a saved game resumes the same.
//
// Everything is a function of the saved components (Pedestrian3D, CrowdSpawner3D::serial) and the
// network's lights, so a run replays and hashes exactly.

#include "mce/asset/handle.h"
#include "mce/ecs/entity.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
struct Prefab;
}  // namespace mce::asset

namespace mce::kit {

enum class PedestrianMode : u8 { Walk, Wait, Cross, Down };  // Down: its Health3D is dead (M-PEDDEAD)
MCE_ENUM(PedestrianMode, Walk, Wait, Cross, Down)

// How long a pedestrian whose Health3D is dead lies where it fell before it is removed.
inline constexpr f32 kPedestrianCorpseSeconds = 10.0f;

struct Pedestrian3D {
    u32 crowd = 0;                   // the owning CrowdSpawner3D's entity index + 1; 0: none (uses the defaults)
    u32 edge = 0xFFFFFFFFu;          // the pavement edge it walks; none: placed on the nearest pavement
    u32 target = 0xFFFFFFFFu;        // while hopping at a junction: the edge end it heads for
    f32 s = 0.0f;                    // metres along the edge
    f32 hop = 0.0f;                  // 0..1 along the hop
    f32 speed = 1.4f;                // m/s it walks at
    f32 pace = 0.0f;                 // m/s it moves at now (engine-written; 0 while waiting)
    f32 yaw = 0.0f;                  // radians about +Y it faces
    f32 lag = 0.0f;                  // seconds since its last step (engine-written)
    u32 rng = 1;                     // its random stream (which way at the next junction)
    u8 dir = 1;                      // 1: along the edge from its first point, 0: back
    PedestrianMode mode = PedestrianMode::Walk;
    u8 lod = 0;                      // 0 near, 1 mid, 2 far (engine-written)
    u8 pad_ = 0;
};
MCE_COMPONENT(Pedestrian3D, (crowd, Save, ReadOnly), (edge, Save), (target, Save), (s, Save), (hop, Save),
              (speed, Save, Range{0.1f, 10}, Help{"m/s the pedestrian walks at"}), (pace, Save, ReadOnly, Help{"m/s it moves at now; read it to pick an animation"}),
              (yaw, Save), (lag, Save, ReadOnly), (rng, Save), (dir, Save), (mode, Save, ReadOnly, Help{"Walk, Wait (at a kerb for a light), Cross or Down (dead)"}),
              (lod, Save, ReadOnly, Help{"0 near, 1 mid, 2 far from the focus"}))

struct CrowdSpawner3D {
    asset::AssetRef<asset::Prefab> prefab;  // spawned per pedestrian; null: a bare entity
    Entity network;                         // the entity with the RoadNetwork3D; null: the first one
    Entity focus;                           // the crowd gathers around it; null: the first camera
    u16 max_count = 60;                     // pedestrians alive at once
    u16 per_tick = 2;                       // spawn attempts per tick
    f32 spawn_min = 30.0f;                  // m from the focus: nobody pops in closer
    f32 spawn_max = 90.0f;
    f32 despawn_radius = 120.0f;            // farther than this from the focus they are removed
    f32 near_radius = 35.0f;                // within: stepped every tick
    f32 far_radius = 80.0f;                 // beyond: every 16th tick
    f32 speed_min = 1.0f;
    f32 speed_max = 1.7f;
    f32 cross = 0.4f;                       // 0..1: how willing they are to cross a road (against going round)
    u32 seed = 1;
    u32 serial = 0;                         // engine-written: pedestrians spawned so far
    u8 enabled = 1;
    u8 pad_[3] = {};
};
MCE_COMPONENT(CrowdSpawner3D, (prefab, Help{"spawned per pedestrian; it needs a LocalTransform and a Pedestrian3D. Empty: a bare entity"}),
              (network, Help{"the entity with the RoadNetwork3D; empty: the first one"}),
              (focus, Help{"the crowd gathers around this entity; empty: the first camera"}),
              (max_count, Range{0, 5000}, Help{"pedestrians alive at once"}), (per_tick, Range{1, 32}, Help{"spawn attempts per tick"}),
              (spawn_min, Range{0, 1000}, Help{"metres from the focus inside which nobody spawns"}), (spawn_max, Range{1, 2000}),
              (despawn_radius, Range{1, 4000}, Help{"pedestrians farther than this from the focus are removed; keep it above spawn_max"}),
              (near_radius, Range{0, 2000}, Help{"within this they step every tick"}), (far_radius, Range{0, 4000}, Help{"beyond this every 16th tick"}),
              (speed_min, Range{0.1f, 10}), (speed_max, Range{0.1f, 10}),
              (cross, Range{0, 1}, Help{"how willing they are to cross a road instead of going round the corner"}), seed,
              (serial, Save, ReadOnly, Help{"pedestrians spawned so far"}), (enabled, Help{"0: no new pedestrians"}))

// What the crowd system did last tick (a resource; not part of the state hash). Budget counters.
struct CrowdStats {
    u32 pedestrians = 0;
    u32 near = 0, mid = 0, far = 0;  // by level of detail
    u32 waiting = 0, crossing = 0;
    u32 down = 0;                    // dead pedestrians lying about (not counted in `pedestrians`)
    u32 stepped = 0;                 // pedestrians stepped this tick
    u32 spawned = 0, despawned = 0;
};
const CrowdStats* crowd_stats(World& world);

// Registers Pedestrian3D and CrowdSpawner3D and adds "kit_crowd" (FixedUpdate_Pre, after "roads_clock").
// Idempotent (re-points the assets).
void install_crowd(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
