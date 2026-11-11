#pragma once
// High cirrus streaks as data (M-CIRRUS step 53c). One optional Cirrus per scene (the first in ECS order is read).
// install_weather registers the component next to CloudLayer; there is no system and nothing is stored between ticks:
// the component never feeds the simulation or the state hash. The render side is render::Cirrus (53a, drawn by 53b);
// Kit3DExtract fills it from this component, the first Weather, the first WindDirection and the first WorldClock, then
// the quality step (render::apply_quality: the tier, `r.cirrus`, the clamps) applies.
//
//   Cirrus  Authoring (all Save, clamped to the Range): `strength` (1, 0..1) scales the cover, 0 = none;
//           `fair_coverage` (0.45, 0..1) the cover in clear weather; `front_coverage` (0.7, 0..1) the cover in the
//           hours before a change of weather; `density` (0.35, 0..1) how opaque the streaks are (the shader caps
//           them at 0.5); `scale_m` (3000, 200..40000) the noise cell across the wind in metres; `stretch` (6, 1..16)
//           how many times longer a cell is along the wind; `speed_scale` (2, 0..10) x Weather.wind for the drift
//           speed; `height_m` (9000, 1000..30000) the plane's height above the camera.
//
// cirrus_coverage is a pure function of the authoring, the Weather and the clock (no rand, no sin(), nothing
// written): see its comment. A scene without a Cirrus (or a Weather) is unchanged, bit for bit.

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct Weather;

struct Cirrus {
    f32 strength = 1.0f;
    f32 fair_coverage = 0.45f;
    f32 front_coverage = 0.7f;
    f32 density = 0.35f;
    f32 scale_m = 3000.0f;
    f32 stretch = 6.0f;
    f32 speed_scale = 2.0f;
    f32 height_m = 9000.0f;
};
MCE_COMPONENT(Cirrus, (strength, Save, Range{0, 1}, Help{"scales the cirrus cover; 0 = no cirrus"}),
              (fair_coverage, Save, Range{0, 1}, Help{"cirrus cover in clear weather"}),
              (front_coverage, Save, Range{0, 1}, Help{"cirrus cover in the hours before a change of weather"}),
              (density, Save, Range{0, 1}, Help{"how opaque the streaks are (the shader caps them at 0.5)"}),
              (scale_m, Save, Range{200, 40000}, Help{"size of one streak noise cell across the wind in metres"}),
              (stretch, Save, Range{1, 16}, Help{"how many times longer a cell is along the wind than across it"}),
              (speed_scale, Save, Range{0, 10}, Help{"scales the drift speed (the weather's wind)"}),
              (height_m, Save, Range{1000, 30000}, Help{"height of the cirrus plane above the camera in metres"}))

// The strength of the day variation (+- coverage) in cirrus_coverage.
inline constexpr f32 kCirrusDayVariation = 0.12f;
// The most the low deck takes off the cirrus at full overcast (the deck hides it anyway).
inline constexpr f32 kCirrusOvercastCut = 0.85f;

// The world clock as one number for cirrus_coverage: whole days plus the hour's share of a day (the hour clamps to
// 0..24, NaN and negative count as 0).
f32 cirrus_day_fraction(u32 day, f32 hour);

// The cover the extract hands the renderer, 0..1, a pure function of its arguments:
//   base = fair_coverage blended toward front_coverage by the change progress p of the weather: 1 while a change is
//   pending (Weather.target != current), else 1 - Weather.intensity for a kind that is not Clear (the front has
//   arrived and the deck thickens: the cirrus ran ahead of it), else 0 in settled clear weather;
//   plus a slow variation over the clock: kCirrusDayVariation x a value in -1..1 from an integer hash of the whole day
//   (floor of `day_fraction`) smoothstep-blended into the next day's;
//   times 1 - kCirrusOvercastCut x the low deck's blend (the kind factor Overcast / Rain / Storm 1, Snow 0.9, times
//   the clamped intensity), then clamped to 0..1 and times `strength`.
// NaN fields fall back to the defaults, out-of-range ones clamp, and front_coverage below fair_coverage (inverted)
// falls back to the default pair; a non-finite or negative `day_fraction` counts as 0. `strength` 0 returns exactly 0.
f32 cirrus_coverage(const Cirrus& c, const Weather& w, f32 day_fraction);

}  // namespace mce::kit
