#pragma once
// Layer-internal: the scene-editing panels (scene_panels.cpp).

#include "mce/editor/editor_shell.h"

namespace mce::editor {

// Registers Hierarchy, Inspector and Cook.
void add_scene_panels(EditorShell& shell);
// Destroys every selected entity (Del in the Hierarchy and the Viewport).
void delete_selection(EditorScene& scene);

}  // namespace mce::editor
