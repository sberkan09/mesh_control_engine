#pragma once
// Solstice Reach components: plain reflected data (MCE_COMPONENT). Scenes author them, the cook
// validates them, the editor inspects them and the systems in solstice_reach.cpp run on them.
// Data-only header (core + refl + math + asset refs), so the cook compiles it without the game.
//
// The engine has no 3D kit yet (user guide ch. 8), so the first group below is this game's own
// minimal one: a placed model, a scatter volume, a light. The host (games/solstice_reach/host)
// draws them; headless, nothing does, and the sim does not care.
//
// Layouts are padding-free (World::state_hash hashes component bytes): asset refs (24 bytes) and
// 8-byte fields first, then 4-byte fields, with explicit pad_ fields to a multiple of 8.

#include "mce/asset/handle.h"
#include "mce/ecs/entity.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"
#include "mce/types.h"

namespace solstice_reach {

using mce::f32;
using mce::i32;
using mce::u32;
using mce::math::Color;
using mce::math::vec2;
using mce::math::vec3;
using mce::math::vec4;

// ---- the 3D kit this game carries ------------------------------------------------------------

// A placed model: the host draws `mesh` at the entity's LocalTransform.
struct MeshRef {
    mce::asset::AssetRef<mce::asset::Mesh> mesh;
    vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    f32 cull_radius = 0.0f;  // 0: the renderer's bounds; else a sphere the host culls against first
    u32 cast_shadows = 1;
};
MCE_COMPONENT(MeshRef, mesh, tint, (cull_radius, Help{"0 = mesh bounds"}), cast_shadows)

// A volume filled at Startup with instances of up to three meshes, placed on the terrain by a
// seeded hash (no entities: the instances live in the ScatterCache resource; the host draws them
// in 16 m cells). Positions are deterministic in the seed and the volume.
struct Scatter {
    mce::asset::AssetRef<mce::asset::Mesh> mesh;
    mce::asset::AssetRef<mce::asset::Mesh> mesh_b;
    mce::asset::AssetRef<mce::asset::Mesh> mesh_c;
    vec2 half_extent{10.0f, 10.0f};
    f32 density = 1.0f;         // instances per square metre
    f32 min_scale = 0.8f;
    f32 max_scale = 1.3f;
    f32 max_slope_deg = 35.0f;  // ground steeper than this gets nothing
    f32 min_height = 1.5f;      // ground below this (metres above the sea) gets nothing
    f32 max_height = 60.0f;
    u32 seed = 1;
    u32 align_to_ground = 0;    // 1: tilt instances to the ground normal (rocks); 0: upright
    u32 count = 0;              // runtime: instances placed
    u32 pad_ = 0;
};
MCE_COMPONENT(Scatter, mesh, mesh_b, mesh_c, half_extent, (density, Range{0, 20}), min_scale, max_scale, max_slope_deg,
              min_height, max_height, seed, align_to_ground, (count, ReadOnly, Transient))

enum class LightKind : u32 { Lantern, Firefly, Sconce, Flare, Moonbeam };
MCE_ENUM(LightKind, Lantern, Firefly, Sconce, Flare, Moonbeam)

// A point light (a spot when spot_outer > 0) at the entity's position plus `offset`. Lanterns and
// sconces follow the time of day's lantern gain and flicker; fireflies wander and glow at night.
struct Light3D {
    Color color{1.0f, 0.7f, 0.4f, 1.0f};
    vec3 offset{0.0f, 2.0f, 0.4f};
    f32 intensity = 3.0f;
    vec3 direction{0.0f, -1.0f, 0.0f};
    f32 radius = 7.0f;
    f32 spot_outer = 0.0f;  // degrees; 0 = point light
    f32 spot_inner = 0.0f;
    f32 flicker = 0.25f;
    LightKind kind = LightKind::Lantern;
};
MCE_COMPONENT(Light3D, color, offset, (intensity, Range{0, 50}), direction, (radius, Range{0.1f, 60}), spot_outer, spot_inner,
              (flicker, Range{0, 1}), kind)

// A box collider for the 3D physics world (static). `pitch_deg` tilts it about its local X, for
// ramps; the entity's yaw and position place it.
struct BoxCollider3D {
    vec3 half{1.0f, 1.0f, 1.0f};
    f32 pitch_deg = 0.0f;
    vec3 offset;
    f32 pad_ = 0.0f;
};
MCE_COMPONENT(BoxCollider3D, half, pitch_deg, offset)

enum class ClipKind : u32 { None, Turn, Flutter, Gust, Glide, Walk, Idle };
MCE_ENUM(ClipKind, None, Turn, Flutter, Gust, Glide, Walk, Idle)

// Plays a node clip of the entity's MeshRef mesh.
struct Animated {
    f32 speed = 1.0f;
    f32 phase = 0.0f;  // seconds added to the clip time (desynchronises a row of banners)
    ClipKind clip = ClipKind::None;
    u32 pad_ = 0;
};
MCE_COMPONENT(Animated, speed, phase, clip)

// ---- the game ----------------------------------------------------------------------------------

// One of the seven heliostats. `dish` is the entity carrying the dish model (its rotation is
// written by the game); `next` is the mirror the beam goes to (null: the Vault's oculus).
struct Heliostat {
    mce::Entity dish;
    mce::Entity next;
    vec3 beam_offset{0.0f, 1.9f, 0.0f};  // where the beam leaves, in the base's space
    u32 index = 0;                        // 0..6, the order of the design's table
    f32 aligned = 0.0f;                   // 1 once aligned (saved)
    f32 turn = 0.0f;                      // 0..1 progress of the alignment (saved)
    u32 turning = 0;                      // runtime: the Keeper is holding Interact on the plate
    u32 pad_ = 0;
};
MCE_COMPONENT(Heliostat, dish, next, beam_offset, (index, Range{0, 6}), (aligned, Save, ReadOnly), (turn, Save, ReadOnly),
              (turning, ReadOnly, Transient))

// A collectible crystal.
struct Sunshard {
    u32 taken = 0;  // saved
    u32 pad_ = 0;
};
MCE_COMPONENT(Sunshard, (taken, Save, ReadOnly))

// Where the Keeper stands at the start. The entity's transform is the spawn; the character state
// lives on this entity at run time (Keeper).
struct KeeperSpawn {
    f32 yaw = 0.0f;
    f32 pad_ = 0.0f;
};
MCE_COMPONENT(KeeperSpawn, yaw)

// The Keeper's run-time state (added to the spawn entity at Startup; saved fields restore a run).
struct Keeper {
    vec3 pos;
    f32 yaw = 0.0f;
    vec3 prev_pos;
    f32 prev_yaw = 0.0f;
    f32 vy = 0.0f;
    f32 speed = 0.0f;       // horizontal speed this tick (animation)
    f32 anim_time = 0.0f;
    f32 coyote = 0.0f;
    u32 grounded = 0;
    u32 lantern_on = 1;
    u32 sprinting = 0;
    u32 pad_ = 0;
};
MCE_COMPONENT(Keeper, (pos, Save), (yaw, Save), (prev_pos, Transient), (prev_yaw, Transient), (vy, Transient),
              (speed, Transient, ReadOnly), (anim_time, Transient), (coyote, Transient), (grounded, Transient, ReadOnly),
              (lantern_on, Save), (sprinting, Transient))

// A point on the autopilot's route (visited in `order`); the bot walks to each in turn and aligns
// the mirror it stands next to when `mirror` is 1.
struct Waypoint {
    u32 order = 0;
    u32 mirror = 0;
};
MCE_COMPONENT(Waypoint, order, mirror)

// A gull: circles the entity's position at `radius`, `height` above it.
struct GullPath {
    f32 radius = 12.0f;
    f32 height = 8.0f;
    f32 speed = 0.5f;  // radians per second
    f32 phase = 0.0f;
};
MCE_COMPONENT(GullPath, radius, height, speed, phase)

// Run progress that survives a save (one entity with a SaveKey carries it).
struct Progress {
    f32 hour = 17.5f;
    f32 target_hour = 17.5f;
    u32 mirrors = 0;
    u32 shards = 0;
    u32 finale_done = 0;
    u32 pad_ = 0;
};
MCE_COMPONENT(Progress, (hour, Save), (target_hour, Save), (mirrors, Save), (shards, Save), (finale_done, Save))

}  // namespace solstice_reach
