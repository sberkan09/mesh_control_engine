#pragma once
// Prism Break gameplay: the install function the module entry hands the host, and the run state the
// systems keep (a World resource).
//
// Systems (deterministic, tick-driven):
//   FixedUpdate_Pre   prism_setup    first tick: find the rules and the paddle, arm the bricks
//   FixedUpdate_Pre   prism_paddle   the paddle follows Move (or the ball, on autopilot)
//   FixedUpdate_Pre   prism_balls    serve, sweep, bounce, break bricks, split, lose balls
//   FixedUpdate_Post  prism_present  brick tints, hidden balls, HUD text

#include "components.h"
#include "mce/gameplay.h"

namespace prism_break {

using mce::u64;

enum class RunPhase : u32 { Serve, Play, Won, Lost };

struct PrismState {
    RunPhase phase = RunPhase::Serve;
    u32 setup = 0;
    u32 lives = 3;
    u32 score = 0;
    u32 bricks_left = 0;  // breakable bricks still standing
    u32 bricks_total = 0;
    u32 balls_lost = 0;
    u32 splits = 0;
    u32 serve_ticks = 0;  // ticks in the Serve phase (autopilot launches after a beat)
    u32 restarted = 0;    // 1 on the tick a press restarted the run (that press does not serve)
    u32 bounces = 0;      // paddle bounces this run (the autopilot's aim changes on each)
    u64 ticks = 0;
    u64 ended_at = 0;
    mce::Entity rules;
    mce::Entity paddle;
};

// Registers the game's components, systems and the PrismState resource. The host installs
// physics2d first; input is optional (an InputState resource is registered when the host has
// none, so headless runs and the editor's play mode run with the paddle still).
mce::ResultV install(mce::World& world);

}  // namespace prism_break
