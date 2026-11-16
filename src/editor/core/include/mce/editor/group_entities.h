#pragma once
// group_entities / ungroup_entities — wrap entities in a new parent and take them out again (M-GROUP). Both are one
// undo entry and keep every entity where it sits in the scene (set_parent keeps the world transform). Headless,
// tested in CI.

#include "mce/containers/array.h"
#include "mce/editor/editor_scene.h"

namespace mce::editor {

// Creates an empty "Group" entity under the parent of the first entity of `ids` and moves the top-level entities of
// `ids` (those without an ancestor in `ids`) under it. Returns the group, or kNoEntity when nothing was grouped.
EntityId group_entities(EditorScene& scene, Span<const EntityId> ids);

// Moves the children of each entity of `ids` to that entity's parent and deletes the entity. Returns the freed
// children (document order); empty when no entity of `ids` had a child (nothing changes then).
Array<EntityId> ungroup_entities(EditorScene& scene, Span<const EntityId> ids);

}  // namespace mce::editor
