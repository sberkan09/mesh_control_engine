#pragma once
// Solstice Reach components: plain reflected data (MCE_COMPONENT). Scenes author them, the cook
// validates them, the editor inspects them and the systems in solstice_reach.cpp run on them.
// Data-only header (core + refl + math + asset refs), so the cook compiles it without the game.
//
// The game is built on the engine's 3D kit: scenes place kit3d MeshRef / PointLight3D / Animator /
// BoxCollider3D / Camera3DComponent entities, and the stock player draws them. The components
// below are the game's own; the systems in present.cpp turn its state into the kit's.
//
// Layouts are padding-free (World::state_hash hashes component bytes): asset refs (24 bytes) and
// 8-byte fields first, then 4-byte fields, with explicit pad_ fields to a multiple of 8.

#include "mce/asset/handle.h"
#include "mce/ecs/entity.h"
#include "mce/kit/kit3d.h"
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

// ---- on the engine's 3D kit (mce/kit/kit3d.h) -------------------------------------------------

// Placed models, lights, clips and static colliders are the kit's: MeshRef, PointLight3D,
// Animator and BoxCollider3D. The stock player draws them; what follows is what this game adds.
using mce::kit::Animator;
using mce::kit::BoxCollider3D;
using mce::kit::Camera3DComponent;
using mce::kit::MeshRef;
using mce::kit::PointLight3D;

// A volume filled at Startup with copies of up to three meshes, placed on the terrain by a
// seeded hash into the kit's StaticInstances3D resource (no entities; drawn in 16 m cells).
// Positions are deterministic in the seed and the volume.
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

enum class GlowKind : u32 { Lantern, Firefly, Sconce, Steady };
MCE_ENUM(GlowKind, Lantern, Firefly, Sconce, Steady)

// Drives the entity's PointLight3D from the time of day: lanterns and sconces follow the hour's
// lantern gain and flicker; fireflies glow at night and wander around where they were placed
// (the entity moves, so a MeshRef on it is the firefly's body).
struct Glow {
    f32 intensity = 3.0f;  // the light's intensity at full gain
    f32 flicker = 0.25f;
    GlowKind kind = GlowKind::Lantern;
    u32 placed = 0;        // runtime: `home` is set
    vec3 home;             // runtime: where a firefly wanders around
    u32 pad_ = 0;
};
MCE_COMPONENT(Glow, (intensity, Range{0, 50}), (flicker, Range{0, 1}), kind, (placed, ReadOnly, Transient),
              (home, ReadOnly, Transient))

// The Keeper's body: the entity carrying the Keeper's MeshRef, Animator (walk / idle) and the
// lantern's PointLight3D. sr_present puts it on the character every tick.
struct KeeperModel {
    f32 lift = 0.95f;  // metres from the character's centre down to the model's feet
    u32 pad_ = 0;
};
MCE_COMPONENT(KeeperModel, lift)

// A light beam (its MeshRef stretched from a dish to the next): `mirror` 0..6 is the beam that
// leaves heliostat `mirror` once aligned, 7 the finale's shaft from the Vault's oculus. The
// entity's PointLight3D is the dish's glow.
struct Beam {
    u32 mirror = 0;
    u32 pad_ = 0;
};
MCE_COMPONENT(Beam, (mirror, Range{0, 7}))

enum class HudKind : u32 { Status, Journal, Prompt, Banner };
MCE_ENUM(HudKind, Status, Journal, Prompt, Banner)

// A line of the HUD: sr_hud writes the entity's TextLabel (text, position under the screen
// camera, visibility). `line` picks a row of a multi-line kind (the journal's two, the title's three).
struct HudText {
    HudKind kind = HudKind::Status;
    u32 line = 0;
};
MCE_COMPONENT(HudText, kind, line)

// The game's sounds (one entity carries them): a shard taken, a mirror aligned, the finale.
struct ReachSounds {
    mce::asset::AssetRef<mce::asset::Sfx> chime;
    mce::asset::AssetRef<mce::asset::Sfx> flare;
    mce::asset::AssetRef<mce::asset::Sfx> organ;
};
MCE_COMPONENT(ReachSounds, chime, flare, organ)

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

// A gull: circles where the entity was placed at `radius`, `height` above it.
// The entity moves (carry the gull's MeshRef and Animator on it); `center` is where it was placed.
struct GullPath {
    f32 radius = 12.0f;
    f32 height = 8.0f;
    f32 speed = 0.5f;  // radians per second
    f32 phase = 0.0f;
    vec3 center;       // runtime: the authored position
    u32 placed = 0;    // runtime: `center` is set
};
MCE_COMPONENT(GullPath, radius, height, speed, phase, (center, ReadOnly, Transient), (placed, ReadOnly, Transient))

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
