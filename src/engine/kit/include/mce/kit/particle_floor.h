#pragma once
// Particle box floor (M-PARTICLEFLOOR step 32a). A render-side weather emitter that follows the camera spawns in a box
// `position +- box`. For the butterfly, dusk bat and snow sparkle emitters the position was the camera itself, so with a
// camera at eye height the lower part of the box lay under the ground. `particle_floor_center_y` returns the emitter
// height whose box floor sits at the assumed ground (`camera_y - eye_m`): the spawn range becomes
// `camera_y - eye_m .. camera_y - eye_m + 2 * half_height`. Pure and polynomial; NaN, infinite and negative inputs fall
// back (camera 0, half height 0, eye `kDefaultEyeM`), so the result is always finite.

#include <cmath>

#include "mce/math/math.h"

namespace mce::kit {

inline constexpr f32 kDefaultEyeM = 1.6f;  // assumed camera height above the ground, metres

inline f32 particle_floor_center_y(f32 camera_y, f32 half_height, f32 eye_m) {
    const f32 cam = std::isfinite(camera_y) ? camera_y : 0.0f;
    const f32 half = (std::isfinite(half_height) && half_height > 0.0f) ? half_height : 0.0f;
    const f32 eye = (std::isfinite(eye_m) && eye_m >= 0.0f) ? eye_m : kDefaultEyeM;
    return cam - eye + half;
}

}  // namespace mce::kit
