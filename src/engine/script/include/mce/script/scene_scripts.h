#pragma once
// Scene scripts (M-SCRIPT 1): the Luau scripts a scene names with kit::ScriptSystem components,
// run by the stock hosts (mce_player, play-in-editor, MCP run_headless) in one call each.
//
// install_scene_scripts registers "script.pre" (FixedUpdate_Pre) and "script.post"
// (FixedUpdate_Post). At the start of each, the enabled ScriptSystems are read in (order, ECS
// order); when that list differs from the one loaded, a fresh ScriptHost loads every script's
// CHNK_SCRIPT bytecode from `assets` (a scene change or an edit between two ticks swaps the whole
// set). Scripts keep no state between ticks (ADR-0041), so a rebuild changes no simulation state.
// A script that fails to load or run is logged as "script <name>: <file>:<line>: message" and
// disabled; the other scripts keep running. A scene without ScriptSystems runs nothing and hashes
// exactly as before.

#include "mce/script/script.h"

namespace mce::asset {
class AssetSystem;
}

namespace mce::script {

// Idempotent. `assets` must outlive the World's ticks; null: ScriptSystems are reported, not run.
void install_scene_scripts(World& w, asset::AssetSystem* assets, const ScriptBudget& budget = {});

// The host running the scene's scripts (null before the first tick that found any): tools read
// its scripts() for state and errors.
const ScriptHost* scene_script_host(World& w);

}  // namespace mce::script
