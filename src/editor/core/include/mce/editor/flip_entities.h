#pragma once
// flip_entities — mirror the selected entities across the middle of the selection on one axis (M-FLIP). Positions are
// mirrored in world space (a parent's transform is respected) and the local scale on that axis changes sign, so a
// sprite or mesh faces the other way. Flipping twice restores the scene. One undo entry. Headless, tested in CI.

#include "mce/containers/array.h"
#include "mce/editor/align_entities.h"
#include "mce/editor/editor_scene.h"

namespace mce::editor {

// Mirrors every entity of `ids` that has a LocalTransform across the centre of their extremes on `axis` (X: left to
// right, Y: top to bottom). A single entity flips in place (only its scale changes sign). True when something
// changed; false when no entity has a transform.
bool flip_entities(EditorScene& scene, Span<const EntityId> ids, AlignAxis axis);

}  // namespace mce::editor
