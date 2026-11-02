#pragma once
// Midges as data (M-MIDGES step 33a). One optional Midge per scene (the first in ECS order is read). install_weather
// registers the component; there is no system and nothing is stored between ticks.
//
//   Midge      Authoring (all Save, clamped to the Range when read and never rewritten): `enabled` (1); `window_full`
//              (0.12, 0..0.5) and `window_end` (0.2, 0..0.5) the half-widths, in season phase, of the warm-season trapezoid
//              around the summer centre 0.375 (full strength inside window_full, nothing beyond window_end, a smoothstep
//              between; an inverted or empty pair falls back to the defaults); `manual` (0, 0..1) a floor that ignores the
//              window and twilight gates (not the rain), so a scene can show midges all year; `twilight_low` (-0.12, -1..0)
//              and `twilight_high` (0.25, 0..1) the sun elevation (the sine of the angle, 0 = horizon): the twilight gate is 0
//              below twilight_low, rises smoothly to 1 at the horizon and falls smoothly to 0 at twilight_high (a pair that
//              does not straddle the horizon falls back to the defaults); `wind_calm` (0.2, 0..1) and `wind_still` (0.6,
//              0..1) the wind below which they fly fully and above which none fly (an inverted or empty pair falls back to the
//              defaults); `rate` (30, 0..200) specks per second at full strength; `box_xz_m` (8, 1..100) half extent of the
//              spawn box around the camera; `height_m` (2.5, 0.3..20) height of the box above the ground-level camera;
//              `size_m` (0.025, 0.005..0.5); `lifetime_s` (3, 0.5..20); `speed` (0.35, 0..4) the jitter speed in m/s;
//              `color` (0.08 0.07 0.06, linear, dark); `max_particles` (192, 8..512); `seed` (1); `strength` (1, 0..1) scales
//              every effect, 0 = off exactly.
//
// The amount is max(manual, window(phase) * twilight(sun_elev)) * calm_gate * (1 - rain) times strength. The window is a
// trapezoid in the unsigned circular distance of the phase from the summer centre; the calm gate is 1 - smoothstep(wind_calm,
// wind_still, wind). No Season gives the manual floor. The look turns the amount into the emitter description the extract
// (33b) adds as a local alpha-blended (not additive) render::ParticleEmitter: many tiny dark specks with a high spread and
// almost no net speed, so they hover and shimmer. Polynomials only, fixed order, no rand. `enabled` 0, `strength` 0 and a
// missing component (a null pointer) give an amount of 0 and the inactive zero look as exact values, which the bit-identical
// render gates rely on.

#include <algorithm>
#include <cmath>

#include "mce/kit/season.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct Midge {
    f32 window_full = 0.12f;
    f32 window_end = 0.2f;
    f32 manual = 0.0f;
    f32 twilight_low = -0.12f;
    f32 twilight_high = 0.25f;
    f32 wind_calm = 0.2f;
    f32 wind_still = 0.6f;
    f32 rate = 30.0f;
    f32 box_xz_m = 8.0f;
    f32 height_m = 2.5f;
    f32 size_m = 0.025f;
    f32 lifetime_s = 3.0f;
    f32 speed = 0.35f;
    f32 strength = 1.0f;
    math::vec3 color{0.08f, 0.07f, 0.06f};
    u32 max_particles = 192;
    u32 seed = 1;
    u8 enabled = 1;
    u8 pad_[3] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(Midge, (enabled, Save, Range{0, 1}, Help{"0: the midges are off"}),
              (window_full, Save, Range{0, 0.5f}, Help{"half-width in season phase of full strength around the summer centre"}),
              (window_end, Save, Range{0, 0.5f}, Help{"half-width in season phase beyond which there are no midges"}),
              (manual, Save, Range{0, 1}, Help{"floor that ignores the window and twilight gates"}),
              (twilight_low, Save, Range{-1, 0}, Help{"sun elevation below which there are no midges"}),
              (twilight_high, Save, Range{0, 1}, Help{"sun elevation above which there are no midges"}),
              (wind_calm, Save, Range{0, 1}, Help{"wind below which they fly fully"}),
              (wind_still, Save, Range{0, 1}, Help{"wind above which none fly"}),
              (rate, Save, Range{0, 200}, Help{"specks per second at full strength"}),
              (box_xz_m, Save, Range{1, 100}, Help{"half extent of the spawn box around the camera, metres"}),
              (height_m, Save, Range{0.3f, 20}, Help{"height of the spawn box above the camera, metres"}),
              (size_m, Save, Range{0.005f, 0.5f}, Help{"midge speck size, metres"}),
              (lifetime_s, Save, Range{0.5f, 20}, Help{"midge lifetime, seconds"}),
              (speed, Save, Range{0, 4}, Help{"jitter speed, metres per second"}),
              (color, Save, Help{"midge colour, linear"}),
              (max_particles, Save, Range{8, 512}, Help{"particle budget of the midge emitter"}),
              (seed, Save, Help{"random seed of the midge emitter"}),
              (strength, Save, Range{0, 1}, Help{"scales every effect; 0 is off exactly"}))

// The emitter description for an amount. At amount 0 (and for a disabled or zero-strength component) it is `active =
// false` and every other field zero. The extract copies the fields into a render::ParticleEmitter: `direction` is up
// (0, 1, 0) with a full `spread`, so the specks jitter in every direction, there is no gravity, `color_start` fades out to
// `color_end` (same rgb, alpha 0); the sprite is alpha-blended, not additive (dark specks).
struct MidgeLook {
    bool active = false;
    f32 rate = 0.0f;
    f32 speed_min = 0.0f;
    f32 speed_max = 0.0f;
    math::vec3 direction{0.0f, 0.0f, 0.0f};
    f32 spread = 0.0f;
    math::vec3 gravity{0.0f, 0.0f, 0.0f};
    f32 drag = 0.0f;
    f32 size = 0.0f;
    f32 lifetime_min = 0.0f;
    f32 lifetime_max = 0.0f;
    math::vec4 color_start{0.0f, 0.0f, 0.0f, 0.0f};
    math::vec4 color_end{0.0f, 0.0f, 0.0f, 0.0f};
    f32 box = 0.0f;
    f32 height = 0.0f;
};

namespace detail {

inline f32 midge_unit(f32 v, f32 def) { return season_clamp(v, def, 0.0f, 1.0f); }

// The unsigned circular distance of a year phase (0..1) from the summer centre, in [0, 0.5].
inline f32 midge_dist(f32 phase) {
    const f32 d = phase - kSeasonSummerCentre;
    return std::fabs(d - std::floor(d + 0.5f));
}

// A rising pair of thresholds clamped to lo..hi; an inverted or empty pair falls back to the defaults.
inline void midge_pair(f32 a, f32 b, f32 def_a, f32 def_b, f32 lo, f32 hi, f32& out_a, f32& out_b) {
    out_a = season_clamp(a, def_a, lo, hi);
    out_b = season_clamp(b, def_b, lo, hi);
    if (!(out_a < out_b)) {
        out_a = def_a;
        out_b = def_b;
    }
}

// The twilight gate: 0 at or below `low`, a smooth rise to 1 at the horizon (0), a smooth fall to 0 at `high`. `low` must be
// below 0 and `high` above 0, otherwise the defaults (-0.12, 0.25) apply; NaN elevation is 0 (below the gate).
inline f32 midge_twilight(f32 low_in, f32 high_in, f32 sun_elev) {
    f32 low = season_clamp(low_in, -0.12f, -1.0f, 0.0f);
    f32 high = season_clamp(high_in, 0.25f, 0.0f, 1.0f);
    if (!(low < 0.0f) || !(high > 0.0f)) {
        low = -0.12f;
        high = 0.25f;
    }
    if (std::isnan(sun_elev)) return 0.0f;
    const f32 e = std::clamp(sun_elev, -1.0f, 1.0f);
    return season_smooth(low, 0.0f, e) * (1.0f - season_smooth(0.0f, high, e));
}

// Drag of the hovering specks, 1/s.
inline constexpr f32 kMidgeDrag = 1.5f;

}  // namespace detail

// The midge amount 0..1. `phase` is the year phase, `sun_elev` the sun elevation (the sine of the angle, -1..1), `wind` the 0..1
// wind, `rain` the 0..1 rain intensity. A null or disabled Season has no window, so the amount is the manual floor (still
// gated by wind and rain). Exactly 0 for `enabled` 0 and `strength` 0.
inline f32 midge_amount(const Midge& m, const Season* season, f32 phase, f32 sun_elev, f32 wind, f32 rain) {
    if (m.enabled == 0) return 0.0f;
    const f32 st = detail::midge_unit(m.strength, 1.0f);
    if (!(st > 0.0f)) return 0.0f;
    f32 w = 0.0f;
    if (season != nullptr && season->enabled != 0) {
        f32 full, end;
        detail::midge_pair(m.window_full, m.window_end, 0.12f, 0.2f, 0.0f, 0.5f, full, end);
        w = (1.0f - detail::season_smooth(full, end, detail::midge_dist(detail::season_wrap(phase)))) *
            detail::season_strength(*season);
    }
    const f32 twilight = detail::midge_twilight(m.twilight_low, m.twilight_high, sun_elev);
    const f32 manual = detail::midge_unit(m.manual, 0.0f);
    const f32 base = std::max(manual, w * twilight);
    f32 calm, still;
    detail::midge_pair(m.wind_calm, m.wind_still, 0.2f, 0.6f, 0.0f, 1.0f, calm, still);
    const f32 calm_gate = 1.0f - detail::season_smooth(calm, still, detail::midge_unit(wind, 0.0f));
    const f32 dry = 1.0f - detail::midge_unit(rain, 0.0f);
    return std::clamp(base * calm_gate * dry * st, 0.0f, 1.0f);
}

// The emitter look at an amount (clamped to 0..1, NaN is 0).
inline MidgeLook midge_look(const Midge& m, f32 amount) {
    MidgeLook out;
    if (m.enabled == 0 || !(detail::midge_unit(m.strength, 1.0f) > 0.0f)) return out;
    const f32 a = detail::midge_unit(amount, 0.0f);
    if (!(a > 0.0f)) return out;
    out.active = true;
    out.rate = detail::season_clamp(m.rate, 30.0f, 0.0f, 200.0f) * a;
    const f32 sp = detail::season_clamp(m.speed, 0.35f, 0.0f, 4.0f);
    out.speed_min = 0.3f * sp;
    out.speed_max = sp;
    out.direction = math::vec3{0.0f, 1.0f, 0.0f};
    out.spread = 1.0f;
    out.gravity = math::vec3{0.0f, 0.0f, 0.0f};
    out.drag = detail::kMidgeDrag;
    out.size = detail::season_clamp(m.size_m, 0.025f, 0.005f, 0.5f);
    const f32 life = detail::season_clamp(m.lifetime_s, 3.0f, 0.5f, 20.0f);
    out.lifetime_min = 0.6f * life;
    out.lifetime_max = life;
    const math::vec3 rgb{detail::season_clamp(m.color.x, 0.08f, 0.0f, 1.0f), detail::season_clamp(m.color.y, 0.07f, 0.0f, 1.0f),
                         detail::season_clamp(m.color.z, 0.06f, 0.0f, 1.0f)};
    out.color_start = math::vec4{rgb.x, rgb.y, rgb.z, 0.7f * a};
    out.color_end = math::vec4{rgb.x, rgb.y, rgb.z, 0.0f};
    out.box = detail::season_clamp(m.box_xz_m, 8.0f, 1.0f, 100.0f);
    out.height = detail::season_clamp(m.height_m, 2.5f, 0.3f, 20.0f);
    return out;
}

// The same helpers for a component that may be missing: a null pointer is amount 0 and the inactive look.
inline f32 midge_amount(const Midge* m, const Season* season, f32 phase, f32 sun_elev, f32 wind, f32 rain) {
    return m ? midge_amount(*m, season, phase, sun_elev, wind, rain) : 0.0f;
}
inline MidgeLook midge_look(const Midge* m, f32 amount) { return m ? midge_look(*m, amount) : MidgeLook{}; }

}  // namespace mce::kit
