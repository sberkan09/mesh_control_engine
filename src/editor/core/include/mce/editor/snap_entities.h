#pragma once
// snap_entities — put the selected entities on the grid (M-SNAPSEL). The gizmo snaps while dragging; this snaps what
// is already placed: the world X and Y of every entity are rounded to a multiple of `step`, Z and everything else stay.
// A parent's transform is respected. One undo entry. Headless, tested in CI.

#include "mce/containers/array.h"
#include "mce/editor/editor_scene.h"

namespace mce::editor {

// Rounds the world X and Y of every entity of `ids` that has a LocalTransform to the nearest multiple of `step`.
// Returns how many entities moved (no undo entry when none did, or when `step` is not positive).
usize snap_entities(EditorScene& scene, Span<const EntityId> ids, f32 step);

}  // namespace mce::editor
