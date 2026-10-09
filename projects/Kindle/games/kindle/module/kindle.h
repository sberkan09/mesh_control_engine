#pragma once
// Kindle gameplay: the install function the module entry hands the host, and the run state the
// systems keep (a World resource).
//
// Systems (deterministic, tick-driven):
//   FixedUpdate_Pre   kindle_setup     first tick: find the player, count coins, pick the start colour
//   FixedUpdate_Pre   kindle_movers    moving platforms follow their path (kinematic velocity)
//   FixedUpdate_Pre   kindle_crumble   crumbling blocks shake, give way and grow back
//   FixedUpdate_Pre   kindle_blocks    switch blocks of the active colour are solid, the rest ghosts
//   FixedUpdate_Pre   kindle_player    the platformer controller
//   FixedUpdate_Post  kindle_triggers  coins, hazards, switches and the goal (sensor TriggerEnter)
//   FixedUpdate_Post  kindle_camera    the camera follows the player
//   FixedUpdate_Post  kindle_present   player frames, switch tint, HUD text

#include "components.h"
#include "mce/gameplay.h"

namespace kindle {

using mce::u64;

struct KindleState {
    BlockColor active = BlockColor::Red;
    u32 setup = 0;     // 1 once kindle_setup ran
    u32 toggles = 0;   // colour switches hit this run
    u32 coins = 0;
    u32 coins_total = 0;
    u32 deaths = 0;
    u32 won = 0;
    u32 camera_snapped = 0;
    u64 ticks = 0;     // run time (stops at the flag)
    u64 won_at = 0;
    mce::Entity player;
};

// Registers the game's components, systems and the KindleState resource. The host installs
// physics2d first; input is optional (an InputState resource is registered when the host has
// none, so the editor's play mode runs the level with the player standing still).
mce::ResultV install(mce::World& world);

}  // namespace kindle
