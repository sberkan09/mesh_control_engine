#pragma once
// Snow that settles on up-facing surfaces as data (M-SNOWCOVER step 6c). One per scene (the first in ECS order is
// read and advanced). install_weather registers it and the system "kit_snow_cover" (after "kit_weather").
//
//   SnowCover  `strength` (1) scales the cover; `build_seconds` (90) from bare to fully covered at full snow
//              intensity; `melt_seconds` (120) from fully covered to bare; `coverage` (0.8) the share of up-facing
//              ground that whitens at full cover; `noise_scale_m` (3) the breakup noise cell in metres; `wind_bias`
//              (0.5) how much the layer favors the lee side. Engine-written and saved: `snow_q` (the cover as a
//              numerator out of 2^24) and `cover` (float 0..1, snow_q / 2^24).
//
// "kit_snow_cover" advances `snow_q` the way Weather.wet_q is advanced, in integers, with no wall clock and no RNG:
// while Weather.current is snow it rises by 2^24 * intensity / (build_seconds / dt) per tick (clamped to 2^24),
// otherwise (any other kind, or no Weather) it falls by 2^24 / (melt_seconds / dt), clamped at 0. A save mid-snow
// resumes on the same state.
//
// Kit3DExtract builds render::SnowCover only when the scene has both a Weather and a SnowCover:
// cover = SnowCover.cover * strength, the other fields copied, then the quality tier applies (render::apply_quality:
// Low drops the noise breakup, `r.snow_cover 0` drops all). A scene lacking either keeps cover 0, bit-identical to
// before. Weather's bytes are untouched.

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct SnowCover {
    f32 strength = 1.0f;        // x the accumulated cover
    f32 build_seconds = 90.0f;  // bare to fully covered at full snow intensity
    f32 melt_seconds = 120.0f;  // fully covered to bare
    f32 coverage = 0.8f;        // 0..1 share of up-facing ground that whitens at full cover
    f32 noise_scale_m = 3.0f;   // breakup noise cell in metres
    f32 wind_bias = 0.5f;       // 0..1 lee-side bias
    u32 snow_q = 0;             // engine-written: cover numerator out of 2^24
    f32 cover = 0.0f;           // engine-written: 0..1
    u32 pad_ = 0;               // explicit pad (state_hash hashes whole columns); reserved
};
MCE_COMPONENT(SnowCover, (strength, Save, Range{0, 4}, Help{"scales the accumulated snow cover; 0 = bare"}),
              (build_seconds, Save, Range{0, 86400}, Help{"seconds from bare to fully covered at full snow intensity"}),
              (melt_seconds, Save, Range{0, 86400}, Help{"seconds from fully covered to bare once it stops snowing"}),
              (coverage, Save, Range{0, 1}, Help{"share of up-facing ground that whitens at full cover"}),
              (noise_scale_m, Save, Range{0.1f, 100}, Help{"size of the snow breakup noise in metres"}),
              (wind_bias, Save, Range{0, 1}, Help{"how much the layer favors the lee side"}),
              (snow_q, Save, ReadOnly), (cover, Save, ReadOnly))

}  // namespace mce::kit
