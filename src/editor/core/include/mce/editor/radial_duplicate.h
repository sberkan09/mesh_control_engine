#pragma once
// radial_duplicate — copy the selection around a centre point in one step (M-RADIAL). A ring of pillars, the spokes of
// a wheel or a fan of torches is the same group turned about a pivot; this makes all the copies at once, as one undo
// entry, where array_duplicate lays them on a grid. Headless, tested in CI.

#include "mce/containers/array.h"
#include "mce/editor/editor_scene.h"

namespace mce::editor {

struct RadialParams {
    u32 count = 6;        // groups in all, the originals included
    f32 center_x = 0.0f;  // the pivot, world units
    f32 center_y = 0.0f;
    f32 total_degrees = 360.0f;  // 360 or more closes the ring; less spreads the groups over an arc, ends included
    bool turn = true;            // the copies turn with their position (a ring facing out); false keeps their heading
};

// The most copies one call makes (the same cap as array_duplicate).
constexpr usize kMaxRadialCopies = 4096;

bool radial_params_valid(const RadialParams& p);

// Copies exactly `ids` `count - 1` times, copy k moved about the centre by k steps of total_degrees / count (a full
// ring) or total_degrees / (count - 1) (an arc), counter-clockwise. Returns how many entities were created (0: invalid
// parameters, nothing to copy or more than kMaxRadialCopies, and no undo entry). `created`, when given, receives every
// new entity, copy by copy.
usize radial_duplicate(EditorScene& scene, Span<const EntityId> ids, const RadialParams& p,
                       Array<EntityId>* created = nullptr);

}  // namespace mce::editor
