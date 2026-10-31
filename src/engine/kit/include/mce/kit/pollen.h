#pragma once
// Spring pollen and dandelion seeds as data (M-POLLEN step 24a). One optional Pollen per scene (the first in ECS order is
// read). install_weather registers the component; there is no system and nothing is stored between ticks.
//
//   Pollen   Authoring (all Save, clamped to the Range when read and never rewritten): `enabled` (1); `spring_start`
//            (0.95) and `spring_full` (0.02), `spring_end_full` (0.22) and `spring_end` (0.32), all 0..1, the year phases
//            where the pollen starts to show, reaches full strength, starts to thin and is gone (the window is centred on
//            the spring centre 0.125 and WRAPS the year start, see below; an inverted pair is swapped, an empty one falls
//            back to the defaults); `manual` (0, 0..1) a floor for the amount, so a scene with no Season can still show
//            pollen; `day_only` (1, 0..1) 1: scaled by the day gate, 0: shows by night too; `min_daylight` (0.5, 0..1) the
//            daylight (one minus the night fade) below which none show (a smoothstep from it to 1); `rate` (14, 0..200)
//            specks per second at full strength; `box_xz_m` (18, 1..100) half extent of the spawn box around the camera;
//            `height_m` (5, 1..30) height of the box above the ground-level camera; `size_m` (0.06, 0.02..0.5);
//            `lifetime_s` (12, 1..30); `fall_speed` (0.08, 0..2) the terminal fall speed in m/s; `wind_follow` (0.6, 0..2)
//            how much of the scene wind the specks follow; `color` (1.0 0.95 0.7, linear); `max_particles` (512, 16..4096);
//            `seed` (1); `strength` (1, 0..1) scales every effect, 0 = off exactly.
//
// The amount is max(manual, window(phase)) * day_gate * (1 - rain) times strength, a pure function of the authoring, the
// Season, the phase, the night fade and the rain. The spring window straddles the year start (0.95 .. 0.32), so it cannot
// be a plain interval of the phase. Instead every threshold and the phase are turned into a signed circular distance from
// the spring centre, d = wrap(x - 0.125) in [-0.5, 0.5), by subtracting floor(d + 0.5): no trig, one floor. In that
// distance the window is an ordinary trapezoid of smoothsteps, rising over the distance of spring_start..spring_full
// (-0.175..-0.105 by default) and falling over spring_end_full..spring_end (+0.095..+0.195), so the year wrap point is just
// an interior point of the rising edge. The look turns the amount into the emitter description the extract (24b) adds as
// a local alpha-blended (not additive) render::ParticleEmitter. No rand, no pow/exp/trig; polynomial smoothsteps in a fixed
// order. `enabled` 0, `strength` 0 and a missing component (a null pointer) give an amount of 0 and the inactive zero look
// as exact values, which the bit-identical render gates rely on.

#include <algorithm>
#include <cmath>

#include "mce/kit/season.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct Pollen {
    f32 spring_start = 0.95f;
    f32 spring_full = 0.02f;
    f32 spring_end_full = 0.22f;
    f32 spring_end = 0.32f;
    f32 manual = 0.0f;
    f32 min_daylight = 0.5f;
    f32 rate = 14.0f;
    f32 box_xz_m = 18.0f;
    f32 height_m = 5.0f;
    f32 size_m = 0.06f;
    f32 lifetime_s = 12.0f;
    f32 fall_speed = 0.08f;
    f32 wind_follow = 0.6f;
    f32 strength = 1.0f;
    math::vec3 color{1.0f, 0.95f, 0.7f};
    u32 max_particles = 512;
    u32 seed = 1;
    u8 enabled = 1;
    u8 day_only = 1;
    u8 pad_[2] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(Pollen, (enabled, Save, Range{0, 1}, Help{"0: the pollen is off"}),
              (spring_start, Save, Range{0, 1}, Help{"year phase where pollen starts to show (may be before the year start)"}),
              (spring_full, Save, Range{0, 1}, Help{"year phase where it reaches full strength"}),
              (spring_end_full, Save, Range{0, 1}, Help{"year phase where it starts to thin"}),
              (spring_end, Save, Range{0, 1}, Help{"year phase where the last speck is gone"}),
              (manual, Save, Range{0, 1}, Help{"floor for the amount; shows pollen with no Season"}),
              (day_only, Save, Range{0, 1}, Help{"1: only by day; 0: by night too"}),
              (min_daylight, Save, Range{0, 1}, Help{"daylight (1 - night fade) below which no pollen shows"}),
              (rate, Save, Range{0, 200}, Help{"specks per second at full strength"}),
              (box_xz_m, Save, Range{1, 100}, Help{"half extent of the spawn box around the camera, metres"}),
              (height_m, Save, Range{1, 30}, Help{"height of the spawn box above the camera, metres"}),
              (size_m, Save, Range{0.02f, 0.5f}, Help{"speck sprite size, metres"}),
              (lifetime_s, Save, Range{1, 30}, Help{"speck lifetime, seconds"}),
              (fall_speed, Save, Range{0, 2}, Help{"terminal fall speed, metres per second"}),
              (wind_follow, Save, Range{0, 2}, Help{"how much of the scene wind the specks follow"}),
              (color, Save, Help{"speck colour, linear"}),
              (max_particles, Save, Range{16, 4096}, Help{"particle budget of the pollen emitter"}),
              (seed, Save, Help{"random seed of the pollen emitter"}),
              (strength, Save, Range{0, 1}, Help{"scales every effect; 0 is off exactly"}))

// The emitter description for an amount. At amount 0 (and for a disabled or zero-strength component) it is `active =
// false` and every other field zero. The extract copies the fields into a render::ParticleEmitter: `direction` is a unit
// vector, mostly down and tilted along the wind, `spread` scatters it, gravity and drag give the slow terminal fall,
// `color_start` fades out to `color_end` (same rgb, alpha 0).
struct PollenLook {
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

inline f32 pollen_unit(f32 v, f32 def) { return season_clamp(v, def, 0.0f, 1.0f); }

// The signed circular distance of a year phase x (0..1) from the spring centre, in [-0.5, 0.5): the wrap-safe form that
// lets the window straddle the year start. 0.95 is -0.175, 0.0 is -0.125, 0.125 is 0, 0.32 is +0.195.
inline f32 pollen_dist(f32 x) {
    const f32 d = x - kSeasonSpringCentre;
    const f32 r = d - std::floor(d + 0.5f);
    return std::clamp(r, -0.5f, 0.4999999f);
}

// Two thresholds (year phases) read as a rising or falling pair of signed distances: clamped to 0..1, converted, an
// inverted pair swapped, an empty one the defaults (given as phases).
inline void pollen_pair(f32 a, f32 b, f32 def_a, f32 def_b, f32& lo, f32& hi) {
    lo = pollen_dist(pollen_unit(a, def_a));
    hi = pollen_dist(pollen_unit(b, def_b));
    if (lo > hi) std::swap(lo, hi);
    if (!(lo < hi)) {
        lo = pollen_dist(def_a);
        hi = pollen_dist(def_b);
    }
}

// Gravity of the slow fall, m/s^2; drag = gravity / fall_speed gives the terminal speed.
inline constexpr f32 kPollenGravity = 2.0f;
// The slowest terminal speed the look divides by.
inline constexpr f32 kPollenMinSpeed = 0.01f;
// The wind speed (m/s) the specks follow at most, so a storm cannot blow them out of the box.
inline constexpr f32 kPollenMaxWind = 6.0f;

}  // namespace detail

// The pollen amount 0..1. `phase` is the year phase, `night_fade` the 0..1 night fade (daylight is one minus it), `rain`
// the 0..1 rain intensity. A null or disabled Season (or one with strength 0) has no window, so the amount is the manual
// floor. Exactly 0 for `enabled` 0 and `strength` 0.
inline f32 pollen_amount(const Pollen& po, const Season* season, f32 phase, f32 night_fade, f32 rain) {
    if (po.enabled == 0) return 0.0f;
    const f32 st = detail::pollen_unit(po.strength, 1.0f);
    if (!(st > 0.0f)) return 0.0f;
    f32 w = 0.0f;
    if (season != nullptr && season->enabled != 0) {
        f32 s0, s1, e0, e1;
        detail::pollen_pair(po.spring_start, po.spring_full, 0.95f, 0.02f, s0, s1);
        detail::pollen_pair(po.spring_end_full, po.spring_end, 0.22f, 0.32f, e0, e1);
        const f32 d = detail::pollen_dist(detail::season_wrap(phase));
        w = detail::season_smooth(s0, s1, d) * (1.0f - detail::season_smooth(e0, e1, d)) * detail::season_strength(*season);
    }
    const f32 manual = detail::pollen_unit(po.manual, 0.0f);
    const f32 base = std::max(manual, w);
    const f32 daylight = 1.0f - detail::pollen_unit(night_fade, 0.0f);
    const f32 gate = po.day_only == 0 ? 1.0f : detail::season_smooth(detail::pollen_unit(po.min_daylight, 0.5f), 1.0f, daylight);
    const f32 dry = 1.0f - detail::pollen_unit(rain, 0.0f);
    return std::clamp(base * gate * dry * st, 0.0f, 1.0f);
}

// The emitter look at an amount (clamped to 0..1, NaN is 0), the scene wind speed and its horizontal direction (x, z).
inline PollenLook pollen_look(const Pollen& po, f32 amount, f32 wind, const math::vec2& wind_dir) {
    PollenLook out;
    if (po.enabled == 0 || !(detail::pollen_unit(po.strength, 1.0f) > 0.0f)) return out;
    const f32 a = detail::pollen_unit(amount, 0.0f);
    if (!(a > 0.0f)) return out;
    out.active = true;
    out.rate = detail::season_clamp(po.rate, 14.0f, 0.0f, 200.0f) * a;
    const f32 fs = std::max(detail::season_clamp(po.fall_speed, 0.08f, 0.0f, 2.0f), detail::kPollenMinSpeed);
    const f32 follow = detail::season_clamp(po.wind_follow, 0.6f, 0.0f, 2.0f);
    const f32 wv = std::isnan(wind) ? 0.0f : std::clamp(wind, 0.0f, detail::kPollenMaxWind);
    const f32 w = wv * follow;
    const f32 dx = std::isnan(wind_dir.x) ? 0.0f : wind_dir.x;
    const f32 dz = std::isnan(wind_dir.y) ? 0.0f : wind_dir.y;
    out.direction = math::normalize(math::vec3{dx * w, -1.0f, dz * w});
    out.speed_min = 0.5f * fs + 0.25f * w;
    out.speed_max = 1.0f * fs + 0.5f * w;
    out.spread = 0.6f;
    out.gravity = math::vec3{0.0f, -detail::kPollenGravity, 0.0f};
    out.drag = detail::kPollenGravity / fs;
    out.size = detail::season_clamp(po.size_m, 0.06f, 0.02f, 0.5f);
    const f32 life = detail::season_clamp(po.lifetime_s, 12.0f, 1.0f, 30.0f);
    out.lifetime_min = 0.7f * life;
    out.lifetime_max = life;
    const math::vec3 rgb{detail::season_clamp(po.color.x, 1.0f, 0.0f, 1.0f), detail::season_clamp(po.color.y, 0.95f, 0.0f, 1.0f),
                         detail::season_clamp(po.color.z, 0.7f, 0.0f, 1.0f)};
    out.color_start = math::vec4{rgb.x, rgb.y, rgb.z, 0.8f * a};
    out.color_end = math::vec4{rgb.x, rgb.y, rgb.z, 0.0f};
    out.box = detail::season_clamp(po.box_xz_m, 18.0f, 1.0f, 100.0f);
    out.height = detail::season_clamp(po.height_m, 5.0f, 1.0f, 30.0f);
    return out;
}

// The same helpers for a component that may be missing: a null pointer is amount 0 and the inactive look.
inline f32 pollen_amount(const Pollen* po, const Season* season, f32 phase, f32 night_fade, f32 rain) {
    return po ? pollen_amount(*po, season, phase, night_fade, rain) : 0.0f;
}
inline PollenLook pollen_look(const Pollen* po, f32 amount, f32 wind, const math::vec2& wind_dir) {
    return po ? pollen_look(*po, amount, wind, wind_dir) : PollenLook{};
}

}  // namespace mce::kit
