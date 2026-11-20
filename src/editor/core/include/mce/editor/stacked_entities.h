#pragma once
// find_stacked — entities that are exact copies of each other (M-STACKLINT): the same prefab link, the same components
// with the same values (the name and the id do not count), which with a LocalTransform means they sit on the same spot
// under the same parent. Two copies drawn on top of each other look like one; the usual cause is an array or a
// duplicate with a step of zero. Read-only, document-level (no ECS), headless, tested in CI. Entities without a
// LocalTransform are never reported (nothing places them), and neither are entity references that differ (two torches
// that follow different targets are not copies).

#include "mce/containers/array.h"
#include "mce/cook/string.h"
#include "mce/editor/scene_document.h"

namespace mce::editor {

struct StackedGroup {
    Array<EntityId> ids;  // two or more, in the scene's entity order; the first is the one to keep
};

// The groups of exact copies, ordered by the scene position of their first entity.
Array<StackedGroup> find_stacked(const SceneDocument& doc);

// One line per group: `Crate (id 3) x3: ids 3, 7, 9`. Empty text for no groups.
cook::String format_stacked(const SceneDocument& doc, Span<const StackedGroup> groups);

}  // namespace mce::editor
