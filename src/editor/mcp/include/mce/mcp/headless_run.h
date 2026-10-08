#pragma once
// run_headless (editor-mcp.md §5): the player's headless path, in process. A fresh AssetSystem
// mounts the project's cooked tree and the scene plays exactly as `mce_player -headless 1` plays it
// (physics2d + the audio bridge on a Silent device, scene::load, the app loop's stepping clock), so
// the same scene + ticks give the same World::state_hash as the player (tests/mcp checks this).

#include "mce/asset/guid.h"
#include "mce/ecs/world.h"

namespace mce::mcp {

struct HeadlessRun {
    u64 ticks = 0;
    usize entities = 0;  // spawned by the scene
    u64 state_hash = 0;
    WorldMetrics metrics;
};

// Called every `kProgressTicks` ticks and after the last one.
using TickProgressFn = void (*)(u64 done, u64 total, void* user);
inline constexpr u64 kProgressTicks = 60;

// `pack` is a cooked/ directory or an assets.mce pack.
Result<HeadlessRun> run_headless(StringView pack, asset::Guid scene, u64 ticks, TickProgressFn progress = nullptr,
                                 void* user = nullptr);

}  // namespace mce::mcp
