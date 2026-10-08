#pragma once
// Layer-internal: the scene-editing panels (scene_panels.cpp).

#include "mce/editor/editor_shell.h"

namespace mce::editor {

// Registers Hierarchy, Inspector and Cook.
void add_scene_panels(EditorShell& shell);
// Registers the Play panel (play_panel.cpp).
void add_play_panel(EditorShell& shell);
// Registers the Assets browser (asset_panel.cpp).
void add_asset_panel(EditorShell& shell);
// Drag-drop payload of the asset browser: the asset's Guid.
inline constexpr const char* kAssetPayload = "MCE_ASSET";
// Destroys every selected entity (Del in the Hierarchy and the Viewport).
void delete_selection(EditorScene& scene);

}  // namespace mce::editor
