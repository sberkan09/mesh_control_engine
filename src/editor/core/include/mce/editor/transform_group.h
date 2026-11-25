#pragma once
// transform_group — turn and resize the selection as one body about a pivot (M-GROUPXFORM 1). Rotating each entity on
// its own spins every crate in place; rotating the group moves them round a common point and scales their distances,
// as a layout tool does. One undo entry. Headless, tested in CI.

#include "mce/containers/array.h"
#include "mce/editor/editor_scene.h"

namespace mce::editor {

struct GroupTransformParams {
    f32 degrees = 0.0f;      // counter-clockwise about +Z; entities turn by the same angle
    f32 scale = 1.0f;        // factor on their distances from the pivot and on their own size; > 0
    bool use_pivot = false;  // false: the middle of the group's bounding box (world X and Y)
    f32 pivot_x = 0.0f;
    f32 pivot_y = 0.0f;
};

// True when every number is finite and scale > 0.
bool group_transform_valid(const GroupTransformParams& p);

// Moves each entity of `ids` that has a LocalTransform to pivot + scale * R(degrees) * (position - pivot), turns it by
// `degrees` and multiplies its scale by `scale`. A listed parent carries its listed children consistently. Returns how
// many entities changed; 0 with no undo entry for an invalid `p`, nothing to change, or no turn and no resize.
usize transform_group(EditorScene& scene, Span<const EntityId> ids, const GroupTransformParams& p);

}  // namespace mce::editor
