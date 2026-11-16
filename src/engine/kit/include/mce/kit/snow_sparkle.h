#pragma once
// Snow sparkle as data (M-SNOWSPARKLE step 31a). One optional SnowSparkle per scene (the first in ECS order is read).
// install_weather registers the component; there is no system and nothing is stored between ticks.
//
//   SnowSparkle    Authoring (all Save, clamped to the Range when read and never rewritten): `enabled` (1); `cover_low`
//              (0.2, 0..1) and `cover_full` (0.8, 0..1) the settled snow cover below which there is no sparkle and above
//              which the cover gate is fully open (a smoothstep between; an inverted or empty pair falls back to the
//              defaults); `sun_low` (0.05, 0..1) and `sun_high` (0.6, 0..1) the sun-elevation band (0 = horizon, 1 =
//              zenith): the gate rises from 0 at the horizon to 1 at sun_low, stays open up to a quarter of the way to
//              sun_high and falls to 0 at sun_high, so low sun glitters most (an inverted or empty pair falls back to the
//              defaults); `manual` (0, 0..1) a floor that ignores the cover and sun gates (not the cloud and rain); `rate`
//              (24, 0..200) specks per second at full strength; `box_xz_m` (14, 1..100) half extent of the flat spawn box
//              around the camera; `height_m` (1.2, 0.2..10) height of the box above the ground-level camera; `size_m`
//              (0.03, 0.005..0.5); `lifetime_s` (0.6, 0.1..5); `color` (1 1 1, linear); `max_particles` (96, 8..512);
//              `seed` (1); `strength` (1, 0..1) scales every effect, 0 = off exactly.
//
// The amount is max(manual, cover_gate * sun_gate) * (1 - cloud) * (1 - rain) times strength. The cover gate is
// smoothstep(cover_low, cover_full, cover); the sun gate is the trapezoid above. The look turns the amount into the
// emitter description the extract (31b) adds as a local additive render::ParticleEmitter: very small, short-lived white
// specks that barely move. Polynomials only, fixed order, no rand. `enabled` 0, `strength` 0 and a missing component (a null
// pointer) give an amount of 0 and the inactive zero look as exact values, which the bit-identical render gates rely on.

#include <algorithm>
#include <cmath>

#include "mce/kit/season.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct SnowSparkle {
    f32 cover_low = 0.2f;
    f32 cover_full = 0.8f;
    f32 sun_low = 0.05f;
    f32 sun_high = 0.6f;
    f32 manual = 0.0f;
    f32 rate = 24.0f;
    f32 box_xz_m = 14.0f;
    f32 height_m = 1.2f;
    f32 size_m = 0.03f;
    f32 lifetime_s = 0.6f;
    f32 strength = 1.0f;
    math::vec3 color{1.0f, 1.0f, 1.0f};
    u32 max_particles = 96;
    u32 seed = 1;
    u8 enabled = 1;
    u8 pad_[3] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(SnowSparkle, (enabled, Save, Range{0, 1}, Help{"0: the snow sparkle is off"}),
              (cover_low, Save, Range{0, 1}, Help{"snow cover below which there is no sparkle"}),
              (cover_full, Save, Range{0, 1}, Help{"snow cover above which the cover gate is fully open"}),
              (sun_low, Save, Range{0, 1}, Help{"sun elevation at which the sparkle reaches full strength"}),
              (sun_high, Save, Range{0, 1}, Help{"sun elevation above which there is no sparkle"}),
              (manual, Save, Range{0, 1}, Help{"floor that ignores the cover and sun gates"}),
              (rate, Save, Range{0, 200}, Help{"specks per second at full strength"}),
              (box_xz_m, Save, Range{1, 100}, Help{"half extent of the spawn box around the camera, metres"}),
              (height_m, Save, Range{0.2f, 10}, Help{"height of the spawn box above the camera, metres"}),
              (size_m, Save, Range{0.005f, 0.5f}, Help{"speck size, metres"}),
              (lifetime_s, Save, Range{0.1f, 5}, Help{"speck lifetime, seconds"}),
              (color, Save, Help{"speck colour, linear"}),
              (max_particles, Save, Range{8, 512}, Help{"particle budget of the sparkle emitter"}),
              (seed, Save, Help{"random seed of the sparkle emitter"}),
              (strength, Save, Range{0, 1}, Help{"scales every effect; 0 is off exactly"}))

// The emitter description for an amount. At amount 0 (and for a disabled or zero-strength component) it is `active =
// false` and every other field zero. The extract copies the fields into a render::ParticleEmitter: `direction` is up
// (0, 1, 0) with a wide `spread`, the speed and gravity are almost none, `additive` is true, `color_start` fades out to
// `color_end` (same rgb, alpha 0).
struct SnowSparkleLook {
    bool active = false;
    bool additive = false;
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

inline f32 snow_sparkle_unit(f32 v, f32 def) { return season_clamp(v, def, 0.0f, 1.0f); }

// A rising pair of thresholds clamped to 0..1; an inverted or empty pair falls back to the defaults.
inline void snow_sparkle_pair(f32 a, f32 b, f32 def_a, f32 def_b, f32& out_a, f32& out_b) {
    out_a = season_clamp(a, def_a, 0.0f, 1.0f);
    out_b = season_clamp(b, def_b, 0.0f, 1.0f);
    if (!(out_a < out_b)) {
        out_a = def_a;
        out_b = def_b;
    }
}

// Share of the way from sun_low to sun_high at which the sun gate starts to fall (the plateau end).
inline constexpr f32 kSnowSparklePlateau = 0.25f;

// Drag of the almost still specks, 1/s.
inline constexpr f32 kSnowSparkleDrag = 1.0f;

}  // namespace detail

// The sparkle amount 0..1. `cover` is the settled snow cover 0..1, `sun_elev` the sun elevation 0..1, `cloud` the 0..1 cloud
// cover, `rain` the 0..1 rain intensity. Exactly 0 for `enabled` 0 and `strength` 0.
inline f32 snow_sparkle_amount(const SnowSparkle& s, f32 cover, f32 sun_elev, f32 cloud, f32 rain) {
    if (s.enabled == 0) return 0.0f;
    const f32 st = detail::snow_sparkle_unit(s.strength, 1.0f);
    if (!(st > 0.0f)) return 0.0f;
    f32 clo, cfull;
    detail::snow_sparkle_pair(s.cover_low, s.cover_full, 0.2f, 0.8f, clo, cfull);
    const f32 cover_gate = detail::season_smooth(clo, cfull, detail::snow_sparkle_unit(cover, 0.0f));
    f32 slo, shigh;
    detail::snow_sparkle_pair(s.sun_low, s.sun_high, 0.05f, 0.6f, slo, shigh);
    const f32 e = detail::snow_sparkle_unit(sun_elev, 0.0f);
    const f32 sun_gate = detail::season_smooth(0.0f, slo, e) *
                         (1.0f - detail::season_smooth(slo + detail::kSnowSparklePlateau * (shigh - slo), shigh, e));
    const f32 manual = detail::snow_sparkle_unit(s.manual, 0.0f);
    const f32 base = std::max(manual, cover_gate * sun_gate);
    const f32 clear = 1.0f - detail::snow_sparkle_unit(cloud, 0.0f);
    const f32 dry = 1.0f - detail::snow_sparkle_unit(rain, 0.0f);
    return std::clamp(base * clear * dry * st, 0.0f, 1.0f);
}

// The emitter look at an amount (clamped to 0..1, NaN is 0).
inline SnowSparkleLook snow_sparkle_look(const SnowSparkle& s, f32 amount) {
    SnowSparkleLook out;
    if (s.enabled == 0 || !(detail::snow_sparkle_unit(s.strength, 1.0f) > 0.0f)) return out;
    const f32 a = detail::snow_sparkle_unit(amount, 0.0f);
    if (!(a > 0.0f)) return out;
    out.active = true;
    out.additive = true;
    out.rate = detail::season_clamp(s.rate, 24.0f, 0.0f, 200.0f) * a;
    out.speed_min = 0.0f;
    out.speed_max = 0.05f;
    out.direction = math::vec3{0.0f, 1.0f, 0.0f};
    out.spread = 1.0f;
    out.gravity = math::vec3{0.0f, 0.0f, 0.0f};
    out.drag = detail::kSnowSparkleDrag;
    out.size = detail::season_clamp(s.size_m, 0.03f, 0.005f, 0.5f);
    const f32 life = detail::season_clamp(s.lifetime_s, 0.6f, 0.1f, 5.0f);
    out.lifetime_min = 0.5f * life;
    out.lifetime_max = life;
    const math::vec3 rgb{detail::season_clamp(s.color.x, 1.0f, 0.0f, 1.0f), detail::season_clamp(s.color.y, 1.0f, 0.0f, 1.0f),
                         detail::season_clamp(s.color.z, 1.0f, 0.0f, 1.0f)};
    out.color_start = math::vec4{rgb.x, rgb.y, rgb.z, a};
    out.color_end = math::vec4{rgb.x, rgb.y, rgb.z, 0.0f};
    out.box = detail::season_clamp(s.box_xz_m, 14.0f, 1.0f, 100.0f);
    out.height = detail::season_clamp(s.height_m, 1.2f, 0.2f, 10.0f);
    return out;
}

// The same helpers for a component that may be missing: a null pointer is amount 0 and the inactive look.
inline f32 snow_sparkle_amount(const SnowSparkle* s, f32 cover, f32 sun_elev, f32 cloud, f32 rain) {
    return s ? snow_sparkle_amount(*s, cover, sun_elev, cloud, rain) : 0.0f;
}
inline SnowSparkleLook snow_sparkle_look(const SnowSparkle* s, f32 amount) {
    return s ? snow_sparkle_look(*s, amount) : SnowSparkleLook{};
}

}  // namespace mce::kit
