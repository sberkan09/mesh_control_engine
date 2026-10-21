#pragma once
// Glowmoth gameplay: the install function the module entry hands the host, and the run state the
// systems keep (a World resource, kept across scene changes).
//
// Systems (deterministic, tick-driven):
//   FixedUpdate_Pre   glowmoth_setup    per scene: find the moth, count lanterns, reset per-level state
//   FixedUpdate_Pre   glowmoth_screens  title / win / lose: Jump goes on
//   FixedUpdate_Pre   glowmoth_bats     bats fly their paths
//   FixedUpdate_Pre   glowmoth_moth     the platformer controller
//   FixedUpdate_Post  glowmoth_triggers lanterns (sensor TriggerEnter), bats (box overlap), the gate
//   FixedUpdate_Post  glowmoth_present  facing, blink, lantern and gate textures, HUD text

#include "components.h"
#include "mce/gameplay.h"

namespace glowmoth {

using mce::u64;

constexpr u32 kMaxHits = 3;

struct GlowState {
    // Kept for the whole run (across scenes).
    u32 hits = 0;
    u32 levels_done = 0;
    // Per scene (reset when a new scene loads).
    u32 scene_serial = 0xffffffffu;  // SceneChange::changes the per-scene state belongs to
    u32 lanterns = 0;
    u32 lanterns_total = 0;
    u32 gate_open = 0;
    u32 leaving = 0;  // 1 once a scene change was requested
    u32 pad_ = 0;
    u64 ticks = 0;  // ticks in this scene
    mce::Entity moth;
};

mce::ResultV install(mce::World& world);

}  // namespace glowmoth
