#pragma once
// entity_path / entity_paths — the hierarchy path of an entity, "Room/Wall/Torch" (M-COPYPATH). Edit > Copy path puts
// the paths of the selection on the clipboard, one per line, for bug reports, scripts and MCP clients that address an
// entity by name. Headless, tested in CI.

#include "mce/containers/array.h"
#include "mce/cook/string.h"
#include "mce/editor/editor_scene.h"

namespace mce::editor {

// Names from the root down to `id`, joined by '/'. Empty for an entity that is not in the scene.
cook::String entity_path(const EditorScene& scene, EntityId id);

// The paths of `ids`, in the order given, one per line without a trailing newline; entities not in the scene are
// skipped.
cook::String entity_paths(const EditorScene& scene, Span<const EntityId> ids);

}  // namespace mce::editor
