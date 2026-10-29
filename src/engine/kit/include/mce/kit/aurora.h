#pragma once
// An opt-in aurora as data (M-AURORA step 16a). One optional Aurora per scene (the first in ECS order is read).
// install_weather registers the component; there is no system and nothing is stored between ticks.
//
//   Aurora  Authoring (all Save, clamped to the Range when read and never rewritten): `enabled` (1); `latitude_deg`
//           (68, -90..90) the scene's magnetic latitude, authored (the engine has no globe; the sign only mirrors the
//           curtain side, the gate reads the absolute value); `gate_low_deg` (55) and `gate_high_deg` (65), 0..90: the
//           aurora is off below the first and fully on above the second (smoothstep between; an inverted or zero pair
//           falls back to the defaults); `strength` (1, 0..4) brightness gain; `curtain_count` (3, 1..6) ribbons;
//           `base_height` (0.18, 0.02..0.6) the lower edge of the curtains as the sine of the elevation; `height_span`
//           (0.35, 0.05..0.8) how far up they reach, same unit; `azimuth_deg` (0, 0..360) where the arc centres (0 = +z,
//           counter-clockwise from above, like WindDirection); `width_deg` (70, 10..180) the arc's angular width; `speed`
//           (1, 0..4) scales the sway and shimmer rates; `low_color` (0.10 1.00 0.35) and `high_color` (0.55 0.15 0.90),
//           linear; `night_only` (1) 1: scaled by the night fade, so it never shows by day; 0 forces it.
//
// The helpers are pure functions of the authoring, the night fade, the cloud coverage and the time; nothing is written
// anywhere. No rand, no hash of pointers, and no sin() of an unbounded argument: every animation rate is a whole number
// of cycles per kAuroraPeriodSeconds (the speed is quantised to quarters for that), the phase is the fractional turn
// computed in f64, so the pattern repeats exactly after the period and the time is wrapped to it in f64. A closed gate,
// a zero strength or `enabled` 0 gives exactly 0 from aurora_level and aurora_curtain_intensity.

#include <algorithm>
#include <cmath>

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct Aurora {
    f32 latitude_deg = 68.0f;
    f32 gate_low_deg = 55.0f;
    f32 gate_high_deg = 65.0f;
    f32 strength = 1.0f;
    f32 base_height = 0.18f;
    f32 height_span = 0.35f;
    f32 azimuth_deg = 0.0f;
    f32 width_deg = 70.0f;
    f32 speed = 1.0f;
    math::vec3 low_color{0.10f, 1.00f, 0.35f};
    math::vec3 high_color{0.55f, 0.15f, 0.90f};
    u8 curtain_count = 3;
    u8 enabled = 1;
    u8 night_only = 1;
    u8 pad_[1] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(Aurora, (enabled, Save, Help{"0: the aurora is off"}),
              (latitude_deg, Save, Range{-90, 90}, Help{"magnetic latitude of the scene, degrees (authored)"}),
              (gate_low_deg, Save, Range{0, 90}, Help{"absolute latitude below which the aurora is off"}),
              (gate_high_deg, Save, Range{0, 90}, Help{"absolute latitude above which the aurora is fully on"}),
              (strength, Save, Range{0, 4}, Help{"brightness gain"}),
              (curtain_count, Save, Range{1, 6}, Help{"how many ribbons"}),
              (base_height, Save, Range{0.02f, 0.6f}, Help{"lower edge of the curtains, sine of the elevation"}),
              (height_span, Save, Range{0.05f, 0.8f}, Help{"how far up the curtains reach, sine of the elevation"}),
              (azimuth_deg, Save, Range{0, 360}, Help{"where the arc centres: 0 = +z, counter-clockwise from above"}),
              (width_deg, Save, Range{10, 180}, Help{"angular width of the arc, degrees"}),
              (speed, Save, Range{0, 4}, Help{"scales the sway and shimmer rates"}),
              (low_color, Save, Help{"colour of the curtain base, linear"}),
              (high_color, Save, Help{"colour of the curtain top, linear"}),
              (night_only, Save, Range{0, 1}, Help{"1: scaled by the night fade; 0: shows by day too"}))

// The seconds after which every animation of the curtains repeats exactly.
inline constexpr f64 kAuroraPeriodSeconds = 64.0;

namespace detail {

// NaN falls back to `def`, anything else clamps to [lo, hi].
inline f32 aurora_clamp(f32 v, f32 def, f32 lo, f32 hi) { return std::isnan(v) ? def : std::clamp(v, lo, hi); }

inline f32 aurora_smooth(f32 lo, f32 hi, f32 x) {
    if (!(x > lo)) return 0.0f;
    if (!(x < hi)) return 1.0f;
    const f32 t = (x - lo) / (hi - lo);
    return t * t * (3.0f - 2.0f * t);
}

inline constexpr f32 kAuroraTwoPi = 6.2831853071795864769f;

// pcg2d (Jarzynski and Olano 2020), one lane: the integer hash the sky shader's cloud_hash uses, as a pure function.
inline u32 aurora_hash(u32 x, u32 y) {
    u32 vx = x * 1664525u + 1013904223u;
    u32 vy = y * 1664525u + 1013904223u;
    vx += vy * 1664525u;
    vy += vx * 1664525u;
    vx ^= vx >> 16u;
    vy ^= vy >> 16u;
    vx += vy * 1664525u;
    vy += vx * 1664525u;
    vx ^= vx >> 16u;
    return vx;
}

// The speed in quarters (0..16) so that every rate is a whole number of cycles per period.
inline u32 aurora_speed_quarters(f32 speed) {
    return static_cast<u32>(std::lround(aurora_clamp(speed, 1.0f, 0.0f, 4.0f) * 4.0f));
}

// The fractional turn (0..1) of an animation that makes `cycles_per_quarter * quarters` cycles per period, at the time
// `tw` wrapped to the period. Exact periodicity: the product is a whole number of cycles.
inline f32 aurora_turns(f32 tw, u32 cycles_per_quarter, u32 quarters) {
    const f64 x = static_cast<f64>(tw) / kAuroraPeriodSeconds * static_cast<f64>(cycles_per_quarter * quarters);
    return static_cast<f32>(x - std::floor(x));
}

// sin(2 pi (turns + offset)) with the argument folded to a fraction first, so it is never large.
inline f32 aurora_sin_turns(f32 turns, f32 offset) {
    const f32 x = turns + offset;
    return std::sin(kAuroraTwoPi * (x - std::floor(x)));
}

// The gate's two ends: an inverted or zero pair and NaN fall back to the defaults 55 and 65.
inline void aurora_gate_range(const Aurora& a, f32& lo, f32& hi) {
    lo = aurora_clamp(a.gate_low_deg, 55.0f, 0.0f, 90.0f);
    hi = aurora_clamp(a.gate_high_deg, 65.0f, 0.0f, 90.0f);
    if (!(lo < hi)) {
        lo = 55.0f;
        hi = 65.0f;
    }
}

}  // namespace detail

// smoothstep(gate_low, gate_high, abs(latitude)) as a fixed polynomial: 0 at or below gate_low (exactly), 1 at or above
// gate_high (exactly), monotone between, the same for -latitude. A NaN latitude counts as 0 (closed).
inline f32 aurora_latitude_gate(const Aurora& a) {
    f32 lo = 0.0f, hi = 0.0f;
    detail::aurora_gate_range(a, lo, hi);
    const f32 lat = std::isnan(a.latitude_deg) ? 0.0f : std::fabs(std::clamp(a.latitude_deg, -90.0f, 90.0f));
    return detail::aurora_smooth(lo, hi, lat);
}

// The brightness gain of the whole effect: enabled * gate * strength * (night_only ? night_fade : 1) *
// (1 - 0.85 * coverage). `night_fade` and `cloud_coverage` are clamped to 0..1 (NaN counts as 0). Exactly 0 when the
// aurora is off, the gate is closed, the strength is 0 or it is day with night_only. In [0, 4].
inline f32 aurora_level(const Aurora& a, f32 night_fade, f32 cloud_coverage) {
    if (a.enabled == 0) return 0.0f;
    const f32 gate = aurora_latitude_gate(a);
    const f32 strength = detail::aurora_clamp(a.strength, 1.0f, 0.0f, 4.0f);
    if (gate <= 0.0f || strength <= 0.0f) return 0.0f;
    const f32 fade = a.night_only != 0 ? detail::aurora_clamp(night_fade, 0.0f, 0.0f, 1.0f) : 1.0f;
    const f32 cov = detail::aurora_clamp(cloud_coverage, 0.0f, 0.0f, 1.0f);
    const f32 level = gate * strength * fade * (1.0f - 0.85f * cov);
    return level > 0.0f ? level : 0.0f;
}

// The time wrapped to [0, kAuroraPeriodSeconds) in f64; NaN and infinity count as 0.
inline f32 aurora_wrap_time(f64 t) {
    if (!std::isfinite(t)) return 0.0f;
    const f64 w = t - std::floor(t / kAuroraPeriodSeconds) * kAuroraPeriodSeconds;
    return static_cast<f32>(w >= kAuroraPeriodSeconds ? 0.0 : w);
}

// The lower and upper edge (sines of the elevation) of the curtain at the arc parameter `u` (0..1, clamped; NaN counts
// as 0.5) and the time `t` seconds (wrapped to the period in f64 inside, so a caller may pass the raw clock): the
// lower edge is base_height with a slow sine sway of up to 20 percent, the upper edge is the lower plus height_span
// with a second sway of up to 20 percent. Always 0 < lower < upper <= 1.
inline math::vec2 aurora_curtain_height(const Aurora& a, f32 u, f32 t) {
    using detail::aurora_clamp;
    const f32 base = aurora_clamp(a.base_height, 0.18f, 0.02f, 0.6f);
    const f32 span = aurora_clamp(a.height_span, 0.35f, 0.05f, 0.8f);
    const f32 uu = aurora_clamp(u, 0.5f, 0.0f, 1.0f);
    const f32 tw = aurora_wrap_time(static_cast<f64>(t));
    const u32 q = detail::aurora_speed_quarters(a.speed);
    const f32 lower = std::min(base * (1.0f + 0.2f * detail::aurora_sin_turns(detail::aurora_turns(tw, 1, q), 0.9f * uu)), 0.95f);
    const f32 upper = lower + span * (1.0f + 0.2f * detail::aurora_sin_turns(detail::aurora_turns(tw, 3, q), 1.3f * uu + 0.25f));
    return math::vec2{lower, std::min(upper, 1.0f)};
}

// The brightness 0..1 at the arc parameter `u` and the relative height `h01` in the curtain (both 0..1; outside, or
// NaN, gives 0) at the time `t` (wrapped as above): a sharp lower edge (smoothstep over the lowest 8 percent), an
// exponential fall toward the top, vertical ray striations (a pcg2d hash of the integer ray cell, 16 cells per ribbon),
// a faster per-ray shimmer, a slow pulse of the whole curtain and an end fade at the arc ends (a wider arc fades over a
// shorter stretch). Exactly 0 when the aurora is off, the gate is closed or the strength is 0; the strength itself is
// the gain of aurora_level, not applied here. Zero at u = 0, u = 1 and h01 = 0.
inline f32 aurora_curtain_intensity(const Aurora& a, f32 u, f32 h01, f32 t) {
    using detail::aurora_clamp;
    if (a.enabled == 0 || aurora_latitude_gate(a) <= 0.0f || aurora_clamp(a.strength, 1.0f, 0.0f, 4.0f) <= 0.0f) return 0.0f;
    if (!(u > 0.0f && u < 1.0f && h01 > 0.0f && h01 <= 1.0f)) return 0.0f;
    const f32 width = aurora_clamp(a.width_deg, 70.0f, 10.0f, 180.0f);
    const f32 fade_len = 0.5f - 0.3f * ((width - 10.0f) / 170.0f);
    const f32 ends = detail::aurora_smooth(0.0f, fade_len, u) * detail::aurora_smooth(0.0f, fade_len, 1.0f - u);
    const f32 edge = detail::aurora_smooth(0.0f, 0.08f, h01);
    const f32 fall = std::exp(-2.5f * h01);
    const u32 count = std::clamp<u32>(a.curtain_count, 1u, 6u);
    const u32 cells = 16u * count;
    const u32 cell = std::min(static_cast<u32>(u * static_cast<f32>(cells)), cells - 1u);
    const f32 r = static_cast<f32>(detail::aurora_hash(cell, 7u) >> 8u) * (1.0f / 16777216.0f);
    const f32 ray = 0.4f + 0.6f * r;
    const f32 tw = aurora_wrap_time(static_cast<f64>(t));
    const u32 q = detail::aurora_speed_quarters(a.speed);
    const f32 shimmer = 0.85f + 0.15f * detail::aurora_sin_turns(detail::aurora_turns(tw, 5, q), r);
    const f32 pulse = 0.9f + 0.1f * detail::aurora_sin_turns(detail::aurora_turns(tw, 2, q), 0.0f);
    return std::clamp(ends * edge * fall * ray * shimmer * pulse, 0.0f, 1.0f);
}

// The colour at the relative height `h01` (clamped to 0..1, NaN counts as 0): low_color * (1 - s) + high_color * s with
// s = smoothstep(0.55, 1, h01), so the lower part (and so the bright fringe) stays low_color and the top is
// high_color. Each channel of the authored colours is clamped to 0..1 (NaN means the default channel); exactly
// low_color at 0 and high_color at 1.
inline math::vec3 aurora_band_color(const Aurora& a, f32 h01) {
    using detail::aurora_clamp;
    const math::vec3 lo{aurora_clamp(a.low_color.x, 0.10f, 0.0f, 1.0f), aurora_clamp(a.low_color.y, 1.00f, 0.0f, 1.0f),
                        aurora_clamp(a.low_color.z, 0.35f, 0.0f, 1.0f)};
    const math::vec3 hi{aurora_clamp(a.high_color.x, 0.55f, 0.0f, 1.0f), aurora_clamp(a.high_color.y, 0.15f, 0.0f, 1.0f),
                        aurora_clamp(a.high_color.z, 0.90f, 0.0f, 1.0f)};
    const f32 s = detail::aurora_smooth(0.55f, 1.0f, aurora_clamp(h01, 0.0f, 0.0f, 1.0f));
    return math::vec3{lo.x * (1.0f - s) + hi.x * s, lo.y * (1.0f - s) + hi.y * s, lo.z * (1.0f - s) + hi.z * s};
}

}  // namespace mce::kit
