#pragma once
// duplicate_offset — duplicate the selection and shift the copies in one step (M-DUPOFFSET). Laying out a row of
// pillars or a column of tiles is "duplicate, then move the copy by one step", over and over; this does both as one
// undo entry, and the copies are selected so the command can be repeated. Headless, tested in CI.

#include "mce/containers/array.h"
#include "mce/editor/editor_scene.h"

namespace mce::editor {

// Duplicates exactly `ids` (as Edit > Duplicate does) and moves the copies by (dx, dy) in world space; a copy whose
// parent is also a copy travels with that parent and is not moved twice. Returns how many entities were created
// (0: nothing to duplicate, and no undo entry). `created`, when given, receives every new entity.
usize duplicate_offset(EditorScene& scene, Span<const EntityId> ids, f32 dx, f32 dy, Array<EntityId>* created = nullptr);

}  // namespace mce::editor
