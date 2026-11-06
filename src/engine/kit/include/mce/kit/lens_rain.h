#pragma once
// Rain on the camera lens as data (M-LENSRAIN step 42d). One optional LensRain3D per scene (the first in ECS order is read);
// install_kit3d registers the component next to LensFlare3D. Kit3DExtract copies the fields into Scene3D post.rain with
// `amount` = wet_q / 2^24 (the render side, 42a to 42c).
//
//   LensRain3D  Authoring (all Save, clamped by the renderer to the Range): `enabled` (1); `density` (1, 0..4); `size` (1,
//               0.25..4); `refraction` (1, 0..2); `blur` (0.35, 0..1); `trail` (0.5, 0..1); `fall_speed` (0.25, 0..2);
//               `seed` (1); `wet_rate` (1, 0.1..4) scales how fast the lens wets and dries (NaN counts as 1).
//               Runtime state `wet_q` (Save): the lens wetness as an integer out of 2^24, like Weather::wet_q, so a save
//               and a replay resume the same. It is not ReadOnly: a scene without a Weather leaves it alone, so an agent or
//               a script may write it by hand.
//
// System "kit_lens_rain" (FixedUpdate_Pre, after "kit_weather", added by install_weather): advances wet_q from the first
// Weather in ECS order, in integers on the fixed tick dt (no float helper, no wall clock, no RNG), so it is replay
// deterministic and part of the world state hash. The rates follow render::lens_rain_wetness_step (rise about 4 s, fall about
// 40 s), divided by wet_rate; each is a whole number of ticks (llround of seconds / dt, at least 1) and a step of 2^24 / ticks
// (at least 1) per tick.
//   * Weather `current` is Rain or Storm: wet_q rises toward the target (2^24 * fade / fade_den, the integer behind
//     Weather::intensity) and stops there; above the target it falls back to it at the fall rate.
//   * Anything else: wet_q falls to 0 at the fall rate.
//   * No Weather in the scene, or dt <= 0: wet_q is left alone (only clamped to 2^24).

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct LensRain3D {
    f32 density = 1.0f;
    f32 size = 1.0f;
    f32 refraction = 1.0f;
    f32 blur = 0.35f;
    f32 trail = 0.5f;
    f32 fall_speed = 0.25f;
    u32 seed = 1;
    f32 wet_rate = 1.0f;
    u32 wet_q = 0;  // the lens wetness out of 2^24
    u8 enabled = 1;
    u8 pad_[3] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(LensRain3D, (enabled, Save, Range{0, 1}, Help{"0: no rain on the lens"}),
              (density, Save, Range{0, 4}, Help{"drops per cell, a multiplier"}),
              (size, Save, Range{0.25f, 4}, Help{"drop radius multiplier"}),
              (refraction, Save, Range{0, 2}, Help{"strength of the magnification inside a drop"}),
              (blur, Save, Range{0, 1}, Help{"how much the glass between the drops softens the picture"}),
              (trail, Save, Range{0, 1}, Help{"strength of the runs behind larger drops"}),
              (fall_speed, Save, Range{0, 2}, Help{"how fast the big drops creep down, view heights per minute"}),
              (seed, Save, Help{"selects the drop pattern"}),
              (wet_rate, Save, Range{0.1f, 4}, Help{"scales how fast the lens wets (about 4 s) and dries (about 40 s)"}),
              (wet_q, Save, Range{0, 16777216}, Help{"lens wetness out of 2^24, advanced from Weather; write it for a scene without one"}))

}  // namespace mce::kit
