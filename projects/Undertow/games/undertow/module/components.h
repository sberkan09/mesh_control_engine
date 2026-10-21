#pragma once
// Undertow components: plain reflected data (MCE_COMPONENT). Scenes author them, the cook
// (undertow_cook links types.cpp) validates them, and the systems in undertow.cpp run on them.
// Keep this header data-only (core + refl + asset refs), so the cook can include it without the
// game's code.
//
// Undertow is a salvage-drone game in a flooded cave: a dynamic physics body with thrust in four
// directions, a rope joint to haul cargo pods, sensors for pads, switches, mines and currents, and
// the kit's lit sprites, particles, post effects and sequences for the look and the juice.
//
// Layouts are padding-free (World::state_hash hashes component bytes): asset refs first (24 B
// each), then vec2 / 4-byte fields, with explicit pad_ fields to keep sizes a multiple of 8.

#include "mce/asset/handle.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"
#include "mce/types.h"

namespace undertow {

using mce::f32;
using mce::i32;
using mce::u32;
using mce::math::vec2;

// ---- the drone ---------------------------------------------------------------------------------

// The drone's physics entity: a Dynamic RigidBody2D (fixed rotation, low gravity scale, damping)
// with a circle Collider2D. Thrust is a force along the move axis; boost (Jump) multiplies it and
// drains energy; Fire throws or drops the tether. Hull damage comes from hard impacts (a velocity
// jump above `impact_speed` in one tick), lasers, crushers and mines.
struct Drone {
    mce::asset::AssetRef<mce::asset::Sfx> bump_sound;
    mce::asset::AssetRef<mce::asset::Sfx> hit_sound;
    mce::asset::AssetRef<mce::asset::Sfx> explode_sound;
    mce::asset::AssetRef<mce::asset::Sfx> tether_on_sound;
    mce::asset::AssetRef<mce::asset::Sfx> tether_off_sound;
    f32 thrust = 16.0f;          // newtons per unit mass, roughly (force on a density-1 circle)
    f32 boost_mult = 1.9f;
    f32 max_speed = 8.0f;
    f32 boost_max_speed = 12.5f;
    f32 energy_max = 100.0f;
    f32 boost_drain = 28.0f;     // energy per second while boosting
    f32 energy_regen = 9.0f;     // energy per second otherwise
    f32 tether_range = 3.2f;     // metres: the nearest loose pod within it gets the rope
    f32 tether_length = 2.3f;    // rope length
    f32 impact_speed = 5.5f;     // velocity change in one tick that costs hull
    f32 bump_speed = 2.2f;       // velocity change that only sparks and thuds
    u32 hull_max = 3;
    u32 respawn_ticks = 80;
    // Runtime state (not authored).
    vec2 prev_vel;
    vec2 spawn;
    f32 energy = 0.0f;
    u32 hull = 0;
    u32 started = 0;
    u32 dead_ticks = 0;
    u32 invuln = 0;
    u32 thrust_bits = 0;  // bit 0 right, 1 left, 2 up, 3 down (what the player is pushing)
    u32 boosting = 0;
    u32 pad_ = 0;
};
MCE_COMPONENT(Drone, bump_sound, hit_sound, explode_sound, tether_on_sound, tether_off_sound, (thrust, Range{0, 100}),
              (boost_mult, Range{1, 5}), (max_speed, Range{1, 40}), (boost_max_speed, Range{1, 60}),
              (energy_max, Range{1, 1000}), (boost_drain, Range{0, 500}), (energy_regen, Range{0, 500}),
              (tether_range, Range{0, 20}), (tether_length, Range{0.5f, 10}), (impact_speed, Range{0, 50}),
              (bump_speed, Range{0, 50}), (hull_max, Range{1, 9}), respawn_ticks, (spawn, ReadOnly, Transient),
              (energy, ReadOnly, Transient), (hull, ReadOnly, Transient), (dead_ticks, ReadOnly, Transient),
              (invuln, ReadOnly, Transient), (thrust_bits, ReadOnly, Transient), (boosting, ReadOnly, Transient))

// The drone's look: a separate entity (Sprite + the headlamp Light2D) the game moves onto the
// physics body every tick, tilting the sprite with sideways thrust. Physics owns the body's
// transform, so the visual cannot live on the body entity.
struct DroneVisual {
    f32 tilt_max = 0.32f;   // radians at full sideways thrust
    f32 tilt_rate = 0.12f;  // fraction of the gap closed per tick
    f32 tilt = 0.0f;        // runtime
    u32 pad_ = 0;
};
MCE_COMPONENT(DroneVisual, (tilt_max, Range{0, 1.5f}), (tilt_rate, Range{0.01f, 1}), (tilt, ReadOnly, Transient))

enum class ThrustDir : u32 { Right, Left, Up, Down };
MCE_ENUM(ThrustDir, Right, Left, Up, Down)

// A thruster plume: a ParticleEmitter entity kept at the drone's position + `offset`; it emits
// while the drone pushes in `dir` (the plume points the other way, so the preset's direction does).
struct Thruster {
    vec2 offset;
    ThrustDir dir = ThrustDir::Right;
    u32 pad_ = 0;
};
MCE_COMPONENT(Thruster, offset, dir)

// ---- cargo and pads ----------------------------------------------------------------------------

enum class CargoState : u32 { Loose, Tethered, Delivered };
MCE_ENUM(CargoState, Loose, Tethered, Delivered)

// A core pod: a Dynamic body (denser than the drone, more damping) the drone hauls on the rope.
// It is delivered when it enters a Dock pad's sensor.
struct Cargo {
    u32 index = 0;  // which beacon lights when it is delivered
    CargoState state = CargoState::Loose;
};
MCE_COMPONENT(Cargo, index, (state, ReadOnly, Transient))

enum class PadKind : u32 { Dock, Relay };
MCE_ENUM(PadKind, Dock, Relay)

// A pad: a static sensor. Touching one with the drone makes it the checkpoint and restores hull and
// energy; a Dock also takes delivery of pods (its sensor mask includes the pod layer).
struct Pad {
    mce::asset::AssetRef<mce::asset::Sfx> sound;
    mce::asset::AssetRef<mce::asset::Sfx> deliver_sound;
    PadKind kind = PadKind::Relay;
    u32 visited = 0;  // runtime
};
MCE_COMPONENT(Pad, sound, deliver_sound, kind, (visited, ReadOnly, Transient))

// A dock beacon (Light2D + Sprite): off until `index` cores are delivered.
struct Beacon {
    f32 on_intensity = 2.2f;
    u32 index = 0;
    u32 lit = 0;  // runtime
    u32 pad_ = 0;
};
MCE_COMPONENT(Beacon, (on_intensity, Range{0, 64}), index, (lit, ReadOnly, Transient))

// ---- hazards and switches ----------------------------------------------------------------------

// A switch: the drone touching its sensor opens every Gate on `channel` for `open_seconds`.
struct Switch {
    mce::asset::AssetRef<mce::asset::Sfx> sound;
    f32 open_seconds = 8.0f;
    u32 channel = 0;
    u32 presses = 0;  // runtime
    u32 pad_ = 0;
};
MCE_COMPONENT(Switch, sound, (open_seconds, Range{0.5f, 120}), (channel, Range{0, 7}), (presses, ReadOnly, Transient))

// A laser gate: a static body that is solid and deadly to touch while closed; a Switch on its
// channel opens it (body disabled, sprite faded) for a while.
struct Gate {
    f32 open_alpha = 0.12f;
    u32 channel = 0;
    u32 closed = 1;  // runtime: 1 while the body is enabled
    u32 pad_ = 0;
};
MCE_COMPONENT(Gate, (open_alpha, Range{0, 1}), (channel, Range{0, 7}), (closed, ReadOnly, Transient))

// Touching it costs hull (crushers: a Mover with a Hazard). Contact-based, so it needs a solid body.
struct Hazard {
    u32 damage = 1;
    u32 pad_ = 0;
};
MCE_COMPONENT(Hazard, (damage, Range{0, 9}))

// A drifting mine: a sensor that explodes on the drone (damage, knockback, a particle burst) and
// is gone.
struct Mine {
    mce::asset::AssetRef<mce::asset::Sfx> sound;
    f32 knockback = 9.0f;
    u32 damage = 2;
};
MCE_COMPONENT(Mine, sound, (knockback, Range{0, 50}), (damage, Range{0, 9}))

// A current: every tick the drone and loose pods inside the box (position ± half) get `force`.
// No physics components needed; the emitters that show it are separate entities.
struct Current {
    vec2 force{6.0f, 0.0f};
    vec2 half{2.0f, 1.0f};
    f32 pod_scale = 1.4f;  // pods are heavier and catch more of the flow
    u32 pad_ = 0;
};
MCE_COMPONENT(Current, force, half, (pod_scale, Range{0, 10}))

// A moving body (a kinematic RigidBody2D): position(t) = origin + extent * sin(tau * t / period +
// phase), where origin is where the scene placed it.
struct Mover {
    vec2 extent{2.0f, 0.0f};
    f32 period = 4.0f;
    f32 phase = 0.0f;
    vec2 origin;  // runtime: captured on the first tick
    u32 started = 0;
    u32 pad_ = 0;
};
MCE_COMPONENT(Mover, extent, (period, Range{0.1f, 60}), phase, (origin, ReadOnly, Transient), (started, ReadOnly, Transient))

// Modulates the entity's Light2D intensity around its authored value (crystals, beacons).
struct Flicker {
    f32 amount = 0.25f;  // fraction of the base intensity
    f32 speed = 3.0f;    // cycles per second
    f32 base = 0.0f;     // runtime: the authored intensity
    u32 seed = 0;
};
MCE_COMPONENT(Flicker, (amount, Range{0, 1}), (speed, Range{0, 30}), (base, ReadOnly, Transient), seed)

// The rope's look: a Sprite stretched from the drone to the tethered pod (pivot at x = 0), hidden
// when nothing is on the rope.
struct TetherVisual {
    f32 thickness = 0.1f;
    u32 pad_ = 0;
};
MCE_COMPONENT(TetherVisual, (thickness, Range{0.01f, 1}))

// A screen-covering Sprite pinned to the camera (Parallax 0): its alpha is the game's flash.
struct ScreenFlash {
    f32 max_alpha = 0.55f;
    u32 pad_ = 0;
};
MCE_COMPONENT(ScreenFlash, (max_alpha, Range{0, 1}))

// ---- camera and HUD ----------------------------------------------------------------------------

// On the camera: follows the drone with a velocity look-ahead, clamped to [min, max], shaken by
// impacts, and zooms in when the run is won.
struct CameraRig {
    vec2 min{-1000.0f, -1000.0f};
    vec2 max{1000.0f, 1000.0f};
    vec2 shake;  // runtime offset
    f32 lookahead = 0.3f;   // seconds of velocity added to the target
    f32 smoothing = 0.1f;   // fraction of the gap closed per tick
    f32 win_zoom = 1.35f;
    f32 shake_strength = 0.0f;  // runtime
};
MCE_COMPONENT(CameraRig, min, max, (lookahead, Range{0, 2}), (smoothing, Range{0.01f, 1}), (win_zoom, Range{0.2f, 4}),
              (shake, ReadOnly, Transient), (shake_strength, ReadOnly, Transient))

// On a TextLabel pinned to the screen (Parallax 0): the game writes the run's state into it.
enum class HudKind : u32 { Status, Banner, Hint, Timer };
MCE_ENUM(HudKind, Status, Banner, Hint, Timer)
struct HudText {
    HudKind kind = HudKind::Status;
    u32 pad_ = 0;
};
MCE_COMPONENT(HudText, kind)

// The run's numbers, mirrored from the game state every tick so tools can read them
// (MCP run_headless `inspect`, the project test).
struct RunStats {
    f32 energy = 0.0f;
    u32 hull = 0;
    u32 cores = 0;
    u32 cores_total = 0;
    u32 deaths = 0;
    u32 won = 0;
    u32 ticks = 0;
    u32 started = 0;  // 1 once the player (or the autopilot) gave input
    u32 tethered = 0;
    u32 gates_open = 0;
};
MCE_COMPONENT(RunStats, (energy, ReadOnly, Transient), (hull, ReadOnly, Transient), (cores, ReadOnly, Transient),
              (cores_total, ReadOnly, Transient), (deaths, ReadOnly, Transient), (won, ReadOnly, Transient),
              (ticks, ReadOnly, Transient), (started, ReadOnly, Transient), (tethered, ReadOnly, Transient),
              (gates_open, ReadOnly, Transient))

// Flies the drone for the attract scene and headless runs: waypoints visited in order; at a
// waypoint whose bit is set in `tether_mask` it presses the tether button. Loops when `loop` is set.
struct Autopilot {
    vec2 w0, w1, w2, w3, w4, w5, w6, w7;
    f32 reach = 0.9f;      // metres to count a waypoint as reached
    f32 cruise = 5.5f;     // target speed
    u32 count = 0;
    u32 tether_mask = 0;
    u32 loop = 0;
    u32 index = 0;         // runtime
    u32 done = 0;          // runtime
    u32 pad_ = 0;
};
MCE_COMPONENT(Autopilot, w0, w1, w2, w3, w4, w5, w6, w7, (reach, Range{0.1f, 10}), (cruise, Range{0.5f, 30}),
              (count, Range{0, 8}), tether_mask, loop, (index, ReadOnly, Transient), (done, ReadOnly, Transient))

}  // namespace undertow
