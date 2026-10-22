#pragma once
// Seeded wind gusts (M-WINDGUST step 12a). One optional WindGust per scene (the first in ECS order drives).
// install_weather registers the component and the system "kit_wind_gust" (FixedUpdate_Pre, after
// "kit_wind_direction", hence after "kit_weather").
//
//   WindGust  Authoring (all Save, clamped to the Range when read and never rewritten): `strength` (0.6, 0..3) how much a
//             full gust adds, effective = wind * (1 + strength * gust); `min_gap_s` (2) and `max_gap_s` (8), 0..120,
//             the calm time between gusts; `min_rise_s` (0.4) and `max_rise_s` (1.5), 0.05..30; `min_fall_s` (1) and
//             `max_fall_s` (3), 0.05..60; `min_peak` (0.4) and `max_peak` (1), 0..1, the height of one gust; `seed`
//             (0 is a valid seed); `affects` (all five bits: kGustSway 1, kGustRain 2, kGustClouds 4, kGustSnow 8,
//             kGustAudio 16; the other bits of the u32 mean nothing); `enabled` (1).
//             Engine-written and saved: `gust_q` (the level, numerator over 2^24), `segment` (the gust counter), `tick`
//             (ticks into the segment), `gap_ticks`, `rise_ticks`, `fall_ticks` and `peak_q` (the segment's shape, stored
//             when the segment starts so a `dt` change or an authoring edit mid-gust cannot jump the curve), `seeded`
//             (0 until the first segment is built) and an explicit pad, zeroed every tick.
//
// "kit_wind_gust" is integer arithmetic on the saved state, with no wall clock and no RNG:
//   * The shape of a segment is a pure function of (seed, segment): the gap, rise and fall lengths and the peak come
//     from hash_mix(seed, segment * 4 + {0, 1, 2, 3}) (the mixer of kit_lightning), each mapped linearly into its
//     authored [min, max] by its top 24 bits. Seconds become ticks with one ceil(seconds / dt) at the start of the
//     segment (dt snapped to a whole tick rate as in kit_wind_direction) and are then stored. The rise and the fall are
//     at least 1 tick, the gap may be 0, every length is capped at 65536 ticks.
//   * Each tick `tick` advances by 1. The level is 0 during the gap, then peak_q * smoothstep(t / rise_ticks) over the
//     rise, then peak_q * smoothstep(1 - t / fall_ticks) over the fall, all in integer Q24 arithmetic. On the last tick
//     of the fall (level 0) `segment` increases, `tick` returns to 0 and the next shape is built. No float crosses a
//     tick; the only float is the one ceil per segment start, from authoring and dt.
//   * Clamps: NaN authoring falls back to the defaults, other values clamp to the Range; an inverted pair (min above
//     max) is swapped; zero-width ranges are fine. dt <= 0 or NaN does not advance (nothing is written).
//   * No Weather in the scene, no WindGust or `enabled` 0: the system returns before writing, the world is untouched.
//     A second WindGust is ignored and stays frozen at its saved values.
//
// The system never writes the Weather. `effective_wind` is the gusted wind for the extract, the audio and the
// precipitation systems; it never feeds state.

#include <algorithm>
#include <cmath>

#include "mce/kit/weather.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::kit {

inline constexpr u32 kGustSway = 1u;
inline constexpr u32 kGustRain = 2u;
inline constexpr u32 kGustClouds = 4u;
inline constexpr u32 kGustSnow = 8u;
inline constexpr u32 kGustAudio = 16u;
inline constexpr u32 kGustAll = 31u;

struct WindGust {
    f32 strength = 0.6f;
    f32 min_gap_s = 2.0f;
    f32 max_gap_s = 8.0f;
    f32 min_rise_s = 0.4f;
    f32 max_rise_s = 1.5f;
    f32 min_fall_s = 1.0f;
    f32 max_fall_s = 3.0f;
    f32 min_peak = 0.4f;
    f32 max_peak = 1.0f;
    u32 seed = 0;
    u32 affects = kGustAll;
    u8 enabled = 1;
    u8 seeded = 0;          // engine-written: 0 until the first segment is built
    u8 pad_[2] = {};        // explicit pad (state_hash hashes whole columns); zeroed every tick
    u32 gust_q = 0;         // engine-written: the level, numerator over 2^24
    u32 segment = 0;        // engine-written: gust counter
    u32 tick = 0;           // engine-written: ticks into the segment
    u32 gap_ticks = 0;      // engine-written: the segment's shape
    u32 rise_ticks = 0;
    u32 fall_ticks = 0;
    u32 peak_q = 0;
};
MCE_COMPONENT(WindGust, (strength, Save, Range{0, 3}, Help{"how much a full gust adds: wind * (1 + strength * gust)"}),
              (min_gap_s, Save, Range{0, 120}, Help{"shortest calm time between gusts, seconds"}),
              (max_gap_s, Save, Range{0, 120}, Help{"longest calm time between gusts, seconds"}),
              (min_rise_s, Save, Range{0.05f, 30}, Help{"shortest rise of a gust, seconds"}),
              (max_rise_s, Save, Range{0.05f, 30}, Help{"longest rise of a gust, seconds"}),
              (min_fall_s, Save, Range{0.05f, 60}, Help{"shortest fall of a gust, seconds"}),
              (max_fall_s, Save, Range{0.05f, 60}, Help{"longest fall of a gust, seconds"}),
              (min_peak, Save, Range{0, 1}, Help{"lowest gust height"}), (max_peak, Save, Range{0, 1}, Help{"highest gust height"}),
              (seed, Save, Help{"gust stream seed; 0 is a valid seed"}),
              (affects, Save, Help{"bit mask of consumers: 1 sway, 2 rain, 4 clouds, 8 snow, 16 audio"}),
              (enabled, Save, Help{"0: the system does nothing"}), (gust_q, Save, ReadOnly), (segment, Save, ReadOnly),
              (tick, Save, ReadOnly), (gap_ticks, Save, ReadOnly), (rise_ticks, Save, ReadOnly),
              (fall_ticks, Save, ReadOnly), (peak_q, Save, ReadOnly), (seeded, Save, ReadOnly))

// The wind a consumer should use. With no gust, `enabled` 0, the consumer's bit clear or a zero level it returns
// Weather.wind itself (the same f32, not wind * 1.0); otherwise clamp(wind * (1 + strength * gust_q / 2^24), 0, 1),
// computed in f64 and rounded once. A NaN strength counts as the default 0.6, strength clamps to 0..3, a level above
// 2^24 to 2^24, a NaN result returns Weather.wind. For the extract, the audio and the precipitation systems only.
inline f32 effective_wind(const Weather& weather, const WindGust* gust, u32 consumer_bit) {
    if (!gust || !gust->enabled || (gust->affects & consumer_bit) == 0 || gust->gust_q == 0) return weather.wind;
    const f64 strength = std::isnan(gust->strength) ? 0.6 : std::clamp(static_cast<f64>(gust->strength), 0.0, 3.0);
    const f64 level = static_cast<f64>(std::min<u32>(gust->gust_q, 1u << 24)) / 16777216.0;
    const f64 v = static_cast<f64>(weather.wind) * (1.0 + strength * level);
    if (std::isnan(v)) return weather.wind;
    return static_cast<f32>(std::clamp(v, 0.0, 1.0));
}

// Adds the system "kit_wind_gust" (called by install_weather, after kit_wind_direction). Idempotent only through
// install_weather.
void install_wind_gust(World& world);

}  // namespace mce::kit
