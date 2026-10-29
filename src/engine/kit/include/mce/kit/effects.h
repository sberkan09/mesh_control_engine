#pragma once
// Events and effects as components (M-DATA-REACH 4): what a scene authored in the editor or over
// MCP plays, shakes, follows and reacts to without game code. kit::install_stock installs them, so
// mce_player, play-in-editor and MCP run_headless run the same set.
//
//   SfxTrigger      plays a cooked Sfx once (an audio::PlaySfx event) on a FireOn condition
//   ParticleBurst   ParticleSystem::burst on the entity's ParticleEmitter on a FireOn condition
//   ShakeTrigger    adds trauma to every CameraShake2D (and kit/cinematic.h CameraShake3D) on a FireOn condition
//   CameraShake2D   trauma screen shake on a Camera2DComponent entity (render::shake_offset)
//   CameraFollow2D  a 2D camera following an entity: offset, dead zone, smoothing, look-ahead, bounds
//   CameraFollow3D  a 3D camera orbiting an entity: distance, yaw/pitch, smoothing, look-input orbit,
//                   a spring arm, look-ahead, first person and lock-on (M-MOVE3D 2)
//   PhysicsEvents   the entity's last trigger (2D and 3D) and 2D contact pairs and counts (engine-written)
//   NavGridSource   the NavGrid PathAgents walk on: from a tilemap layer and/or static colliders
//
// FireOn: Manual fires when `fire` is set (by data, a sequence's field track or MCP
// entity_set_component; the engine clears it), Start on the first tick the entity is seen,
// TriggerEnter / ContactBegin when a TriggerEnter (2D), TriggerEnter3D (bodies3d.h) or ContactBegin2D
// event names the entity (as the sensor or trigger, or the other body; as either contact body), Landed when
// a Landed3D names it (a CharacterController3D touching down), Skid when a Skid3D names it (a Vehicle3D's
// wheel starting to skid, vehicles3d.h), Lap when a LapCompleted3D names it (a VehicleDriver3D finishing a
// lap). `fired` counts the plays.
//
// Systems ("kit_effects" in FixedUpdate_Post, after physics so this tick's events are read;
// "kit_camera_follow" in FixedUpdate_Post after it; "kit_nav_grid" in FixedUpdate_Pre before
// "path_agents"). Sounds go out as PlaySfx events, which audio_bridge plays on the next tick.
// Everything is a function of sim state: jitter draws from a hash of (entity, fired), shake noise
// from (seed, time), so replays and determinism hashes match.

#include "mce/asset/handle.h"
#include "mce/audio/components.h"
#include "mce/ecs/entity.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

enum class FireOn : u8 { Manual, Start, TriggerEnter, ContactBegin, Landed, Skid, Lap };
MCE_ENUM(FireOn, Manual, Start, TriggerEnter, ContactBegin, Landed, Skid, Lap)

// A one-shot sound. 2D (centred, `pan`); the mixer has no resampling, so there is no pitch.
struct SfxTrigger {
    asset::AssetRef<asset::Sfx> clip;
    f32 gain_db = 0.0f;
    f32 gain_jitter_db = 0.0f;  // each play adds a value in [-jitter, +jitter] dB
    f32 pan = 0.0f;
    audio::AudioBus bus = audio::AudioBus::SFX;
    FireOn on = FireOn::Manual;
    bool fire = false;
    u8 pad_ = 0;
    u32 fired = 0;  // engine-written: plays so far
    u32 pad2_ = 0;
};
MCE_COMPONENT(SfxTrigger, clip, (gain_db, Range{-60, 12}, Help{"gain in dB"}),
              (gain_jitter_db, Range{0, 24}, Help{"random +/- dB per play"}), (pan, Range{-1, 1}), bus,
              (on, Help{"Manual: when 'fire' is set; Start: first tick; TriggerEnter / ContactBegin: physics events naming this entity (TriggerEnter: 2D, Trigger3D and sensors)"}),
              (fire, Help{"set to play once (the engine clears it)"}), (fired, ReadOnly, Transient))

// A burst of particles from the ParticleEmitter on the same entity.
struct ParticleBurst {
    u32 count = 20;
    FireOn on = FireOn::Manual;
    bool fire = false;
    u8 pad_[2] = {};
    u32 fired = 0;
};
MCE_COMPONENT(ParticleBurst, (count, Range{1, 4096}, Help{"particles per burst"}),
              (on, Help{"Manual: when 'fire' is set; Start: first tick; TriggerEnter / ContactBegin: physics events naming this entity (TriggerEnter: 2D, Trigger3D and sensors)"}),
              (fire, Help{"set to burst once (the engine clears it)"}), (fired, ReadOnly, Transient))

// Adds trauma to every CameraShake2D and CameraShake3D (kit/cinematic.h).
struct ShakeTrigger {
    f32 trauma = 0.5f;
    FireOn on = FireOn::Manual;
    bool fire = false;
    u8 pad_[2] = {};
    u32 fired = 0;
};
MCE_COMPONENT(ShakeTrigger, (trauma, Range{0, 1}, Help{"trauma added (0..1; the shake grows with its square)"}),
              (on, Help{"Manual: when 'fire' is set; Start: first tick; TriggerEnter / ContactBegin: physics events naming this entity (TriggerEnter: 2D, Trigger3D and sensors)"}),
              (fire, Help{"set to shake once (the engine clears it)"}), (fired, ReadOnly, Transient))

// Trauma screen shake (render::ScreenShake's model) on a Camera2DComponent entity: kit::primary_camera
// adds `offset` and `offset_rotation` to the view. Set `trauma` directly or through ShakeTrigger.
struct CameraShake2D {
    f32 trauma = 0.0f;              // 0..1, decays by decay_per_second
    f32 max_translation = 0.5f;     // metres at trauma 1
    f32 max_rotation = 0.05f;       // radians at trauma 1
    f32 decay_per_second = 1.5f;
    f32 frequency_hz = 15.0f;
    u32 seed = 0x5EED;
    f32 time = 0.0f;                // engine-written
    math::vec2 offset;              // engine-written: this tick's view offset
    f32 offset_rotation = 0.0f;     // engine-written
};
MCE_COMPONENT(CameraShake2D, (trauma, Range{0, 1}), (max_translation, Range{0, 10}, Help{"metres at trauma 1"}),
              (max_rotation, Range{0, 1}, Help{"radians at trauma 1"}), (decay_per_second, Range{0, 20}),
              (frequency_hz, Range{0.1f, 60}), seed, (time, ReadOnly, Transient), (offset, ReadOnly, Transient),
              (offset_rotation, ReadOnly, Transient))

// Moves this entity (a 2D camera) after `target`, in the XY plane.
struct CameraFollow2D {
    Entity target;
    math::vec2 offset;              // from the target to the view centre
    math::vec2 dead_zone;           // half-size: the target moves this far before the camera does
    f32 smoothing = 8.0f;           // 1/s; 0 = snap to the target every tick
    f32 look_ahead = 0.0f;          // seconds of the target's velocity to lead by
    math::vec2 bounds_min;          // with use_bounds: the view centre stays inside
    math::vec2 bounds_max;
    bool use_bounds = false;
    bool primed = false;            // engine-written
    u8 pad_[2] = {};
    math::vec2 last_target;         // engine-written: the target's position last tick
};
MCE_COMPONENT(CameraFollow2D, (target, Help{"the entity to follow"}), offset,
              (dead_zone, Help{"half-size in metres the target moves before the camera follows"}),
              (smoothing, Range{0, 60}, Help{"1/s; 0 = snap"}), (look_ahead, Range{0, 2}, Help{"seconds of target velocity to lead by"}),
              bounds_min, bounds_max, use_bounds, (primed, ReadOnly, Transient), (last_target, ReadOnly, Transient))

// Places this entity (a 3D camera) on an orbit around `target`: the pivot is the target's position
// plus `offset`; the camera sits `distance` behind it at `yaw` / `pitch` and looks at it. With
// orbit_speed > 0 the Look input (InputState.look) turns the orbit.
// M-MOVE3D 2 (each off by default): `collide` makes the boom a spring arm that a sphere cast pulls in
// front of static and kinematic geometry and that grows back at `return_speed`; `look_ahead` leads the
// pivot by that many seconds of the target's ground velocity; `first_person` puts the camera at the
// pivot, looking along yaw / pitch; `lock_on` turns the yaw (at `lock_on_rate`) so the camera frames
// the target in front of that entity.
// Chase (M-OPENWORLD-POLICE 4, off by default): `chase` 1 eases the yaw behind the target's ground velocity while the target has a
// Vehicle3D driving forward faster than `chase_min_speed` (2: any target moving that fast), at `chase_rate` per second; the Look
// input holds the yaw where the player put it for `chase_hold` seconds after it was last used. `lock_on` wins over chase.
struct CameraFollow3D {
    Entity target;
    math::vec3 offset{0.0f, 1.5f, 0.0f};
    f32 distance = 6.0f;
    f32 yaw = 0.0f;                 // radians about +Y; 0 = behind the target along +Z
    f32 pitch = 0.35f;              // radians above the horizon
    f32 min_pitch = -0.2f;
    f32 max_pitch = 1.2f;
    f32 smoothing = 10.0f;          // 1/s for the pivot; 0 = snap
    f32 orbit_speed = 0.0f;         // radians per second at full Look input; 0 = no input
    math::vec3 pivot;               // engine-written: the smoothed pivot
    bool primed = false;            // engine-written
    bool collide = false;           // spring arm: keep the camera in front of walls
    bool first_person = false;      // the camera at the pivot instead of behind it
    u8 chase = 0;                   // 1: turn behind a driving Vehicle3D target; 2: behind any moving target
    f32 collision_radius = 0.2f;    // m: the sphere the arm sweeps
    f32 return_speed = 4.0f;        // m/s the arm grows back once a wall clears; 0 = at once
    f32 look_ahead = 0.0f;          // s of the target's ground velocity to lead the pivot by
    f32 lock_on_rate = 0.0f;        // rad/s the yaw turns towards the lock-on framing; 0 = at once
    Entity lock_on;                 // frame the target with this entity beyond it; null = free
    f32 arm = 0.0f;                 // engine-written: the boom length this tick
    math::vec3 last_goal;           // engine-written: the unsmoothed pivot last tick (for look_ahead)
    f32 chase_rate = 2.5f;          // 1/s the yaw eases behind the heading
    f32 chase_hold = 1.5f;          // s the Look input holds the yaw
    f32 chase_min_speed = 3.0f;     // m/s the target must move to be chased
    f32 chase_idle = 0.0f;          // engine-written: s since the Look input was last used
};
MCE_COMPONENT(CameraFollow3D, (target, Help{"the entity to orbit"}), (offset, Help{"pivot above the target"}),
              (distance, Range{0.1f, 1000}), (yaw, Help{"radians about +Y"}), (pitch, Help{"radians above the horizon"}),
              min_pitch, max_pitch, (smoothing, Range{0, 60}, Help{"1/s; 0 = snap"}),
              (orbit_speed, Range{0, 20}, Help{"radians/s at full Look input; 0 = fixed"}), (pivot, ReadOnly, Transient, WorldPoint),
              (primed, ReadOnly, Transient), (collide, Help{"spring arm: walls pull the camera in, it eases back out"}),
              (first_person, Help{"the camera at the pivot, looking along yaw and pitch"}),
              (collision_radius, Range{0.01f, 5}), (return_speed, Range{0, 100}, Help{"m/s the arm grows back; 0 = at once"}),
              (look_ahead, Range{0, 2}, Help{"seconds of target ground velocity to lead the pivot by"}),
              (lock_on_rate, Range{0, 50}, Help{"rad/s the yaw turns to the lock-on framing; 0 = at once"}),
              (lock_on, Help{"keep this entity framed beyond the target; empty = free orbit"}), (arm, ReadOnly, Transient),
              (last_goal, ReadOnly, Transient, WorldPoint),
              (chase, Range{0, 2}, Help{"0 off; 1 turns behind a Vehicle3D target driving forward; 2 behind any moving target"}),
              (chase_rate, Range{0, 60}, Help{"1/s the yaw eases behind the heading"}),
              (chase_hold, Range{0, 60}, Help{"s the Look input holds the yaw after its last use"}),
              (chase_min_speed, Range{0, 100}, Help{"m/s the target must move to be chased"}), (chase_idle, ReadOnly, Transient))

// Engine-written record of the 2D physics events naming this entity (add the component to
// see them; readable by data, sequences and run_headless inspect).
struct PhysicsEvents {
    Entity last_trigger;            // the other entity of the last TriggerEnter / TriggerExit
    Entity last_contact;            // the other body of the last ContactBegin2D
    math::vec2 contact_point;
    math::vec2 contact_normal;      // from this entity towards the other
    f32 contact_speed = 0.0f;       // approach speed of the last contact, m/s
    u32 trigger_enters = 0;
    u32 trigger_exits = 0;
    u32 contact_begins = 0;
    u32 contact_ends = 0;
    u16 overlapping = 0;            // trigger overlaps now
    u16 touching = 0;               // contacts now
};
MCE_COMPONENT(PhysicsEvents, (last_trigger, ReadOnly, Transient), (last_contact, ReadOnly, Transient),
              (contact_point, ReadOnly, Transient), (contact_normal, ReadOnly, Transient),
              (contact_speed, ReadOnly, Transient), (trigger_enters, ReadOnly, Transient),
              (trigger_exits, ReadOnly, Transient), (contact_begins, ReadOnly, Transient),
              (contact_ends, ReadOnly, Transient), (overlapping, ReadOnly, Transient), (touching, ReadOnly, Transient))

// The NavGrid resource PathAgents walk on (the first NavGridSource in ECS order). With `tilemap`
// set: one cell per tile of that TilemapLayer entity's map (non-empty tiles are walls), at its
// position. Otherwise a width x height grid of cell_size cells with its bottom-left corner at
// `origin`. With `colliders`, static 2D colliders (grown by `inflate`) block cells too. Built on
// the first tick, and again whenever `rebuild` is set. A scene with PathAgents and no
// NavGridSource gets a grid from its static colliders covering the agents, their targets and the
// colliders (half-metre cells, 4 m margin).
struct NavGridSource {
    Entity tilemap;
    math::vec2 origin;
    u32 width = 64;
    u32 height = 64;
    f32 cell_size = 0.5f;
    f32 inflate = 0.25f;            // agent radius, metres
    bool colliders = true;
    bool rebuild = false;
    u8 pad_[2] = {};
    u32 blocked = 0;                // engine-written: blocked cells in the built grid
};
MCE_COMPONENT(NavGridSource, (tilemap, Help{"a TilemapLayer entity: one cell per tile, non-empty tiles block"}),
              (origin, Help{"bottom-left corner when there is no tilemap"}), (width, Range{1, 4096}),
              (height, Range{1, 4096}), (cell_size, Range{0.05f, 100}), (inflate, Range{0, 10}, Help{"collider growth (agent radius)"}),
              (colliders, Help{"static 2D colliders block cells"}), (rebuild, Help{"set to rebuild the grid (the engine clears it)"}),
              (blocked, ReadOnly, Transient))

// Registers the components and adds the systems above; also installs the path agents. Idempotent.
// `assets` (may be null) loads NavGridSource tilemaps.
void install_effects(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
