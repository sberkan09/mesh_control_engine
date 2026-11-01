#pragma once
// Fireflies on warm summer nights as data (M-FIREFLIES step 23a). One optional Firefly per scene (the first in ECS order
// is read). install_weather registers the component; there is no system and nothing is stored between ticks.
//
//   Firefly  Authoring (all Save, clamped to the Range when read and never rewritten): `enabled` (1); `summer_start`
//            (0.2) and `summer_full` (0.3), `summer_end_full` (0.45) and `summer_end` (0.55), all 0..1, the year phases
//            where the fireflies start to show, reach full strength, start to thin and are gone (the window is centred
//            on the summer centre 0.375; an inverted pair is swapped, an empty one falls back to the defaults);
//            `manual` (0, 0..1) a floor for the amount, so a scene with no Season can still show fireflies; `night_only`
//            (1, 0..1) 1: scaled by the night gate, 0: shows by day too; `min_darkness` (0.6, 0..1) the night fade below
//            which none show (a smoothstep from it to 1, as Meteor); `rate` (18, 0..200) fireflies per second at full
//            strength; `box_xz_m` (14, 1..100) half extent of the spawn box around the camera; `height_m` (3, 1..20)
//            spawn height above the ground-level camera; `size_m` (0.08, 0.02..0.5); `lifetime_s` (6, 1..30);
//            `drift_speed` (0.25, 0..2) the slow hover speed in m/s; `color` (0.8 1.0 0.35, linear); `max_particles`
//            (256, 16..4096); `seed` (1); `strength` (1, 0..1) scales every effect, 0 = off exactly.
//
// The amount is max(manual, window(phase)) * night_gate * (1 - rain) times strength, a pure function of the authoring,
// the Season, the phase, the night fade and the weather. The window is a trapezoid of smoothsteps, rising over
// summer_start..summer_full and falling over summer_end_full..summer_end. The look turns the amount into the emitter
// description the extract (23b) adds as a local additive render::ParticleEmitter. No rand, no pow/exp/trig; polynomial
// smoothsteps in a fixed order. `enabled` 0, `strength` 0 and a missing component (a null pointer) give an amount of 0
// and the inactive zero look as exact values, which the bit-identical render gates rely on.

#include <algorithm>
#include <cmath>

#include "mce/kit/meteor.h"
#include "mce/kit/season.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct Firefly {
    f32 summer_start = 0.2f;
    f32 summer_full = 0.3f;
    f32 summer_end_full = 0.45f;
    f32 summer_end = 0.55f;
    f32 manual = 0.0f;
    f32 min_darkness = 0.6f;
    f32 rate = 18.0f;
    f32 box_xz_m = 14.0f;
    f32 height_m = 3.0f;
    f32 size_m = 0.08f;
    f32 lifetime_s = 6.0f;
    f32 drift_speed = 0.25f;
    f32 strength = 1.0f;
    math::vec3 color{0.8f, 1.0f, 0.35f};
    u32 max_particles = 256;
    u32 seed = 1;
    u8 enabled = 1;
    u8 night_only = 1;
    u8 pad_[2] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(Firefly, (enabled, Save, Range{0, 1}, Help{"0: the fireflies are off"}),
              (summer_start, Save, Range{0, 1}, Help{"year phase where fireflies start to show"}),
              (summer_full, Save, Range{0, 1}, Help{"year phase where they reach full strength"}),
              (summer_end_full, Save, Range{0, 1}, Help{"year phase where they start to thin"}),
              (summer_end, Save, Range{0, 1}, Help{"year phase where the last firefly is gone"}),
              (manual, Save, Range{0, 1}, Help{"floor for the amount; shows fireflies with no Season"}),
              (night_only, Save, Range{0, 1}, Help{"1: only at night; 0: by day too"}),
              (min_darkness, Save, Range{0, 1}, Help{"night fade below which no firefly shows"}),
              (rate, Save, Range{0, 200}, Help{"fireflies per second at full strength"}),
              (box_xz_m, Save, Range{1, 100}, Help{"half extent of the spawn box around the camera, metres"}),
              (height_m, Save, Range{1, 20}, Help{"spawn height above the camera, metres"}),
              (size_m, Save, Range{0.02f, 0.5f}, Help{"firefly sprite size, metres"}),
              (lifetime_s, Save, Range{1, 30}, Help{"firefly lifetime, seconds"}),
              (drift_speed, Save, Range{0, 2}, Help{"slow hover drift speed, metres per second"}),
              (color, Save, Help{"glow colour, linear"}),
              (max_particles, Save, Range{16, 4096}, Help{"particle budget of the firefly emitter"}),
              (seed, Save, Help{"random seed of the firefly emitter"}),
              (strength, Save, Range{0, 1}, Help{"scales every effect; 0 is off exactly"}))

// The emitter description for an amount. At amount 0 (and for a disabled or zero-strength component) it is `active =
// false` and every other field zero. The extract copies the fields into a render::ParticleEmitter: `direction` is a unit
// vector (up), `spread` makes the hover drift omnidirectional, `gravity` is tiny and `drag` high, `color_start` fades
// out to `color_end` (same rgb, alpha 0).
struct FireflyLook {
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

inline f32 firefly_unit(f32 v, f32 def) { return season_clamp(v, def, 0.0f, 1.0f); }

// Two thresholds read as a rising or falling pair: clamped to 0..1, an inverted pair swapped, an empty one the defaults.
inline void firefly_pair(f32 a, f32 b, f32 def_a, f32 def_b, f32& lo, f32& hi) {
    lo = firefly_unit(a, def_a);
    hi = firefly_unit(b, def_b);
    if (lo > hi) std::swap(lo, hi);
    if (!(lo < hi)) {
        lo = def_a;
        hi = def_b;
    }
}

// Tiny upward-neutral gravity (m/s^2) and the high drag that make the hover a slow drift.
inline constexpr f32 kFireflyGravity = 0.02f;
inline constexpr f32 kFireflyDrag = 1.5f;

}  // namespace detail

// The firefly amount 0..1. `phase` is the year phase, `night_fade` the 0..1 night fade, `cloud_cover` is accepted for the
// extract's symmetry with the meteor block and does not change the amount, `rain` the 0..1 rain intensity. A null or
// disabled Season (or one with strength 0) has no window, so the amount is the manual floor. Exactly 0 for `enabled` 0
// and `strength` 0.
inline f32 firefly_amount(const Firefly& ff, const Season* season, f32 phase, f32 night_fade, f32 cloud_cover,
                          f32 rain) {
    (void)cloud_cover;
    if (ff.enabled == 0) return 0.0f;
    const f32 st = detail::firefly_unit(ff.strength, 1.0f);
    if (!(st > 0.0f)) return 0.0f;
    f32 w = 0.0f;
    if (season != nullptr && season->enabled != 0) {
        f32 s0, s1, e0, e1;
        detail::firefly_pair(ff.summer_start, ff.summer_full, 0.2f, 0.3f, s0, s1);
        detail::firefly_pair(ff.summer_end_full, ff.summer_end, 0.45f, 0.55f, e0, e1);
        const f32 p = detail::season_wrap(phase);
        w = detail::season_smooth(s0, s1, p) * (1.0f - detail::season_smooth(e0, e1, p)) * detail::season_strength(*season);
    }
    const f32 manual = detail::firefly_unit(ff.manual, 0.0f);
    const f32 base = std::max(manual, w);
    const f32 gate = ff.night_only == 0
                         ? 1.0f
                         : detail::meteor_smooth(detail::meteor_clamp(ff.min_darkness, 0.6f, 0.0f, 1.0f), 1.0f,
                                                 detail::meteor_clamp(night_fade, 0.0f, 0.0f, 1.0f));
    const f32 dry = 1.0f - detail::meteor_clamp(rain, 0.0f, 0.0f, 1.0f);
    return std::clamp(base * gate * dry * st, 0.0f, 1.0f);
}

// The emitter look at an amount (clamped to 0..1, NaN is 0).
inline FireflyLook firefly_look(const Firefly& ff, f32 amount) {
    FireflyLook out;
    if (ff.enabled == 0 || !(detail::firefly_unit(ff.strength, 1.0f) > 0.0f)) return out;
    const f32 a = detail::firefly_unit(amount, 0.0f);
    if (!(a > 0.0f)) return out;
    out.active = true;
    out.rate = detail::season_clamp(ff.rate, 18.0f, 0.0f, 200.0f) * a;
    const f32 ds = detail::season_clamp(ff.drift_speed, 0.25f, 0.0f, 2.0f);
    out.direction = math::vec3{0.0f, 1.0f, 0.0f};
    out.speed_min = 0.5f * ds;
    out.speed_max = ds;
    out.spread = 1.0f;
    out.gravity = math::vec3{0.0f, -detail::kFireflyGravity, 0.0f};
    out.drag = detail::kFireflyDrag;
    out.size = detail::season_clamp(ff.size_m, 0.08f, 0.02f, 0.5f);
    const f32 life = detail::season_clamp(ff.lifetime_s, 6.0f, 1.0f, 30.0f);
    out.lifetime_min = 0.7f * life;
    out.lifetime_max = life;
    const math::vec3 rgb{detail::season_clamp(ff.color.x, 0.8f, 0.0f, 1.0f), detail::season_clamp(ff.color.y, 1.0f, 0.0f, 1.0f),
                         detail::season_clamp(ff.color.z, 0.35f, 0.0f, 1.0f)};
    out.color_start = math::vec4{rgb.x, rgb.y, rgb.z, 0.9f * a};
    out.color_end = math::vec4{rgb.x, rgb.y, rgb.z, 0.0f};
    out.box = detail::season_clamp(ff.box_xz_m, 14.0f, 1.0f, 100.0f);
    out.height = detail::season_clamp(ff.height_m, 3.0f, 1.0f, 20.0f);
    return out;
}

// The same helpers for a component that may be missing: a null pointer is amount 0 and the inactive look.
inline f32 firefly_amount(const Firefly* ff, const Season* season, f32 phase, f32 night_fade, f32 cloud_cover, f32 rain) {
    return ff ? firefly_amount(*ff, season, phase, night_fade, cloud_cover, rain) : 0.0f;
}
inline FireflyLook firefly_look(const Firefly* ff, f32 amount) { return ff ? firefly_look(*ff, amount) : FireflyLook{}; }

}  // namespace mce::kit
