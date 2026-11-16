#pragma once
// reset_transforms — put the local transform of the selected entities back to the identity (M-RESETXFORM): position
// 0, no rotation, scale 1. The entity moves relative to its parent, which is what Unity's and Unreal's "Reset" do.
// One undo entry. Headless, tested in CI.

#include "mce/containers/array.h"
#include "mce/editor/editor_scene.h"

namespace mce::editor {

// Resets the LocalTransform of every entity of `ids` that has one and is not already the identity; entities without a
// transform are skipped. Returns how many changed (no undo entry when none did).
usize reset_transforms(EditorScene& scene, Span<const EntityId> ids);

}  // namespace mce::editor
