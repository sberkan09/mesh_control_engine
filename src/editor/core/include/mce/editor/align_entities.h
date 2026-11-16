#pragma once
// align_entities / distribute_entities — line the selected entities up on one axis (M-ALIGN). They work on world
// positions of entities that have a LocalTransform (a parent's transform is respected), change only that axis, and
// are one undo entry. Headless, tested in CI.

#include "mce/containers/array.h"
#include "mce/editor/editor_scene.h"

namespace mce::editor {

enum class AlignAxis : u8 { X, Y };
enum class AlignMode : u8 { Min, Center, Max };  // left/bottom edge, middle of the extremes, right/top edge

// Moves every entity of `ids` to the lowest / middle / highest world position on `axis` among them. Needs two entities
// with a transform; true when something moved.
bool align_entities(EditorScene& scene, Span<const EntityId> ids, AlignAxis axis, AlignMode mode);

// Spreads the entities evenly on `axis` between the two outermost ones, which stay put; the order along the axis is
// kept. Needs three entities with a transform; true when something moved.
bool distribute_entities(EditorScene& scene, Span<const EntityId> ids, AlignAxis axis);

}  // namespace mce::editor
