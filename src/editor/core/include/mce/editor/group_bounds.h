#pragma once
// group_bounds — where a group of entities sits in the world (M-MEASURE 1): the box around their world X and Y, its
// middle and size, and the spread between the two furthest entities. Read-only, so it never makes an undo entry. It
// answers "how wide is this fence", "where is the middle of these lamps" before a layout command is chosen.
// Headless, tested in CI.

#include "mce/containers/array.h"
#include "mce/editor/editor_scene.h"

namespace mce::editor {

struct GroupBounds {
    usize count = 0;  // entities with a transform that were measured
    f32 min_x = 0.0f, min_y = 0.0f, max_x = 0.0f, max_y = 0.0f;
    f32 center_x = 0.0f, center_y = 0.0f;
    f32 width = 0.0f, height = 0.0f;  // max - min along X and Y
    f32 diagonal = 0.0f;              // the length of the box's diagonal: the most any two entities can be apart
};

// Measures the world positions of every listed entity that has a LocalTransform (an entity listed twice counts once).
// count is 0 and every number 0 when none qualifies.
GroupBounds group_bounds(const EditorScene& scene, Span<const EntityId> ids);

}  // namespace mce::editor
