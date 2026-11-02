#pragma once
// Dusk bats as data (M-BAT step 30a). One optional DuskBat per scene (the first in ECS order is read). install_weather
// registers the component; there is no system and nothing is stored between ticks.
//
//   DuskBat        Authoring (all Save, clamped to the Range when read and never rewritten): `enabled` (1); `window_full`
//              (0.16, 0..0.5) and `window_end` (0.26, 0..0.5) the half-widths, in season phase, of the warm-season trapezoid
//              around the summer centre 0.375 (full strength inside window_full, nothing beyond window_end, a smoothstep
//              between; an inverted or empty pair falls back to the defaults); `manual` (0, 0..1) a floor for the window, so
//              a scene can show bats all year; `dusk_low` (0.25, 0..1) and `dusk_high` (0.85, 0..1) the night-fade band in
//              which they fly (full strength inside it, none outside, a smoothstep ramp of width 0.1 at each edge; an
//              inverted or empty pair falls back to the defaults); `wind_calm` (0.3, 0..1) and `wind_still` (0.75, 0..1)
//              the wind below which they fly fully and above which none fly (an inverted or empty pair falls back to the
//              defaults); `rate` (4, 0..60) specks per second at full strength; `box_xz_m` (24, 1..100) half extent of the
//              spawn box around the camera; `height_m` (6, 1..20) height of the box above the ground-level camera;
//              `size_m` (0.1, 0.02..1); `lifetime_s` (5, 1..30); `speed` (3, 0..8) the dart speed in m/s; `color` (0.08
//              0.07 0.1, linear); `max_particles` (32, 8..512); `seed` (1); `strength` (1, 0..1) scales every effect, 0 =
//              off exactly.
//
// The amount is max(manual, window(phase)) * dusk_gate(night_fade) * calm_gate * (1 - rain) times strength. The window is a
// trapezoid in the unsigned circular distance of the phase from the summer centre; the dusk gate is
// smoothstep(dusk_low, dusk_low + 0.1, night_fade) * (1 - smoothstep(dusk_high - 0.1, dusk_high, night_fade)); the calm gate
// is 1 - smoothstep(wind_calm, wind_still, wind). No Season gives the manual floor. The look turns the amount into the
// emitter description the extract (30b) adds as a local alpha-blended (not additive) render::ParticleEmitter: a few small
// dark specks that dart quickly. Polynomials only, fixed order, no rand. `enabled` 0, `strength` 0 and a missing component
// (a null pointer) give an amount of 0 and the inactive zero look as exact values, which the bit-identical render gates
// rely on.

#include <algorithm>
#include <cmath>

#include "mce/kit/season.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct DuskBat {
    f32 window_full = 0.16f;
    f32 window_end = 0.26f;
    f32 manual = 0.0f;
    f32 dusk_low = 0.25f;
    f32 dusk_high = 0.85f;
    f32 wind_calm = 0.3f;
    f32 wind_still = 0.75f;
    f32 rate = 4.0f;
    f32 box_xz_m = 24.0f;
    f32 height_m = 6.0f;
    f32 size_m = 0.1f;
    f32 lifetime_s = 5.0f;
    f32 speed = 3.0f;
    f32 strength = 1.0f;
    math::vec3 color{0.08f, 0.07f, 0.1f};
    u32 max_particles = 32;
    u32 seed = 1;
    u8 enabled = 1;
    u8 pad_[3] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(DuskBat, (enabled, Save, Range{0, 1}, Help{"0: the bats are off"}),
              (window_full, Save, Range{0, 0.5f}, Help{"half-width in season phase of full strength around the summer centre"}),
              (window_end, Save, Range{0, 0.5f}, Help{"half-width in season phase beyond which there are no bats"}),
              (manual, Save, Range{0, 1}, Help{"floor for the window; shows bats all year"}),
              (dusk_low, Save, Range{0, 1}, Help{"night fade above which the bats fly (dark enough to read)"}),
              (dusk_high, Save, Range{0, 1}, Help{"night fade below which the bats fly (light enough to see)"}),
              (wind_calm, Save, Range{0, 1}, Help{"wind below which they fly fully"}),
              (wind_still, Save, Range{0, 1}, Help{"wind above which none fly"}),
              (rate, Save, Range{0, 60}, Help{"specks per second at full strength"}),
              (box_xz_m, Save, Range{1, 100}, Help{"half extent of the spawn box around the camera, metres"}),
              (height_m, Save, Range{1, 20}, Help{"height of the spawn box above the camera, metres"}),
              (size_m, Save, Range{0.02f, 1}, Help{"bat sprite size, metres"}),
              (lifetime_s, Save, Range{1, 30}, Help{"bat lifetime, seconds"}),
              (speed, Save, Range{0, 8}, Help{"dart speed, metres per second"}),
              (color, Save, Help{"bat colour, linear"}),
              (max_particles, Save, Range{8, 512}, Help{"particle budget of the bat emitter"}),
              (seed, Save, Help{"random seed of the bat emitter"}),
              (strength, Save, Range{0, 1}, Help{"scales every effect; 0 is off exactly"}))

// The emitter description for an amount. At amount 0 (and for a disabled or zero-strength component) it is `active =
// false` and every other field zero. The extract copies the fields into a render::ParticleEmitter: `direction` is up
// (0, 1, 0) with a wide `spread`, so the specks dart in every direction, `gravity` is almost none, `color_start` fades
// out to `color_end` (same rgb, alpha 0).
struct DuskBatLook {
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

inline f32 dusk_bat_unit(f32 v, f32 def) { return season_clamp(v, def, 0.0f, 1.0f); }

// The unsigned circular distance of a year phase (0..1) from the summer centre, in [0, 0.5].
inline f32 dusk_bat_dist(f32 phase) {
    const f32 d = phase - kSeasonSummerCentre;
    return std::fabs(d - std::floor(d + 0.5f));
}

// A rising pair of thresholds clamped to lo..hi; an inverted or empty pair falls back to the defaults.
inline void dusk_bat_pair(f32 a, f32 b, f32 def_a, f32 def_b, f32 lo, f32 hi, f32& out_a, f32& out_b) {
    out_a = season_clamp(a, def_a, lo, hi);
    out_b = season_clamp(b, def_b, lo, hi);
    if (!(out_a < out_b)) {
        out_a = def_a;
        out_b = def_b;
    }
}

// Width of the smoothing ramp at each edge of the dusk band, in night-fade units.
inline constexpr f32 kDuskBatDuskRamp = 0.1f;

// Drag of the quick darts, 1/s.
inline constexpr f32 kDuskBatDrag = 0.3f;

}  // namespace detail

// The bat amount 0..1. `phase` is the year phase, `night_fade` the 0..1 night fade, `wind` the 0..1 wind, `rain` the 0..1
// rain intensity. A null or disabled Season has no window, so the amount is the manual floor (still gated). Exactly 0 for
// `enabled` 0 and `strength` 0.
inline f32 dusk_bat_amount(const DuskBat& b, const Season* season, f32 phase, f32 night_fade, f32 wind, f32 rain) {
    if (b.enabled == 0) return 0.0f;
    const f32 st = detail::dusk_bat_unit(b.strength, 1.0f);
    if (!(st > 0.0f)) return 0.0f;
    f32 w = 0.0f;
    if (season != nullptr && season->enabled != 0) {
        f32 full, end;
        detail::dusk_bat_pair(b.window_full, b.window_end, 0.16f, 0.26f, 0.0f, 0.5f, full, end);
        w = (1.0f - detail::season_smooth(full, end, detail::dusk_bat_dist(detail::season_wrap(phase)))) * detail::season_strength(*season);
    }
    const f32 manual = detail::dusk_bat_unit(b.manual, 0.0f);
    const f32 base = std::max(manual, w);
    f32 low, high;
    detail::dusk_bat_pair(b.dusk_low, b.dusk_high, 0.25f, 0.85f, 0.0f, 1.0f, low, high);
    const f32 nf = detail::dusk_bat_unit(night_fade, 0.0f);
    const f32 dusk = detail::season_smooth(low, low + detail::kDuskBatDuskRamp, nf) *
                     (1.0f - detail::season_smooth(high - detail::kDuskBatDuskRamp, high, nf));
    f32 calm, still;
    detail::dusk_bat_pair(b.wind_calm, b.wind_still, 0.3f, 0.75f, 0.0f, 1.0f, calm, still);
    const f32 calm_gate = 1.0f - detail::season_smooth(calm, still, detail::dusk_bat_unit(wind, 0.0f));
    const f32 dry = 1.0f - detail::dusk_bat_unit(rain, 0.0f);
    return std::clamp(base * dusk * calm_gate * dry * st, 0.0f, 1.0f);
}

// The emitter look at an amount (clamped to 0..1, NaN is 0).
inline DuskBatLook dusk_bat_look(const DuskBat& b, f32 amount) {
    DuskBatLook out;
    if (b.enabled == 0 || !(detail::dusk_bat_unit(b.strength, 1.0f) > 0.0f)) return out;
    const f32 a = detail::dusk_bat_unit(amount, 0.0f);
    if (!(a > 0.0f)) return out;
    out.active = true;
    out.rate = detail::season_clamp(b.rate, 4.0f, 0.0f, 60.0f) * a;
    const f32 sp = detail::season_clamp(b.speed, 3.0f, 0.0f, 8.0f);
    out.speed_min = 0.5f * sp;
    out.speed_max = sp;
    out.direction = math::vec3{0.0f, 1.0f, 0.0f};
    out.spread = 1.0f;
    out.gravity = math::vec3{0.0f, -0.2f, 0.0f};
    out.drag = detail::kDuskBatDrag;
    out.size = detail::season_clamp(b.size_m, 0.1f, 0.02f, 1.0f);
    const f32 life = detail::season_clamp(b.lifetime_s, 5.0f, 1.0f, 30.0f);
    out.lifetime_min = 0.7f * life;
    out.lifetime_max = life;
    const math::vec3 rgb{detail::season_clamp(b.color.x, 0.08f, 0.0f, 1.0f), detail::season_clamp(b.color.y, 0.07f, 0.0f, 1.0f),
                         detail::season_clamp(b.color.z, 0.1f, 0.0f, 1.0f)};
    out.color_start = math::vec4{rgb.x, rgb.y, rgb.z, 0.85f * a};
    out.color_end = math::vec4{rgb.x, rgb.y, rgb.z, 0.0f};
    out.box = detail::season_clamp(b.box_xz_m, 24.0f, 1.0f, 100.0f);
    out.height = detail::season_clamp(b.height_m, 6.0f, 1.0f, 20.0f);
    return out;
}

// The same helpers for a component that may be missing: a null pointer is amount 0 and the inactive look.
inline f32 dusk_bat_amount(const DuskBat* b, const Season* season, f32 phase, f32 night_fade, f32 wind, f32 rain) {
    return b ? dusk_bat_amount(*b, season, phase, night_fade, wind, rain) : 0.0f;
}
inline DuskBatLook dusk_bat_look(const DuskBat* b, f32 amount) { return b ? dusk_bat_look(*b, amount) : DuskBatLook{}; }

}  // namespace mce::kit
