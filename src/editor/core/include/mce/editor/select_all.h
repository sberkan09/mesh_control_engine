#pragma once
// select_all / invert_selection — the two whole-scene selections (M-SELALL). Both return the entities in document
// order; the shell turns the result into the selection (Ctrl+A, Ctrl+I, Edit > Select). Headless, tested in CI.

#include "mce/containers/array.h"
#include "mce/editor/editor_scene.h"

namespace mce::editor {

// Every entity of the open scene.
Array<EntityId> select_all(const EditorScene& scene);

// Every entity that is not selected now (nothing selected: all of them; all selected: none).
Array<EntityId> invert_selection(const EditorScene& scene);

}  // namespace mce::editor
