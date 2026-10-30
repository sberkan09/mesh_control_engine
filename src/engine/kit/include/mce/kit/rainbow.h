#pragma once
// An opt-in rainbow as data (M-RAINBOW step 17a). One optional Rainbow per scene (the first in ECS order is read).
// install_weather registers the component; there is no system and nothing is stored between ticks.
//
//   Rainbow  Authoring (all Save, clamped to the Range when read and never rewritten): `enabled` (1); `strength` (1,
//            0..4) brightness gain; `band_width` (1, 0.5..2) scales the angular width of both bows; `secondary` (1,
//            0..1) strength of the faint secondary bow (0 draws none); `min_wetness` (0.25, 0..1) the ground wetness
//            below which no bow shows; `max_sun_elevation_deg` (42, 10..60) the sun height above which the bow is
//            below the horizon and fades out over 10 degrees.
//
// The helpers are pure functions of the authoring, the sun elevation, the weather state and the cloud coverage;
// nothing is written anywhere. No rand, no pow/exp/trig: smoothsteps, polynomials and a piecewise-linear colour table
// only, evaluated in a fixed order, so the values are bit-stable across platforms. A zero level or strength (or
// `enabled` 0) gives exactly 0 from rainbow_level and rainbow_intensity, which the render off-check relies on.
//
// Geometry (angles are from the anti-solar point, degrees):
//   primary bow    centred 42.0, support 42 - 1.7 * band_width to 42 + 1.48 * band_width (about 1.8 wide at half
//                  height), peak 1;
//                  red on the outer edge (larger angle), violet on the inner edge.
//   secondary bow  centred 51.0, support 51 - 2.43 * band_width to 51 + 2.8 * band_width (about 3.0 wide at half
//                  height), peak 0.35 * secondary;
//                  the colours are reversed (violet outside, red inside).
//   Alexander band between the bows: the emission is simply 0 there. To make it read as slightly darker than the sky
//                  around, the side of each bow that faces the gap is steeper (its angular offset is scaled by 1.15),
//                  so the skirts are pulled in and the gap is a little wider than the bows are apart.

#include <algorithm>
#include <cmath>

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct Rainbow {
    f32 strength = 1.0f;
    f32 band_width = 1.0f;
    f32 secondary = 1.0f;
    f32 min_wetness = 0.25f;
    f32 max_sun_elevation_deg = 42.0f;
    u8 enabled = 1;
    u8 pad_[3] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(Rainbow, (enabled, Save, Range{0, 1}, Help{"0: the rainbow is off"}),
              (strength, Save, Range{0, 4}, Help{"brightness gain"}),
              (band_width, Save, Range{0.5f, 2.0f}, Help{"scales the angular width of the bands"}),
              (secondary, Save, Range{0, 1}, Help{"strength of the faint secondary bow; 0 draws none"}),
              (min_wetness, Save, Range{0, 1}, Help{"ground wetness below which no bow shows"}),
              (max_sun_elevation_deg, Save, Range{10, 60}, Help{"sun height above which the bow fades out, degrees"}))

namespace detail {

// NaN falls back to `def`, anything else clamps to [lo, hi].
inline f32 rainbow_clamp(f32 v, f32 def, f32 lo, f32 hi) { return std::isnan(v) ? def : std::clamp(v, lo, hi); }

// 0 at or below lo (exactly), 1 at or above hi (exactly), the cubic smoothstep between; lo >= hi counts as a step.
inline f32 rainbow_smooth(f32 lo, f32 hi, f32 x) {
    if (!(x > lo)) return 0.0f;
    if (!(x < hi)) return 1.0f;
    const f32 t = (x - lo) / (hi - lo);
    return t * t * (3.0f - 2.0f * t);
}

inline constexpr f32 kRainbowPrimaryCenter = 42.0f;
inline constexpr f32 kRainbowPrimaryHalf = 1.7f;
inline constexpr f32 kRainbowSecondaryCenter = 51.0f;
inline constexpr f32 kRainbowSecondaryHalf = 2.8f;
inline constexpr f32 kRainbowSecondaryPeak = 0.35f;
inline constexpr f32 kRainbowGapSteepness = 1.15f;

// (1 - x^2)^2 for |x| < 1, else 0: a smooth compact bump, 1 at the centre.
inline f32 rainbow_bump(f32 x) {
    const f32 q = 1.0f - x * x;
    return q > 0.0f ? q * q : 0.0f;
}

inline f32 rainbow_width(const Rainbow& r) {
    const f32 w = r.band_width;
    return (std::isnan(w) || !(w > 0.0f)) ? 1.0f : std::clamp(w, 0.5f, 2.0f);
}

}  // namespace detail

// The brightness gain of the whole effect: enabled * strength * sun_gate * wet_gate * (1 - rain) * (1 - cloud)^2.
//   sun_gate = smoothstep(0, 3, elevation) * (1 - smoothstep(max - 10, max, elevation)): 0 at or below the horizon,
//              1 from 3 degrees, fading out over the 10 degrees below `max_sun_elevation_deg` (clamped to 10..60, NaN
//              means 42); a NaN elevation counts as below the horizon.
//   wet_gate = smoothstep(0.5 * min_wetness, min_wetness + 0.15, wetness): closed at half the threshold, fully open
//              0.15 above it (min_wetness clamped to 0..1, NaN means 0.25), so a dry ground (wetness 0) is always 0.
//   rain and cloud are clamped to 0..1 (NaN counts as 0 rain, but a NaN wetness counts as 0 and so closes the gate).
// The result is exactly 0 when off, at zero strength or when any gate is closed. In [0, 4].
inline f32 rainbow_level(const Rainbow& r, f32 sun_elevation_deg, f32 wetness, f32 rain_intensity, f32 cloud_coverage) {
    using detail::rainbow_clamp;
    using detail::rainbow_smooth;
    if (r.enabled == 0) return 0.0f;
    const f32 strength = rainbow_clamp(r.strength, 1.0f, 0.0f, 4.0f);
    if (strength <= 0.0f) return 0.0f;
    const f32 max_el = rainbow_clamp(r.max_sun_elevation_deg, 42.0f, 10.0f, 60.0f);
    const f32 el = std::isnan(sun_elevation_deg) ? 0.0f : sun_elevation_deg;
    const f32 sun = rainbow_smooth(0.0f, 3.0f, el) * (1.0f - rainbow_smooth(max_el - 10.0f, max_el, el));
    const f32 mw = rainbow_clamp(r.min_wetness, 0.25f, 0.0f, 1.0f);
    const f32 wet = rainbow_smooth(0.5f * mw, mw + 0.15f, rainbow_clamp(wetness, 0.0f, 0.0f, 1.0f));
    if (sun <= 0.0f || wet <= 0.0f) return 0.0f;
    const f32 rain = 1.0f - rainbow_clamp(rain_intensity, 0.0f, 0.0f, 1.0f);
    const f32 clear = 1.0f - rainbow_clamp(cloud_coverage, 0.0f, 0.0f, 1.0f);
    const f32 level = strength * sun * wet * rain * (clear * clear);
    return level > 0.0f ? level : 0.0f;
}

// The brightness 0..1 of the two bows at `angle_deg` from the anti-solar point (NaN counts as 0, so far from both):
// the primary's bump (peak 1) plus the secondary's bump (peak 0.35 * secondary), see the geometry above. `strength`
// is the gain of rainbow_level and is not applied here; the result is exactly 0 when off or at zero strength, and
// 0 outside the two supports above. The bows do not overlap even at band_width 2 (43.0 + 2.96 = 44.96 against
// 51 - 4.87 = 46.13), so the sum is at most 1.
inline f32 rainbow_intensity(const Rainbow& r, f32 angle_deg) {
    using namespace detail;
    if (r.enabled == 0 || rainbow_clamp(r.strength, 1.0f, 0.0f, 4.0f) <= 0.0f) return 0.0f;
    if (std::isnan(angle_deg)) return 0.0f;
    const f32 w = rainbow_width(r);
    const f32 sec = rainbow_clamp(r.secondary, 1.0f, 0.0f, 1.0f);
    // primary: the outer side (larger angle) faces the gap
    f32 dp = (angle_deg - kRainbowPrimaryCenter) / (kRainbowPrimaryHalf * w);
    if (dp > 0.0f) dp *= kRainbowGapSteepness;
    f32 sum = rainbow_bump(dp);
    if (sec > 0.0f) {
        // secondary: the inner side (smaller angle) faces the gap
        f32 ds = (angle_deg - kRainbowSecondaryCenter) / (kRainbowSecondaryHalf * w);
        if (ds < 0.0f) ds *= kRainbowGapSteepness;
        sum += kRainbowSecondaryPeak * sec * rainbow_bump(ds);
    }
    return std::clamp(sum, 0.0f, 1.0f);
}

// The spectral colour (linear, each channel in [0, 1]) of the bows at `angle_deg` (NaN counts as 0): below 46.5 degrees
// (halfway between the bows) the primary ramp, violet at 42 - 1.7 and below to red at 42 + 1.7 and above; from 46.5 up
// the secondary ramp, reversed (red at 51 - 2.8 and below, violet at 51 + 2.8 and above). The ramp is the same
// seven-colour table (red, orange, yellow, green, cyan, blue, violet) interpolated linearly, so it is bit-stable. It
// is independent of band_width and strength: it describes the default geometry, and a caller that scales the bows
// samples it at the angle mapped back to the default width. The colour carries no brightness (see rainbow_intensity).
inline math::vec3 rainbow_color(f32 angle_deg) {
    using namespace detail;
    static constexpr f32 kTable[7][3] = {{1.00f, 0.04f, 0.02f},  // red
                                         {1.00f, 0.30f, 0.02f},  // orange
                                         {1.00f, 0.85f, 0.05f},  // yellow
                                         {0.10f, 0.85f, 0.10f},  // green
                                         {0.05f, 0.60f, 0.90f},  // cyan
                                         {0.10f, 0.25f, 1.00f},  // blue
                                         {0.45f, 0.10f, 0.95f}};  // violet
    const f32 a = std::isnan(angle_deg) ? 0.0f : angle_deg;
    // s = 0 at red, 1 at violet
    f32 s = 0.0f;
    if (a < 0.5f * (kRainbowPrimaryCenter + kRainbowSecondaryCenter)) {
        const f32 t = std::clamp((a - kRainbowPrimaryCenter) / kRainbowPrimaryHalf, -1.0f, 1.0f);  // -1 inner, +1 outer
        s = 0.5f - 0.5f * t;
    } else {
        const f32 t = std::clamp((a - kRainbowSecondaryCenter) / kRainbowSecondaryHalf, -1.0f, 1.0f);
        s = 0.5f + 0.5f * t;
    }
    const f32 x = s * 6.0f;
    const u32 i = std::min(static_cast<u32>(x), 5u);
    const f32 f = x - static_cast<f32>(i);
    const f32* c0 = kTable[i];
    const f32* c1 = kTable[i + 1];
    return math::vec3{std::clamp(c0[0] + (c1[0] - c0[0]) * f, 0.0f, 1.0f), std::clamp(c0[1] + (c1[1] - c0[1]) * f, 0.0f, 1.0f),
                      std::clamp(c0[2] + (c1[2] - c0[2]) * f, 0.0f, 1.0f)};
}

}  // namespace mce::kit
