#pragma once
// A wind with a direction (M-WINDDIR step 8a). One per scene (the first in ECS order is read and advanced).
// install_weather registers it and the system "kit_wind_direction" (FixedUpdate_Pre, after "kit_weather").
//
//   WindDirection  `degrees` (0) the authored heading, 0 = +x, counter-clockwise seen from above (+z is 90), wrapped to
//                  0..360; `drift_degrees_per_minute` (0) how fast the heading turns (negative turns the other way).
//                  Engine-written and saved: `angle_q` (the heading in 1/65536-degree units, always below 360 * 65536),
//                  `frac` (the sub-quantum remainder of the drift, in 1/65536 of a quantum, so long runs lose nothing)
//                  and `seeded` (1 while a drift is running from `degrees`).
//
// "kit_wind_direction" is integer arithmetic on the saved state, with no wall clock and no RNG:
//   * drift 0 (or NaN): angle_q = round(degrees * 65536) wrapped modulo 360 * 65536 on every tick, frac and seeded
//     cleared. The result depends only on the authored fields, never on history, so editing `degrees` takes effect
//     on the next tick.
//   * drift != 0: the first tick starts from `degrees` (seeded 0 -> 1), then every tick adds the drift per tick,
//     step = ceil(drift * 2^32 / ticks_per_minute) in 1/2^32-degree units (ticks_per_minute = 60 / dt, snapped to the
//     integer when dt is a whole tick rate), to the accumulator frac + step. Rounding toward +inf keeps the error
//     below 2^-32 degree per tick and never negative, so a whole number of turns lands exactly for either sign.
//     The whole 1/65536-degree quanta move angle_q (floor, so a negative drift carries correctly) and the remainder
//     stays in `frac`. `degrees` is then only the start heading; stopping the drift (set it to 0) returns to the
//     authored `degrees`.
//   * Clamps: drift is clamped to +-3600 degrees per minute (NaN is 0, +-Inf the clamp), degrees to +-1e9 before
//     wrapping (NaN is 0). dt <= 0 or NaN does not advance.
//
// A scene without a WindDirection is unchanged (no other system reads it before the extract, step 8d).
//
// A second WindDirection in the scene is ignored: it stays frozen at its saved values.

#include <cmath>

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct WindDirection {
    f32 degrees = 0.0f;                   // authored heading: 0 = +x, counter-clockwise from above
    f32 drift_degrees_per_minute = 0.0f;  // authored turn rate
    u32 angle_q = 0;                      // engine-written: heading in 1/65536 degree, < 360 * 65536
    u32 frac = 0;                         // engine-written: drift remainder, 1/65536 of a quantum
    u32 seeded = 0;                       // engine-written: 1 while a drift runs from `degrees`
    u32 pad_ = 0;                         // explicit pad (state_hash hashes whole columns); reserved
};
MCE_COMPONENT(WindDirection,
              (degrees, Save, Range{0, 360},
               Help{"wind heading in degrees: 0 = +x, counter-clockwise seen from above"}),
              (drift_degrees_per_minute, Save, Range{-3600, 3600},
               Help{"degrees per minute the heading turns; 0 = fixed"}),
              (angle_q, Save, ReadOnly), (frac, Save, ReadOnly), (seeded, Save, ReadOnly))

// The wind's unit vector on the ground plane (x, z) from the quantized angle: (cos a, sin a), so degrees 90 is +z.
// For rendering and the extract only: it is computed with std::cos / std::sin, so it is not part of the state and
// must never feed back into it.
inline math::vec2 wind_direction_vector(const WindDirection& w) {
    const f64 a = static_cast<f64>(w.angle_q) / 65536.0 * (3.14159265358979323846 / 180.0);
    return math::vec2{static_cast<f32>(std::cos(a)), static_cast<f32>(std::sin(a))};
}

}  // namespace mce::kit
