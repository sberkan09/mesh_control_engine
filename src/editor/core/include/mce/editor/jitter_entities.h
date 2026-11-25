#pragma once
// jitter_entities — give the selection a little random variation (M-SCATTER 1). A row of trees, a field of crates or a
// crowd of lamps that all stand on the grid at one size and one angle looks laid out by a ruler; this nudges each one
// by a bounded random amount: its world X and Y, its angle about +Z and its size. The same seed gives the same result,
// so a layout can be reproduced or tried again with another seed. One undo entry. Headless, tested in CI.

#include "mce/containers/array.h"
#include "mce/editor/editor_scene.h"

namespace mce::editor {

struct JitterParams {
    f32 move_x = 0.0f;          // each entity moves by a value in [-move_x, move_x] along world X
    f32 move_y = 0.0f;          // ... and in [-move_y, move_y] along world Y
    f32 rotate_degrees = 0.0f;  // its angle about +Z changes by a value in [-rotate_degrees, rotate_degrees]
    f32 scale_min = 1.0f;       // its scale is multiplied by a factor in [scale_min, scale_max]
    f32 scale_max = 1.0f;
    u32 seed = 1;
};

// True when every number is finite, the three ranges are not negative and 0 < scale_min <= scale_max.
bool jitter_params_valid(const JitterParams& p);

// Moves, turns and resizes every entity of `ids` that has a LocalTransform, each by its own random amount drawn from
// `p` (an entity listed twice counts once; the draws follow the order of `ids`). A parent's transform is respected and
// a parent that moves carries its children. Returns how many entities were changed; 0 with no undo entry for an
// invalid `p`, nothing to change, or ranges that are all zero.
usize jitter_entities(EditorScene& scene, Span<const EntityId> ids, const JitterParams& p);

}  // namespace mce::editor
