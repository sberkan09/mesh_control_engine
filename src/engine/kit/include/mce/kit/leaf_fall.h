#pragma once
// Autumn leaves that drift around the camera as data (M-LEAFFALL step 21a). One optional LeafFall per scene (the first in
// ECS order is read). install_weather registers the component; there is no system and nothing is stored between ticks.
//
//   LeafFall  Authoring (all Save, clamped to the Range when read and never rewritten): `enabled` (1); `autumn_start`
//             (0.5) and `autumn_full` (0.6), `autumn_end_full` (0.65) and `autumn_end` (0.75), all 0..1, the year
//             phases where the leaves start to fall, reach full strength, start to thin and are gone (an inverted pair
//             is swapped, an empty one falls back to the defaults); `manual` (0, 0..1) a floor for the amount, so a
//             scene with no Season can still drop leaves; `rate` (24, 0..500) leaves per second at full strength;
//             `box_xz_m` (14, 1..100) half extent of the spawn box around the camera; `height_m` (7, 1..50) spawn
//             height above the camera; `size_m` (0.14, 0.02..1); `lifetime_s` (7, 1..30); `fall_speed` (0.9, 0..5)
//             the terminal fall speed in m/s; `wind_follow` (1, 0..2) how strongly the scene wind pushes the leaves
//             sideways; `leaf_color` (0.95 0.5 0.15, linear); `tint_follow` (1, 0..1) how far the colour follows the
//             season foliage tint; `max_particles` (1024, 64..8192); `seed` (1); `strength` (1, 0..1) scales every
//             effect, 0 = off exactly.
//
// The amount is max(manual, window(phase)) times strength, a pure function of the authoring, the Season and the phase
// (season_phase of the WorldClock day and hour). The window is a trapezoid of smoothsteps, rising over
// autumn_start..autumn_full and falling over autumn_end_full..autumn_end. The look turns the amount into the emitter
// description the extract (21b) adds as a local render::ParticleEmitter. No rand, no pow/exp/trig; polynomial smoothstep
// in a fixed order. `enabled` 0, `strength` 0 and a missing component (a null pointer) give an amount of 0 and the
// inactive zero look as exact values, which the bit-identical render gates rely on.

#include <algorithm>
#include <cmath>

#include "mce/kit/season.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct LeafFall {
    f32 autumn_start = 0.5f;
    f32 autumn_full = 0.6f;
    f32 autumn_end_full = 0.65f;
    f32 autumn_end = 0.75f;
    f32 manual = 0.0f;
    f32 rate = 24.0f;
    f32 box_xz_m = 14.0f;
    f32 height_m = 7.0f;
    f32 size_m = 0.14f;
    f32 lifetime_s = 7.0f;
    f32 fall_speed = 0.9f;
    f32 wind_follow = 1.0f;
    f32 tint_follow = 1.0f;
    f32 strength = 1.0f;
    math::vec3 leaf_color{0.95f, 0.5f, 0.15f};
    u32 max_particles = 1024;
    u32 seed = 1;
    u8 enabled = 1;
    u8 pad_[3] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(LeafFall, (enabled, Save, Range{0, 1}, Help{"0: the leaves are off"}),
              (autumn_start, Save, Range{0, 1}, Help{"year phase where leaves start to fall"}),
              (autumn_full, Save, Range{0, 1}, Help{"year phase where the fall reaches full strength"}),
              (autumn_end_full, Save, Range{0, 1}, Help{"year phase where the fall starts to thin"}),
              (autumn_end, Save, Range{0, 1}, Help{"year phase where the last leaf has fallen"}),
              (manual, Save, Range{0, 1}, Help{"floor for the amount; drops leaves with no Season"}),
              (rate, Save, Range{0, 500}, Help{"leaves per second at full strength"}),
              (box_xz_m, Save, Range{1, 100}, Help{"half extent of the spawn box around the camera, metres"}),
              (height_m, Save, Range{1, 50}, Help{"spawn height above the camera, metres"}),
              (size_m, Save, Range{0.02f, 1}, Help{"leaf sprite size, metres"}),
              (lifetime_s, Save, Range{1, 30}, Help{"leaf lifetime, seconds"}),
              (fall_speed, Save, Range{0, 5}, Help{"terminal fall speed, metres per second"}),
              (wind_follow, Save, Range{0, 2}, Help{"how strongly the scene wind pushes the leaves sideways"}),
              (leaf_color, Save, Help{"leaf colour, linear"}),
              (tint_follow, Save, Range{0, 1}, Help{"how far the colour follows the season foliage tint"}),
              (max_particles, Save, Range{64, 8192}, Help{"particle budget of the leaf emitter"}),
              (seed, Save, Help{"random seed of the leaf emitter"}),
              (strength, Save, Range{0, 1}, Help{"scales every effect; 0 is off exactly"}))

// The emitter description for an amount. At amount 0 (and for a disabled or zero-strength component) it is `active =
// false` and every other field zero. The extract copies the fields into a render::ParticleEmitter: `direction` is a unit
// vector along the drift (straight down at wind 0), `gravity` and `drag` give a terminal fall speed of `gravity.y / drag`
// = fall_speed, `color_start` fades out to `color_end` (same rgb, alpha 0).
struct LeafFallLook {
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

inline f32 leaf_fall_unit(f32 v, f32 def) { return season_clamp(v, def, 0.0f, 1.0f); }

// Two thresholds read as a rising or falling pair: clamped to 0..1, an inverted pair swapped, an empty one the defaults.
inline void leaf_fall_pair(f32 a, f32 b, f32 def_a, f32 def_b, f32& lo, f32& hi) {
    lo = leaf_fall_unit(a, def_a);
    hi = leaf_fall_unit(b, def_b);
    if (lo > hi) std::swap(lo, hi);
    if (!(lo < hi)) {
        lo = def_a;
        hi = def_b;
    }
}

// The leaf drift cap (m/s of wind a leaf follows), so a storm cannot blow the leaves out of the box.
inline constexpr f32 kLeafFallMaxWind = 6.0f;
// Gravity of the slow fall, m/s^2; drag = gravity / fall_speed gives the terminal speed.
inline constexpr f32 kLeafFallGravity = 2.0f;
// The slowest terminal speed the look divides by.
inline constexpr f32 kLeafFallMinSpeed = 0.05f;

}  // namespace detail

// The leaf amount 0..1 at a year phase. A null or disabled Season (or one with strength 0) has no window, so the amount
// is the manual floor. Exactly 0 for `enabled` 0 and `strength` 0.
inline f32 leaf_fall_amount(const LeafFall& lf, const Season* season, f32 phase) {
    if (lf.enabled == 0) return 0.0f;
    const f32 st = detail::leaf_fall_unit(lf.strength, 1.0f);
    if (!(st > 0.0f)) return 0.0f;
    f32 w = 0.0f;
    if (season != nullptr && season->enabled != 0) {
        f32 s0, s1, e0, e1;
        detail::leaf_fall_pair(lf.autumn_start, lf.autumn_full, 0.5f, 0.6f, s0, s1);
        detail::leaf_fall_pair(lf.autumn_end_full, lf.autumn_end, 0.65f, 0.75f, e0, e1);
        const f32 p = detail::season_wrap(phase);
        w = detail::season_smooth(s0, s1, p) * (1.0f - detail::season_smooth(e0, e1, p)) * detail::season_strength(*season);
    }
    const f32 manual = detail::leaf_fall_unit(lf.manual, 0.0f);
    const f32 a = std::max(manual, w);
    return std::clamp(a * st, 0.0f, 1.0f);
}

// The emitter look at an amount (clamped to 0..1, NaN is 0), the scene wind speed, its horizontal direction (x, z) and the
// season foliage tint at the phase.
inline LeafFallLook leaf_fall_look(const LeafFall& lf, f32 amount, f32 wind, const math::vec2& wind_dir,
                                   const math::vec3& foliage_tint) {
    LeafFallLook out;
    if (lf.enabled == 0 || !(detail::leaf_fall_unit(lf.strength, 1.0f) > 0.0f)) return out;
    const f32 a = detail::leaf_fall_unit(amount, 0.0f);
    if (!(a > 0.0f)) return out;
    out.active = true;
    out.rate = detail::season_clamp(lf.rate, 24.0f, 0.0f, 500.0f) * a;
    const f32 fs = std::max(detail::season_clamp(lf.fall_speed, 0.9f, 0.0f, 5.0f), detail::kLeafFallMinSpeed);
    const f32 follow = detail::season_clamp(lf.wind_follow, 1.0f, 0.0f, 2.0f);
    const f32 wv = std::isnan(wind) ? 0.0f : std::clamp(wind, 0.0f, detail::kLeafFallMaxWind);
    const f32 w = wv * follow;
    const f32 dx = std::isnan(wind_dir.x) ? 0.0f : wind_dir.x;
    const f32 dz = std::isnan(wind_dir.y) ? 0.0f : wind_dir.y;
    out.direction = math::normalize(math::vec3{dx * w, -1.0f, dz * w});
    out.speed_min = 0.5f * fs + 0.25f * w;
    out.speed_max = 1.0f * fs + 0.5f * w;
    out.spread = 0.6f;
    out.gravity = math::vec3{0.0f, -detail::kLeafFallGravity, 0.0f};
    out.drag = detail::kLeafFallGravity / fs;
    out.size = detail::season_clamp(lf.size_m, 0.14f, 0.02f, 1.0f);
    const f32 life = detail::season_clamp(lf.lifetime_s, 7.0f, 1.0f, 30.0f);
    out.lifetime_min = 0.7f * life;
    out.lifetime_max = life;
    const f32 tf = detail::season_clamp(lf.tint_follow, 1.0f, 0.0f, 1.0f);
    const math::vec3 base{detail::season_clamp(lf.leaf_color.x, 0.95f, 0.0f, 1.0f),
                          detail::season_clamp(lf.leaf_color.y, 0.5f, 0.0f, 1.0f),
                          detail::season_clamp(lf.leaf_color.z, 0.15f, 0.0f, 1.0f)};
    const math::vec3 tint = detail::season_mix(math::vec3{1.0f, 1.0f, 1.0f}, detail::season_tint_of(foliage_tint, math::vec3{1.0f, 1.0f, 1.0f}), tf);
    const math::vec3 rgb{base.x * tint.x, base.y * tint.y, base.z * tint.z};
    out.color_start = math::vec4{rgb.x, rgb.y, rgb.z, 0.9f * a};
    out.color_end = math::vec4{rgb.x, rgb.y, rgb.z, 0.0f};
    out.box = detail::season_clamp(lf.box_xz_m, 14.0f, 1.0f, 100.0f);
    out.height = detail::season_clamp(lf.height_m, 7.0f, 1.0f, 50.0f);
    return out;
}

// The same helpers for a component that may be missing: a null pointer is amount 0 and the inactive look.
inline f32 leaf_fall_amount(const LeafFall* lf, const Season* season, f32 phase) {
    return lf ? leaf_fall_amount(*lf, season, phase) : 0.0f;
}
inline LeafFallLook leaf_fall_look(const LeafFall* lf, f32 amount, f32 wind, const math::vec2& wind_dir,
                                   const math::vec3& foliage_tint) {
    return lf ? leaf_fall_look(*lf, amount, wind, wind_dir, foliage_tint) : LeafFallLook{};
}

}  // namespace mce::kit
