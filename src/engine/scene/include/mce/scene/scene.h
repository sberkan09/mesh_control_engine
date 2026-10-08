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

Result<void> instantiate(World& world, const asset::SceneView& scene, Array<Entity>* spawned = nullptr);

// Loads the Scene asset `guid` from `assets` (one reference held only for the duration of the call)
// and instantiates it.
Result<void> load(World& world, asset::AssetSystem& assets, asset::Guid guid, Array<Entity>* spawned = nullptr);

// The ECS component for a reflected type, registered from the record when missing.
ecs::ComponentId component_for(World& world, const refl::TypeRecord& type);

}  // namespace mce::scene
