#pragma once
// make_names_unique — gives entities that share a name a numbered suffix: "Wall", "Wall" becomes "Wall", "Wall (2)"
// (M-UNIQUENAMES). The first holder of a name keeps it; a trailing " (n)" is replaced, not stacked. One undo entry.
// Headless, tested in CI.

#include "mce/containers/array.h"
#include "mce/editor/editor_scene.h"

namespace mce::editor {

// Renames, in document order, the entities of `ids` whose name is already taken by an entity outside `ids` or by an
// earlier one of `ids`. Returns how many were renamed (0: nothing changed, no undo entry).
usize make_names_unique(EditorScene& scene, Span<const EntityId> ids);

}  // namespace mce::editor
