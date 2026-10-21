#pragma once
// Scene instantiation (assets.md §6, editor-tools.md §3.3): a cooked CHNK_SCENE -> entities.
//
//   MCE_TRY_ASSIGN(asset::SceneView v, asset::SceneView::read(payload));
//   Array<Entity> spawned;
//   MCE_TRY(scene::instantiate(world, v, &spawned));     // spawned[i] = scene entity i
//
// Every column's type must be reflected (MCE_COMPONENT) in this process with the cooked size and
// alignment; the ECS component is registered from the reflection record when the game has not
// registered it yet (same identity as World::component_id<T>, refl::TypeRecord::type_hash).
// EntityRef fields are stamped with the spawned entity; AssetRef fields keep their GUID with a
// null handle (asset::AssetSystem::resolve(AssetRef&) re-stamps on first use). All-or-nothing:
// the scene is validated against the registry before anything is spawned.

#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/ecs/world.h"

namespace mce::scene {

// A scene with a ChildOf column also gets the transform hierarchy (ecs::install_hierarchy +
// propagate_transforms) unless `hierarchy` is false (the editor's preview world places children
// itself).
Result<void> instantiate(World& world, const asset::SceneView& scene, Array<Entity>* spawned = nullptr,
                         bool hierarchy = true);

// Loads the Scene asset `guid` from `assets` (one reference held only for the duration of the call)
// and instantiates it.
Result<void> load(World& world, asset::AssetSystem& assets, asset::Guid guid, Array<Entity>* spawned = nullptr);

// The ECS component for a reflected type, registered from the record when missing.
ecs::ComponentId component_for(World& world, const refl::TypeRecord& type);

// ---- scene changes (title -> level 1 -> level 2 -> win) ------------------------------------
//
//   scene::request_change(f.world(), "scenes/level2.mcscene");   // from any system, any phase
//
// The host (mce_player, the MCP server's runs, the editor's play mode) applies the request after
// the tick that made it: every entity goes (World::destroy_all), the target scene is loaded, and
// the Startup phase runs again on the next tick. Resources stay, so a score or a hit count kept in
// a game resource survives the change. Name the target by its source path or its GUID text
// ("guid:<uuid>" or the bare uuid), or by an AssetRef<asset::Scene> field's GUID: scene references
// are links, so scenes may refer to each other in a loop.
struct SceneChange {
    FixedString<256> next;     // the requested target; empty when nothing is pending
    FixedString<256> current;  // the source path of the scene loaded last (by load or apply_change)
    asset::Guid current_guid;
    u32 changes = 0;           // scene changes applied so far
    bool quit = false;         // request_quit: the host ends the run after this tick
    FixedString<64> load_slot;  // request_load: a save slot (save::load_game) to apply after this tick
};

// Asks the host to apply the save `slot` after this tick, on top of the scene that is loaded (a scene
// change asked for in the same tick happens first). A missing or damaged slot is logged and the world
// is left as it was; the host's own tick counter is not restored (mce.load in Luau).
inline void request_load(World& world, StringView slot) {
    SceneChange* c = world.try_resource<SceneChange>();
    if (!c) c = &world.set_resource(SceneChange{});
    c->load_slot.assign(slot);
}

// Asks the host to switch to `target` after this tick (the last request in a tick wins).
inline void request_change(World& world, StringView target) {
    SceneChange* c = world.try_resource<SceneChange>();
    if (!c) c = &world.set_resource(SceneChange{});
    c->next.assign(target);
}

inline void request_change(World& world, asset::Guid target) {
    FixedString<64> text("guid:");
    text.append(asset::to_string(target).view());
    request_change(world, text.view());
}

// Asks the host to end the run after this tick, as the window's close button does: mce_player
// quits cleanly (exit 0, the -save slot written), a headless run stops there, and the editor's
// play mode pauses. A Quit button: kit::UiFlow's quit_on, or a system that calls this.
inline void request_quit(World& world) {
    SceneChange* c = world.try_resource<SceneChange>();
    if (!c) c = &world.set_resource(SceneChange{});
    c->quit = true;
}

// Host call, between ticks: true once when the game asked to quit (the request is cleared).
inline bool take_quit_request(World& world) {
    SceneChange* c = world.try_resource<SceneChange>();
    if (!c || !c->quit) return false;
    c->quit = false;
    return true;
}

// The Scene asset named by `want`: GUID text ("guid:<uuid>" or the bare uuid) or a source path
// ("scenes/level.mcscene"; a leading "assets/" is ignored).
Result<asset::Guid> find_scene(const asset::AssetSystem& assets, StringView want);

// Host call, between ticks: applies a pending request_change (see above). Returns true when the
// scene changed; an unknown target is an error and leaves the world as it was. `spawned` gets the
// new scene's entities, as load gives them.
Result<bool> apply_change(World& world, asset::AssetSystem& assets, Array<Entity>* spawned = nullptr);

}  // namespace mce::scene
