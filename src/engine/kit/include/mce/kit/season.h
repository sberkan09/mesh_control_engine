#pragma once
// A season that follows the time of year as data (M-SEASONS step 19a). One optional Season per scene (the first in ECS
// order is read). install_weather registers the component; there is no system and nothing is stored between ticks.
//
//   Season  Authoring (all Save, clamped to the Range when read and never rewritten): `enabled` (1); `year_days` (12,
//           1..3650) days in a year; `start_phase` (0.1, 0..1) the phase at day 0 hour 0; `spring_tint` (0.75 1.05 0.7),
//           `autumn_tint` (1.35 0.7 0.35) and `winter_tint` (0.7 0.72 0.75), linear multipliers of the authored
//           foliage tint at the centre of each season (summer is white, the authored colour untouched);
//           `winter_density` (0.4, 0..1) the share of copies left at the centre of winter; `snow_cover_bias` (0.5,
//           0..1) the extra snow cover drawn at the centre of winter; `cold_bias` (0.25, 0..1) the lee-bias shift at
//           the same point; `strength` (1, 0..1) scales every effect, 0 = off exactly.
//
// The phase is fract(start_phase + (day + hour / 24) / year_days), summed in f64 so a long run does not drift. The year
// is four equal quarters, spring [0, 0.25), summer [0.25, 0.5), autumn [0.5, 0.75), winter [0.75, 1), each centred at
// the middle of its quarter (0.125, 0.375, 0.625, 0.875). The helpers blend between neighbouring centres with a cubic
// smoothstep, so everything is continuous over the year and across the wrap at phase 1 to 0.
//
// The helpers are pure functions of the authoring, the phase and the clock; nothing is written anywhere. No rand, no
// pow/exp/trig; smoothstep polynomials in a fixed order. A zero `strength`, `enabled` 0 and a missing component (a null
// pointer) give the identity (white, 1, 0) as exact values, which the bit-identical render gates rely on.

#include <algorithm>
#include <cmath>

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct Season {
    f32 year_days = 12.0f;
    f32 start_phase = 0.1f;
    f32 winter_density = 0.4f;
    f32 snow_cover_bias = 0.5f;
    f32 cold_bias = 0.25f;
    f32 strength = 1.0f;
    math::vec3 spring_tint{0.75f, 1.05f, 0.7f};
    math::vec3 autumn_tint{1.35f, 0.7f, 0.35f};
    math::vec3 winter_tint{0.7f, 0.72f, 0.75f};
    u8 enabled = 1;
    u8 pad_[3] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(Season, (enabled, Save, Range{0, 1}, Help{"0: the season is off"}),
              (year_days, Save, Range{1, 3650}, Help{"days in a year; the short default shows every season in minutes"}),
              (start_phase, Save, Range{0, 1}, Help{"year phase at day 0 hour 0: 0 spring, 0.25 summer, 0.5 autumn, 0.75 winter (quarter starts)"}),
              (spring_tint, Save, Help{"foliage tint multiplier at the centre of spring, linear"}),
              (autumn_tint, Save, Help{"foliage tint multiplier at the centre of autumn, linear"}),
              (winter_tint, Save, Help{"foliage tint multiplier at the centre of winter, linear"}),
              (winter_density, Save, Range{0, 1}, Help{"share of foliage copies kept at the centre of winter"}),
              (snow_cover_bias, Save, Range{0, 1}, Help{"extra snow cover drawn at the centre of winter"}),
              (cold_bias, Save, Range{0, 1}, Help{"shift of the snow lee bias at the centre of winter"}),
              (strength, Save, Range{0, 1}, Help{"scales every effect; 0 is off exactly"}))

// The snow the season adds at a phase: a floor for the drawn cover and a shift of the lee bias, both in 0..1.
struct SeasonSnow {
    f32 cover_floor = 0.0f;
    f32 lee_shift = 0.0f;
};

// The centre of each season in phase units.
inline constexpr f32 kSeasonSpringCentre = 0.125f;
inline constexpr f32 kSeasonSummerCentre = 0.375f;
inline constexpr f32 kSeasonAutumnCentre = 0.625f;
inline constexpr f32 kSeasonWinterCentre = 0.875f;

namespace detail {

// NaN falls back to `def`, anything else clamps to [lo, hi].
inline f32 season_clamp(f32 v, f32 def, f32 lo, f32 hi) { return std::isnan(v) ? def : std::clamp(v, lo, hi); }

// 0 at or below lo (exactly), 1 at or above hi (exactly), the cubic smoothstep between; lo >= hi is a step at hi.
inline f32 season_smooth(f32 lo, f32 hi, f32 x) {
    if (!(lo < hi)) return x >= hi ? 1.0f : 0.0f;
    if (!(x > lo)) return 0.0f;
    if (!(x < hi)) return 1.0f;
    const f32 t = (x - lo) / (hi - lo);
    return t * t * (3.0f - 2.0f * t);
}

inline f32 season_year_days(const Season& s) {
    return (std::isnan(s.year_days) || !(s.year_days > 0.0f)) ? 12.0f : std::clamp(s.year_days, 1.0f, 3650.0f);
}
inline f32 season_strength(const Season& s) { return season_clamp(s.strength, 1.0f, 0.0f, 1.0f); }

// A phase brought into [0, 1); NaN counts as the centre of summer (the identity tint).
inline f32 season_wrap(f32 p) {
    if (std::isnan(p)) return kSeasonSummerCentre;
    if (p >= 0.0f && p < 1.0f) return p;
    const f32 f = p - std::floor(p);
    return f < 1.0f ? f : 0.0f;
}

inline math::vec3 season_tint_of(const math::vec3& v, const math::vec3& def) {
    return math::vec3{season_clamp(v.x, def.x, 0.0f, 4.0f), season_clamp(v.y, def.y, 0.0f, 4.0f),
                      season_clamp(v.z, def.z, 0.0f, 4.0f)};
}

inline math::vec3 season_mix(const math::vec3& a, const math::vec3& b, f32 s) {
    return math::vec3{a.x * (1.0f - s) + b.x * s, a.y * (1.0f - s) + b.y * s, a.z * (1.0f - s) + b.z * s};
}

// The winter weight: 0 from the start of the year to the end of autumn's start (phase 0.5), rising to 1 at the centre
// of winter (0.875) and falling back to 0 at the end of the year (1.0). Summer is exactly 0.
inline f32 season_winter_weight(f32 p) {
    if (p >= kSeasonWinterCentre) return 1.0f - season_smooth(kSeasonWinterCentre, 1.0f, p);
    return season_smooth(0.5f, kSeasonWinterCentre, p);
}

// How far the density has fallen, 0..1: 0 until late autumn (0.6875), 1 at the centre of winter, back to 0 at the
// centre of the next spring.
inline f32 season_thin_weight(f32 p) {
    const f32 u = p < kSeasonSpringCentre ? p + 1.0f : p;
    if (u >= kSeasonWinterCentre) return 1.0f - season_smooth(kSeasonWinterCentre, 1.0f + kSeasonSpringCentre, u);
    return season_smooth(0.6875f, kSeasonWinterCentre, u);
}

}  // namespace detail

// The year phase in [0, 1) at a world-clock day and hour. `hour` outside 0..24 or NaN counts as 0; the sum is in f64.
inline f32 season_phase(const Season& s, u32 day, f32 hour) {
    const f64 h = (std::isnan(hour) || hour < 0.0f || hour > 24.0f) ? 0.0 : static_cast<f64>(hour);
    const f64 start = static_cast<f64>(detail::season_clamp(s.start_phase, 0.1f, 0.0f, 1.0f));
    const f64 years = (static_cast<f64>(day) + h / 24.0) / static_cast<f64>(detail::season_year_days(s));
    const f64 sum = start + years;
    const f64 frac = sum - std::floor(sum);
    const f32 p = static_cast<f32>(frac);
    return p < 1.0f ? p : 0.0f;
}

// The foliage tint multiplier at a phase: white at the centre of summer, each authored tint at its centre, blended by a
// smoothstep between neighbouring centres, then lerped from white by `strength`. Exactly white for `enabled` 0 and
// `strength` 0.
inline math::vec3 season_foliage_tint(const Season& s, f32 phase) {
    const math::vec3 white{1.0f, 1.0f, 1.0f};
    if (s.enabled == 0) return white;
    const f32 st = detail::season_strength(s);
    if (!(st > 0.0f)) return white;
    const math::vec3 c[4] = {detail::season_tint_of(s.spring_tint, math::vec3{0.75f, 1.05f, 0.7f}), white,
                             detail::season_tint_of(s.autumn_tint, math::vec3{1.35f, 0.7f, 0.35f}),
                             detail::season_tint_of(s.winter_tint, math::vec3{0.7f, 0.72f, 0.75f})};
    // The segment runs from one centre to the next, starting at the spring centre.
    f32 q = detail::season_wrap(phase) - kSeasonSpringCentre;
    if (q < 0.0f) q += 1.0f;
    u32 seg = static_cast<u32>(q * 4.0f);
    if (seg > 3u) seg = 3u;
    const f32 t = (q - static_cast<f32>(seg) * 0.25f) * 4.0f;
    const math::vec3 m = detail::season_mix(c[seg], c[(seg + 1u) & 3u], detail::season_smooth(0.0f, 1.0f, t));
    return detail::season_mix(white, m, st);
}

// The share of foliage copies kept at a phase: 1 in spring, summer and most of autumn, falling to `winter_density` at the
// centre of winter and back to 1 at the centre of the next spring; lerped by `strength`. Exactly 1 for `enabled` 0 and
// `strength` 0.
inline f32 season_foliage_density(const Season& s, f32 phase) {
    if (s.enabled == 0) return 1.0f;
    const f32 st = detail::season_strength(s);
    if (!(st > 0.0f)) return 1.0f;
    const f32 wd = detail::season_clamp(s.winter_density, 0.4f, 0.0f, 1.0f);
    const f32 k = detail::season_thin_weight(detail::season_wrap(phase));
    const f32 d = 1.0f * (1.0f - k) + wd * k;
    return 1.0f * (1.0f - st) + d * st;
}

// The snow bias at a phase: `cover_floor` = snow_cover_bias * w and `lee_shift` = cold_bias * w, with w the winter weight
// (0 from the start of the year to the end of summer, 1 at the centre of winter), scaled by `strength`. Exactly 0 in
// summer, for `enabled` 0 and for `strength` 0.
inline SeasonSnow season_snow_bias(const Season& s, f32 phase) {
    if (s.enabled == 0) return SeasonSnow{};
    const f32 st = detail::season_strength(s);
    if (!(st > 0.0f)) return SeasonSnow{};
    const f32 w = detail::season_winter_weight(detail::season_wrap(phase));
    return SeasonSnow{detail::season_clamp(s.snow_cover_bias, 0.5f, 0.0f, 1.0f) * w * st,
                      detail::season_clamp(s.cold_bias, 0.25f, 0.0f, 1.0f) * w * st};
}

// The same helpers for a component that may be missing: a null pointer is the identity.
inline math::vec3 season_foliage_tint(const Season* s, f32 phase) {
    return s ? season_foliage_tint(*s, phase) : math::vec3{1.0f, 1.0f, 1.0f};
}
inline f32 season_foliage_density(const Season* s, f32 phase) { return s ? season_foliage_density(*s, phase) : 1.0f; }
inline SeasonSnow season_snow_bias(const Season* s, f32 phase) { return s ? season_snow_bias(*s, phase) : SeasonSnow{}; }

}  // namespace mce::kit
