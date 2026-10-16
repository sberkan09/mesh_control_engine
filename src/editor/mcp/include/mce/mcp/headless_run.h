#pragma once
// run_headless (editor-mcp.md §5): the player's headless path, in process. A fresh AssetSystem
// mounts the project's cooked tree and the scene plays exactly as `mce_player -headless 1` plays it
// (input, physics2d, the audio bridge on a Silent device, particles, sequences, then the game's
// install() when a project server registered one with editor::set_play_game_install, scene::load,
// the app loop's stepping clock), so the same scene + ticks give the same World::state_hash as the
// player, or as the project's host with its game module (tests/mcp checks this). With `input` (an
// input::InputScript, the tool's `input` argument) every tick's InputState is published before
// World::tick, as the player publishes the keyboard's: the game's systems see presses and holds.

#include "mce/app.h"
#include "mce/asset/guid.h"
#include "mce/ecs/world.h"

namespace mce::mcp {

// A scene script that failed during the run (M-SCRIPT 4): it was disabled at that tick.
struct ScriptFailure {
    FixedString<64> name;
    FixedString<256> error;  // "<file>:<line>: message"
};

struct HeadlessRun {
    u64 ticks = 0;
    usize entities = 0;  // spawned by the scene
    u64 state_hash = 0;
    WorldMetrics metrics;
    Array<ScriptFailure> script_errors;  // the scene's scripts that failed, in load order
};

// Called every `kProgressTicks` ticks and after the last one.
using TickProgressFn = void (*)(u64 done, u64 total, void* user);
inline constexpr u64 kProgressTicks = 60;

// Called once after the last tick, while the world is still alive: `spawned[i]` is the run's
// entity for scene entity i (file order, prefab instances flattened).
using InspectFn = void (*)(const World& world, Span<const Entity> spawned, void* user);

// Per-run hooks (test_run, run_headless's trace, the bots of agent_sim.h): `start` once after the
// scene loaded (the world and the spawned entities a bot steers), `tick` after every tick; it
// returns false to end the run there (a test's `until` condition).
struct RunHooks {
    void (*start)(World& world, Span<const Entity> spawned, void* user) = nullptr;
    bool (*tick)(const World& world, Span<const Entity> spawned, u64 tick, void* user) = nullptr;
    void* user = nullptr;
};

// `pack` is a cooked/ directory or an assets.mce pack. `input` (optional) is sampled per tick.
// `base`: packs mounted after `pack` (a mod project's game, ADR-0034), so its assets win over them.
Result<HeadlessRun> run_headless(StringView pack, asset::Guid scene, u64 ticks, TickProgressFn progress = nullptr,
                                 void* user = nullptr, InspectFn inspect = nullptr, void* inspect_user = nullptr,
                                 app::IInputSource* input = nullptr, Span<const StringView> base = {},
                                 const RunHooks* hooks = nullptr);

}  // namespace mce::mcp
