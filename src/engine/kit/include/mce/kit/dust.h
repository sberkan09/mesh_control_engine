#pragma once
// Wind-blown dust as data (M-DUSTWIND step 25a). One optional Dust per scene (the first in ECS order is read).
// install_weather registers the component; there is no system and nothing is stored between ticks.
//
//   Dust     Authoring (all Save, clamped to the Range when read and never rewritten): `enabled` (1); `wind_start` (4,
//            0..40) and `wind_full` (12, 0..60) the scene wind speed in m/s where the dust starts to blow and reaches full
//            strength (an inverted or empty pair falls back to the defaults); `manual` (0, 0..1) a floor for the amount,
//            so a desert scene can show dust in a calm; `rate` (40, 0..400) specks per second at full strength;
//            `box_xz_m` (14, 1..100) half extent of the spawn box around the camera; `height_m` (3, 1..20) height of the
//            box above the ground-level camera; `size_m` (0.05, 0.01..0.5); `lifetime_s` (2.5, 0.5..10); `speed` (6,
//            0..30) the sideways speed in m/s at full wind; `wind_follow` (0.8, 0..2) how much of that speed the specks
//            take; `color` (0.78 0.66 0.46, linear); `max_particles` (1024, 16..4096); `seed` (1); `strength` (1, 0..1)
//            scales every effect, 0 = off exactly.
//
// The amount is max(manual, smoothstep(wind_start, wind_full, wind)) * (1 - rain) times strength, a pure function of the
// authoring, the gusted scene wind and the rain. The look turns the amount into the emitter description the extract (25b)
// adds as a local alpha-blended (not additive) render::ParticleEmitter: a fast, almost flat stream along the wind
// direction. No rand, no pow/exp/trig; polynomial smoothstep in a fixed order. `enabled` 0, `strength` 0 and a missing
// component (a null pointer) give an amount of 0 and the inactive zero look as exact values, which the bit-identical render
// gates rely on.

#include <algorithm>
#include <cmath>

#include "mce/kit/season.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct Dust {
    f32 wind_start = 4.0f;
    f32 wind_full = 12.0f;
    f32 manual = 0.0f;
    f32 rate = 40.0f;
    f32 box_xz_m = 14.0f;
    f32 height_m = 3.0f;
    f32 size_m = 0.05f;
    f32 lifetime_s = 2.5f;
    f32 speed = 6.0f;
    f32 wind_follow = 0.8f;
    f32 strength = 1.0f;
    math::vec3 color{0.78f, 0.66f, 0.46f};
    u32 max_particles = 1024;
    u32 seed = 1;
    u8 enabled = 1;
    u8 pad_[3] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(Dust, (enabled, Save, Range{0, 1}, Help{"0: the dust is off"}),
              (wind_start, Save, Range{0, 40}, Help{"scene wind speed (m/s) where dust starts to blow"}),
              (wind_full, Save, Range{0, 60}, Help{"scene wind speed (m/s) where dust is at full strength"}),
              (manual, Save, Range{0, 1}, Help{"floor for the amount; shows dust in a calm"}),
              (rate, Save, Range{0, 400}, Help{"specks per second at full strength"}),
              (box_xz_m, Save, Range{1, 100}, Help{"half extent of the spawn box around the camera, metres"}),
              (height_m, Save, Range{1, 20}, Help{"height of the spawn box above the camera, metres"}),
              (size_m, Save, Range{0.01f, 0.5f}, Help{"speck sprite size, metres"}),
              (lifetime_s, Save, Range{0.5f, 10}, Help{"speck lifetime, seconds"}),
              (speed, Save, Range{0, 30}, Help{"sideways speed at full wind, metres per second"}),
              (wind_follow, Save, Range{0, 2}, Help{"how much of the wind speed the specks take"}),
              (color, Save, Help{"speck colour, linear"}),
              (max_particles, Save, Range{16, 4096}, Help{"particle budget of the dust emitter"}),
              (seed, Save, Help{"random seed of the dust emitter"}),
              (strength, Save, Range{0, 1}, Help{"scales every effect; 0 is off exactly"}))

// The emitter description for an amount. At amount 0 (and for a disabled or zero-strength component) it is `active =
// false` and every other field zero. The extract copies the fields into a render::ParticleEmitter: `direction` is a unit
// vector along the wind (a slight downward tilt keeps it defined in a calm), `gravity` and `drag` are small,
// `color_start` fades out to `color_end` (same rgb, alpha 0).
struct DustLook {
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

inline f32 dust_unit(f32 v, f32 def) { return season_clamp(v, def, 0.0f, 1.0f); }

// The wind speed (m/s) the specks follow at most, so a storm cannot blow them out of the box.
inline constexpr f32 kDustMaxWind = 20.0f;
// The small downward tilt of the stream and the weak gravity and drag of a speck.
inline constexpr f32 kDustTilt = 0.08f;
inline constexpr f32 kDustGravity = 0.4f;
inline constexpr f32 kDustDrag = 0.3f;

}  // namespace detail

// The dust amount 0..1. `wind` is the scene wind speed in m/s (effective wind with the gust sway), `rain` the 0..1 rain
// intensity. NaN wind counts as calm, so only the manual floor remains. Exactly 0 for `enabled` 0 and `strength` 0.
inline f32 dust_amount(const Dust& d, f32 wind, f32 rain) {
    if (d.enabled == 0) return 0.0f;
    const f32 st = detail::dust_unit(d.strength, 1.0f);
    if (!(st > 0.0f)) return 0.0f;
    f32 lo = detail::season_clamp(d.wind_start, 4.0f, 0.0f, 40.0f);
    f32 hi = detail::season_clamp(d.wind_full, 12.0f, 0.0f, 60.0f);
    if (!(lo < hi)) {
        lo = 4.0f;
        hi = 12.0f;
    }
    const f32 w = std::isnan(wind) ? 0.0f : wind;
    const f32 blow = detail::season_smooth(lo, hi, w);
    const f32 manual = detail::dust_unit(d.manual, 0.0f);
    const f32 dry = 1.0f - detail::dust_unit(rain, 0.0f);
    return std::clamp(std::max(manual, blow) * dry * st, 0.0f, 1.0f);
}

// The emitter look at an amount (clamped to 0..1, NaN is 0), the scene wind speed and its horizontal direction (x, z).
inline DustLook dust_look(const Dust& d, f32 amount, f32 wind, const math::vec2& wind_dir) {
    DustLook out;
    if (d.enabled == 0 || !(detail::dust_unit(d.strength, 1.0f) > 0.0f)) return out;
    const f32 a = detail::dust_unit(amount, 0.0f);
    if (!(a > 0.0f)) return out;
    out.active = true;
    out.rate = detail::season_clamp(d.rate, 40.0f, 0.0f, 400.0f) * a;
    const f32 sp = detail::season_clamp(d.speed, 6.0f, 0.0f, 30.0f);
    const f32 follow = detail::season_clamp(d.wind_follow, 0.8f, 0.0f, 2.0f);
    f32 full = detail::season_clamp(d.wind_full, 12.0f, 0.0f, 60.0f);
    if (!(full > 0.0f)) full = 12.0f;
    const f32 wv = std::isnan(wind) ? 0.0f : std::clamp(wind, 0.0f, detail::kDustMaxWind);
    // 0.5 in a calm up to 1 at full wind: a manual floor still blows a little.
    const f32 gain = 0.5f + 0.5f * std::min(wv / full, 1.0f);
    const f32 s = sp * follow * gain;
    const f32 dx = std::isnan(wind_dir.x) ? 0.0f : wind_dir.x;
    const f32 dz = std::isnan(wind_dir.y) ? 0.0f : wind_dir.y;
    out.direction = math::normalize(math::vec3{dx, -detail::kDustTilt, dz});
    out.speed_min = 0.6f * s;
    out.speed_max = s;
    out.spread = 0.25f;
    out.gravity = math::vec3{0.0f, -detail::kDustGravity, 0.0f};
    out.drag = detail::kDustDrag;
    out.size = detail::season_clamp(d.size_m, 0.05f, 0.01f, 0.5f);
    const f32 life = detail::season_clamp(d.lifetime_s, 2.5f, 0.5f, 10.0f);
    out.lifetime_min = 0.7f * life;
    out.lifetime_max = life;
    const math::vec3 rgb{detail::season_clamp(d.color.x, 0.78f, 0.0f, 1.0f), detail::season_clamp(d.color.y, 0.66f, 0.0f, 1.0f),
                         detail::season_clamp(d.color.z, 0.46f, 0.0f, 1.0f)};
    out.color_start = math::vec4{rgb.x, rgb.y, rgb.z, 0.6f * a};
    out.color_end = math::vec4{rgb.x, rgb.y, rgb.z, 0.0f};
    out.box = detail::season_clamp(d.box_xz_m, 14.0f, 1.0f, 100.0f);
    out.height = detail::season_clamp(d.height_m, 3.0f, 1.0f, 20.0f);
    return out;
}

// The same helpers for a component that may be missing: a null pointer is amount 0 and the inactive look.
inline f32 dust_amount(const Dust* d, f32 wind, f32 rain) { return d ? dust_amount(*d, wind, rain) : 0.0f; }
inline DustLook dust_look(const Dust* d, f32 amount, f32 wind, const math::vec2& wind_dir) {
    return d ? dust_look(*d, amount, wind, wind_dir) : DustLook{};
}

}  // namespace mce::kit
