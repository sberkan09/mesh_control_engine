#pragma once
// An entity that rides the drawn water surface (M-FLOAT step 55b). Opt-in per entity: a Floater next to a LocalTransform
// and a scene Water. install_weather registers the component and the system kit_floater (FixedUpdate_Pre, after
// kit_wind_direction).
//
//   Floater  Authoring (all Save, clamped to the Range when read and never rewritten): `enabled` (1); `offset_y` (0,
//            -50..50 m) the height of the entity origin above the surface (negative sinks it); `tilt` (1, 0..1) how far
//            the entity leans with the surface normal (0 = stays upright, 1 = the normal); `follow` (1, 0..1) the share
//            of the way to the target pose taken each fixed tick (1 = snaps, smaller values smooth the motion).
//
// kit_floater writes only the y of LocalTransform::position and LocalTransform::rotation. The heading about world Y is
// kept, x and z are never touched, and nothing is stored between ticks. There is no force: a body that needs physics
// reads water_height_at itself. Entities with a ChildOf are skipped (their LocalTransform is parent relative). Without a
// Water (or with it disabled) or with `enabled` 0 the entity is not touched. The scene time is tick * dt and the main wave
// direction is resolved like Kit3DExtract (the first WindDirection when Water::wind_follow > 0), so the entity rides the
// surface that is drawn.

#include "mce/kit/water.h"
#include "mce/kit/water_flow.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::kit {

struct Floater {
    f32 offset_y = 0.0f;
    f32 tilt = 1.0f;
    f32 follow = 1.0f;
    u8 enabled = 1;
    u8 pad_[3] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(Floater, (enabled, Save, Range{0, 1}, Help{"0: the entity is left alone"}),
              (offset_y, Save, Range{-50, 50}, Help{"height of the origin above the water surface, metres"}),
              (tilt, Save, Range{0, 1}, Help{"how far the entity leans with the surface normal; 0 stays upright"}),
              (follow, Save, Range{0, 1}, Help{"share of the way to the target pose taken per fixed tick; 1 snaps"}))

// The target pose at a world position: the world height of the origin and the unit up direction the entity leans toward
// (already blended by `tilt`, always with y above 0). `flow` may be null (still waves). Pure; NaN authoring means the
// default, other values clamp to the Range.
struct FloaterPose {
    f32 y = 0.0f;
    math::vec3 up{0.0f, 1.0f, 0.0f};
};
FloaterPose floater_pose(const Water& water, const WaterFlow* flow, const Floater& floater, math::vec3 position,
                         f64 scene_time, f32 wind_dir_deg);

// The rotation that keeps the heading (twist about world Y) of `current` and leans its up axis toward `up` (a unit
// vector with y above 0). `up` = (0, 1, 0) returns the pure twist.
math::quat floater_rotation(math::quat current, math::vec3 up);

// Registers Floater and adds kit_floater; install_weather calls it.
void install_floater(World& world);

}  // namespace mce::kit
