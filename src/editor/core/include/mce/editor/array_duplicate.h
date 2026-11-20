#pragma once
// array_duplicate — copy the selection into a grid in one step (M-LAYOUTTOOLS 1). A fence, a row of lamps or a block of
// crates is the same group repeated at a fixed step; this makes all the copies at once, as one undo entry, where
// duplicate_offset makes one copy at a time. Headless, tested in CI.

#include "mce/containers/array.h"
#include "mce/editor/editor_scene.h"

namespace mce::editor {

// The most copies one call makes; a request for more is refused (0 returned, nothing changed).
constexpr usize kMaxArrayCopies = 4096;

// Copies exactly `ids` into a `columns` x `rows` grid whose cell (0, 0) is the originals: the copy in column i and row
// j sits at the original's world position plus (i * dx, j * dy). Returns how many entities were created (0: nothing to
// copy, a grid of one cell, a non-positive size, a non-finite step, or more than kMaxArrayCopies entities, and no undo
// entry). `created`, when given, receives every new entity, cell by cell (row-major, the originals' cell skipped).
usize array_duplicate(EditorScene& scene, Span<const EntityId> ids, u32 columns, u32 rows, f32 dx, f32 dy,
                      Array<EntityId>* created = nullptr);

}  // namespace mce::editor
