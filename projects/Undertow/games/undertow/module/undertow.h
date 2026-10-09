#pragma once
// Undertow gameplay: the install function the module entry hands the host, and the run state the
// systems keep (a World resource).
//
// Systems (deterministic, tick-driven):
//   FixedUpdate_Pre   undertow_setup      first tick: find the drone, count pods, define sequences
//   FixedUpdate_Pre   undertow_movers     kinematic bodies follow their path (crushers, drifting mines)
//   FixedUpdate_Pre   undertow_gates      gate timers; a gate's body is enabled only while closed
//   FixedUpdate_Pre   undertow_currents   forces on the drone and loose pods inside current boxes
//   FixedUpdate_Pre   undertow_drone      thrust, boost, energy, tether, death and respawn
//   FixedUpdate_Post  undertow_impacts    hull damage from velocity jumps, lasers, crushers, mines
//   FixedUpdate_Post  undertow_triggers   pads (checkpoint, delivery), switches, mines (sensors)
//   FixedUpdate_Post  undertow_camera     follow, look-ahead, shake, win zoom, post-effect pulses
//   FixedUpdate_Post  undertow_present    drone visual, thrusters, beacons, gates, flicker, HUD text

#include "components.h"
#include "mce/gameplay.h"

namespace undertow {

using mce::u64;

enum class RunPhase : u32 { Intro, Playing, Won };

struct UndertowState {
    RunPhase phase = RunPhase::Intro;
    u32 setup = 0;
    u32 cores = 0;
    u32 cores_total = 0;
    u32 deaths = 0;
    u32 ticks = 0;          // run time in ticks (counts from the first input, stops at the win)
    u32 won_at = 0;
    u32 banner_ticks = 0;   // how long the current banner text stays
    u32 camera_snapped = 0;
    f32 aberration_pulse = 0.0f;
    f32 flash = 0.0f;
    u32 gate_ticks[8] = {};  // ticks each channel stays open
    mce::Entity drone;
    mce::Entity visual;
    mce::Entity tethered;      // the pod on the rope, or null
    mce::Entity boom_emitter;  // one-shot explosion emitter the game moves around
    mce::Entity spark_emitter;
    mce::physics::JointId tether;
    mce::FixedString<64> banner;
};

// Registers the game's components, systems and the UndertowState resource. The host installs
// physics2d, particles and sequences first; input is optional (an InputState resource is registered
// when the host has none, so the editor's play mode runs the scene with the drone idle).
mce::ResultV install(mce::World& world);

}  // namespace undertow
