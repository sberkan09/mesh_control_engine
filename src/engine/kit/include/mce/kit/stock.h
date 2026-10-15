#pragma once
// The kit systems the stock player runs on top of the engine modules (input, physics2d, audio),
// installed in one call so mce_player, play-in-editor and MCP run_headless run the same set
// (M-DATA-REACH): particles, sequences, timelines (timeline.h), kit3d, 3D navigation (nav3d.h), 3D joints (joints3d.h), WorldSettings and the effects (effects.h: sound,
// burst and shake triggers, 3D bodies (bodies3d.h), camera shake and follow, PhysicsEvents, the NavGrid and path agents) and
// sprite-sheet animation (sprite_anim.h). A component the stock player
// should act on gets its system installed here, never in only one of the three hosts.

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

// Idempotent. `assets` may be null (particle presets, sequences and mesh colliders then do not load).
void install_stock(World& world, asset::AssetSystem* assets);

// Only the WorldSettings system (Startup "kit_world_settings": the first WorldSettings into
// physics2d's World2D and kit3d's Physics3D, whichever are installed). install_stock calls it.
void install_world_settings(World& world);
// kit::UiFlow (components.h): FixedUpdate_Pre "kit_ui_flow" shows and hides screens on input
// actions and sets the world's hold; it runs on held ticks too. install_stock calls it.
void install_ui_flow(World& world);

}  // namespace mce::kit
