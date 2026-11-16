#pragma once
// Summer butterflies as data (M-BUTTERFLY step 27a). One optional Butterfly per scene (the first in ECS order is read).
// install_weather registers the component; there is no system and nothing is stored between ticks.
//
//   Butterfly  Authoring (all Save, clamped to the Range when read and never rewritten): `enabled` (1); `window_full`
//              (0.07, 0..0.5) and `window_end` (0.14, 0..0.5) the half-widths, in season phase, of the summer trapezoid
//              around the summer centre 0.375 (full strength inside window_full, nothing beyond window_end, a smoothstep
//              between; an inverted or empty pair falls back to the defaults); `manual` (0, 0..1) a floor for the window, so
//              a scene can show butterflies all year; `min_daylight` (0.6, 0..1) the daylight (one minus the night fade)
//              below which none fly (a smoothstep from it to 1); `wind_calm` (0.25, 0..1) and `wind_still` (0.7, 0..1) the
//              wind below which they fly fully and above which none fly (an inverted or empty pair falls back to the
//              defaults); `rate` (5, 0..60) specks per second at full strength; `box_xz_m` (18, 1..100) half extent of the
//              spawn box around the camera; `height_m` (3, 1..20) height of the box above the ground-level camera;
//              `size_m` (0.12, 0.02..1); `lifetime_s` (8, 1..30); `speed` (0.8, 0..4) the drift speed in m/s; `lift` (0.2,
//              0..2) the slight upward acceleration; `color` (1.0 0.75 0.2, linear); `max_particles` (48, 8..512); `seed`
//              (1); `strength` (1, 0..1) scales every effect, 0 = off exactly.
//
// The amount is max(manual, window(phase)) * day_gate * calm_gate * (1 - rain) times strength. The window is a trapezoid in
// the unsigned circular distance of the phase from the summer centre; the day gate is smoothstep(min_daylight, 1,
// daylight); the calm gate is 1 - smoothstep(wind_calm, wind_still, wind). No Season gives the manual floor. The look
// turns the amount into the emitter description the extract (27b) adds as a local alpha-blended (not additive)
// render::ParticleEmitter: a few small specks that drift slowly and erratically. Polynomials only, fixed order, no rand.
// `enabled` 0, `strength` 0 and a missing component (a null pointer) give an amount of 0 and the inactive zero look as
// exact values, which the bit-identical render gates rely on.

#include <algorithm>
#include <cmath>

#include "mce/kit/season.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct Butterfly {
    f32 window_full = 0.07f;
    f32 window_end = 0.14f;
    f32 manual = 0.0f;
    f32 min_daylight = 0.6f;
    f32 wind_calm = 0.25f;
    f32 wind_still = 0.7f;
    f32 rate = 5.0f;
    f32 box_xz_m = 18.0f;
    f32 height_m = 3.0f;
    f32 size_m = 0.12f;
    f32 lifetime_s = 8.0f;
    f32 speed = 0.8f;
    f32 lift = 0.2f;
    f32 strength = 1.0f;
    math::vec3 color{1.0f, 0.75f, 0.2f};
    u32 max_particles = 48;
    u32 seed = 1;
    u8 enabled = 1;
    u8 pad_[3] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(Butterfly, (enabled, Save, Range{0, 1}, Help{"0: the butterflies are off"}),
              (window_full, Save, Range{0, 0.5f}, Help{"half-width in season phase of full strength around the summer centre"}),
              (window_end, Save, Range{0, 0.5f}, Help{"half-width in season phase beyond which there are no butterflies"}),
              (manual, Save, Range{0, 1}, Help{"floor for the window; shows butterflies all year"}),
              (min_daylight, Save, Range{0, 1}, Help{"daylight (1 - night fade) below which none fly"}),
              (wind_calm, Save, Range{0, 1}, Help{"wind below which they fly fully"}),
              (wind_still, Save, Range{0, 1}, Help{"wind above which none fly"}),
              (rate, Save, Range{0, 60}, Help{"specks per second at full strength"}),
              (box_xz_m, Save, Range{1, 100}, Help{"half extent of the spawn box around the camera, metres"}),
              (height_m, Save, Range{1, 20}, Help{"height of the spawn box above the camera, metres"}),
              (size_m, Save, Range{0.02f, 1}, Help{"butterfly sprite size, metres"}),
              (lifetime_s, Save, Range{1, 30}, Help{"butterfly lifetime, seconds"}),
              (speed, Save, Range{0, 4}, Help{"drift speed, metres per second"}),
              (lift, Save, Range{0, 2}, Help{"slight upward acceleration, metres per second squared"}),
              (color, Save, Help{"butterfly colour, linear"}),
              (max_particles, Save, Range{8, 512}, Help{"particle budget of the butterfly emitter"}),
              (seed, Save, Help{"random seed of the butterfly emitter"}),
              (strength, Save, Range{0, 1}, Help{"scales every effect; 0 is off exactly"}))

// The emitter description for an amount. At amount 0 (and for a disabled or zero-strength component) it is `active =
// false` and every other field zero. The extract copies the fields into a render::ParticleEmitter: `direction` is up
// (0, 1, 0) with a wide `spread`, so the specks wander in every direction, `gravity` is the slight lift, `color_start`
// fades out to `color_end` (same rgb, alpha 0).
struct ButterflyLook {
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

inline f32 butterfly_unit(f32 v, f32 def) { return season_clamp(v, def, 0.0f, 1.0f); }

// The unsigned circular distance of a year phase (0..1) from the summer centre, in [0, 0.5].
inline f32 butterfly_dist(f32 phase) {
    const f32 d = phase - kSeasonSummerCentre;
    return std::fabs(d - std::floor(d + 0.5f));
}

// A rising pair of thresholds clamped to lo..hi; an inverted or empty pair falls back to the defaults.
inline void butterfly_pair(f32 a, f32 b, f32 def_a, f32 def_b, f32 lo, f32 hi, f32& out_a, f32& out_b) {
    out_a = season_clamp(a, def_a, lo, hi);
    out_b = season_clamp(b, def_b, lo, hi);
    if (!(out_a < out_b)) {
        out_a = def_a;
        out_b = def_b;
    }
}

// Drag of the slow drift, 1/s.
inline constexpr f32 kButterflyDrag = 1.0f;

}  // namespace detail

// The butterfly amount 0..1. `phase` is the year phase, `night_fade` the 0..1 night fade (daylight is one minus it),
// `wind` the 0..1 wind, `rain` the 0..1 rain intensity. A null or disabled Season has no window, so the amount is the manual
// floor (still gated). Exactly 0 for `enabled` 0 and `strength` 0.
inline f32 butterfly_amount(const Butterfly& b, const Season* season, f32 phase, f32 night_fade, f32 wind, f32 rain) {
    if (b.enabled == 0) return 0.0f;
    const f32 st = detail::butterfly_unit(b.strength, 1.0f);
    if (!(st > 0.0f)) return 0.0f;
    f32 w = 0.0f;
    if (season != nullptr && season->enabled != 0) {
        f32 full, end;
        detail::butterfly_pair(b.window_full, b.window_end, 0.07f, 0.14f, 0.0f, 0.5f, full, end);
        w = (1.0f - detail::season_smooth(full, end, detail::butterfly_dist(detail::season_wrap(phase)))) * detail::season_strength(*season);
    }
    const f32 manual = detail::butterfly_unit(b.manual, 0.0f);
    const f32 base = std::max(manual, w);
    const f32 daylight = 1.0f - detail::butterfly_unit(night_fade, 0.0f);
    const f32 day = detail::season_smooth(detail::butterfly_unit(b.min_daylight, 0.6f), 1.0f, daylight);
    f32 calm, still;
    detail::butterfly_pair(b.wind_calm, b.wind_still, 0.25f, 0.7f, 0.0f, 1.0f, calm, still);
    const f32 calm_gate = 1.0f - detail::season_smooth(calm, still, detail::butterfly_unit(wind, 0.0f));
    const f32 dry = 1.0f - detail::butterfly_unit(rain, 0.0f);
    return std::clamp(base * day * calm_gate * dry * st, 0.0f, 1.0f);
}

// The emitter look at an amount (clamped to 0..1, NaN is 0).
inline ButterflyLook butterfly_look(const Butterfly& b, f32 amount) {
    ButterflyLook out;
    if (b.enabled == 0 || !(detail::butterfly_unit(b.strength, 1.0f) > 0.0f)) return out;
    const f32 a = detail::butterfly_unit(amount, 0.0f);
    if (!(a > 0.0f)) return out;
    out.active = true;
    out.rate = detail::season_clamp(b.rate, 5.0f, 0.0f, 60.0f) * a;
    const f32 sp = detail::season_clamp(b.speed, 0.8f, 0.0f, 4.0f);
    out.speed_min = 0.4f * sp;
    out.speed_max = sp;
    out.direction = math::vec3{0.0f, 1.0f, 0.0f};
    out.spread = 1.0f;
    out.gravity = math::vec3{0.0f, detail::season_clamp(b.lift, 0.2f, 0.0f, 2.0f), 0.0f};
    out.drag = detail::kButterflyDrag;
    out.size = detail::season_clamp(b.size_m, 0.12f, 0.02f, 1.0f);
    const f32 life = detail::season_clamp(b.lifetime_s, 8.0f, 1.0f, 30.0f);
    out.lifetime_min = 0.7f * life;
    out.lifetime_max = life;
    const math::vec3 rgb{detail::season_clamp(b.color.x, 1.0f, 0.0f, 1.0f), detail::season_clamp(b.color.y, 0.75f, 0.0f, 1.0f),
                         detail::season_clamp(b.color.z, 0.2f, 0.0f, 1.0f)};
    out.color_start = math::vec4{rgb.x, rgb.y, rgb.z, 0.9f * a};
    out.color_end = math::vec4{rgb.x, rgb.y, rgb.z, 0.0f};
    out.box = detail::season_clamp(b.box_xz_m, 18.0f, 1.0f, 100.0f);
    out.height = detail::season_clamp(b.height_m, 3.0f, 1.0f, 20.0f);
    return out;
}

// The same helpers for a component that may be missing: a null pointer is amount 0 and the inactive look.
inline f32 butterfly_amount(const Butterfly* b, const Season* season, f32 phase, f32 night_fade, f32 wind, f32 rain) {
    return b ? butterfly_amount(*b, season, phase, night_fade, wind, rain) : 0.0f;
}
inline ButterflyLook butterfly_look(const Butterfly* b, f32 amount) { return b ? butterfly_look(*b, amount) : ButterflyLook{}; }

}  // namespace mce::kit
