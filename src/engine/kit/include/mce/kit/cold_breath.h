#pragma once
// Cold breath as data (M-COLDBREATH step 34a). One optional ColdBreath per scene (the first in ECS order is read).
// install_weather registers the component; there is no system and nothing is stored between ticks.
//
//   ColdBreath  Authoring (all Save, clamped to the Range when read and never rewritten): `enabled` (1); `window_full`
//               (0.08, 0..0.5) and `window_end` (0.16, 0..0.5) the half-widths, in season phase, of the winter trapezoid
//               around the winter centre 0.875 (full strength inside window_full, nothing beyond window_end, a smoothstep
//               between; an inverted or empty pair falls back to the defaults); `manual` (0, 0..1) a floor that ignores the
//               window and the snow cover (not the wind gate), so a scene can show breath all year; `cover_low` (0.15,
//               0..1) and `cover_full` (0.6, 0..1) the settled SnowCover.cover that also brings breath, smoothed (an
//               inverted or empty pair falls back to the defaults); `wind_calm` (0.5, 0..1) and `wind_still` (0.95, 0..1)
//               the wind below which the vapour stays and above which it is scattered (an inverted or empty pair falls back
//               to the defaults); `rate` (0.8, 0..10) puffs per second at full strength; `mouth_forward_m` (0.35, 0..2) and
//               `mouth_down_m` (0.25, 0..2) the offset of the emitter point in front of and below the camera; `size_m`
//               (0.12, 0.01..1) the puff size at birth and `grow` (2.5, 1..8) the factor it grows to at the end of life;
//               `lifetime_s` (1.2, 0.3..5); `speed` (0.35, 0..3) the forward speed in m/s; `lift` (0.15, 0..2) the upward
//               drift in m/s; `color` (0.9 0.93 0.97, linear, pale); `max_particles` (24, 4..128); `seed` (1);
//               `strength` (1, 0..1) scales every effect, 0 = off exactly.
//
// The amount is max(manual, window(phase) * season_strength, smooth(cover_low, cover_full, cover)) * calm_gate * strength.
// The window is a trapezoid in the unsigned circular distance of the phase from the winter centre (so it wraps across phase
// 0/1); the calm gate is 1 - smoothstep(wind_calm, wind_still, wind). No Season gives window 0 (the cover and the manual floor
// still count). The look turns the amount into the emitter description the extract (34b) adds as a local alpha-blended (not
// additive) render::ParticleEmitter: a few soft, pale, slowly growing puffs. The direction is not part of the look, the
// extract supplies the view's forward. Look fields: `active`; `rate` (puffs/s, authoring rate times the amount);
// `speed_min`/`speed_max` (0.6 * speed, speed); `lift` (m/s upward); `drag` (1/s); `size_start` (size_m) and `size_end`
// (size_m * grow); `lifetime_min`/`lifetime_max` (0.7 * life, life); `color_start` (rgb, alpha 0.35 * amount) and `color_end`
// (same rgb, alpha 0); `mouth_forward`/`mouth_down` (metres). Polynomials only, fixed order, no rand. `enabled` 0, `strength`
// 0 and a missing component (a null pointer) give an amount of 0 and the inactive zero look as exact values, which the
// bit-identical render gates rely on.

#include <algorithm>
#include <cmath>

#include "mce/kit/season.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct ColdBreath {
    f32 window_full = 0.08f;
    f32 window_end = 0.16f;
    f32 manual = 0.0f;
    f32 cover_low = 0.15f;
    f32 cover_full = 0.6f;
    f32 wind_calm = 0.5f;
    f32 wind_still = 0.95f;
    f32 rate = 0.8f;
    f32 mouth_forward_m = 0.35f;
    f32 mouth_down_m = 0.25f;
    f32 size_m = 0.12f;
    f32 grow = 2.5f;
    f32 lifetime_s = 1.2f;
    f32 speed = 0.35f;
    f32 lift = 0.15f;
    f32 strength = 1.0f;
    math::vec3 color{0.9f, 0.93f, 0.97f};
    u32 max_particles = 24;
    u32 seed = 1;
    u8 enabled = 1;
    u8 pad_[3] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(ColdBreath, (enabled, Save, Range{0, 1}, Help{"0: the cold breath is off"}),
              (window_full, Save, Range{0, 0.5f}, Help{"half-width in season phase of full strength around the winter centre"}),
              (window_end, Save, Range{0, 0.5f}, Help{"half-width in season phase beyond which the window gives no breath"}),
              (manual, Save, Range{0, 1}, Help{"floor that ignores the window and the snow cover, not the wind"}),
              (cover_low, Save, Range{0, 1}, Help{"settled snow cover below which the cover brings no breath"}),
              (cover_full, Save, Range{0, 1}, Help{"settled snow cover at which the cover brings full breath"}),
              (wind_calm, Save, Range{0, 1}, Help{"wind below which the vapour stays"}),
              (wind_still, Save, Range{0, 1}, Help{"wind above which the vapour is scattered"}),
              (rate, Save, Range{0, 10}, Help{"puffs per second at full strength"}),
              (mouth_forward_m, Save, Range{0, 2}, Help{"emitter offset in front of the camera, metres"}),
              (mouth_down_m, Save, Range{0, 2}, Help{"emitter offset below the camera, metres"}),
              (size_m, Save, Range{0.01f, 1}, Help{"puff size at birth, metres"}),
              (grow, Save, Range{1, 8}, Help{"factor the puff grows to at the end of life"}),
              (lifetime_s, Save, Range{0.3f, 5}, Help{"puff lifetime, seconds"}),
              (speed, Save, Range{0, 3}, Help{"forward speed, metres per second"}),
              (lift, Save, Range{0, 2}, Help{"upward drift, metres per second"}),
              (color, Save, Help{"puff colour, linear"}),
              (max_particles, Save, Range{4, 128}, Help{"particle budget of the breath emitter"}),
              (seed, Save, Help{"random seed of the breath emitter"}),
              (strength, Save, Range{0, 1}, Help{"scales every effect; 0 is off exactly"}))

// The emitter description for an amount. At amount 0 (and for a disabled or zero-strength component) it is `active =
// false` and every other field zero. The extract copies the fields into a render::ParticleEmitter and supplies the
// direction (the view's forward); the sprite is alpha-blended, not additive.
struct ColdBreathLook {
    bool active = false;
    f32 rate = 0.0f;
    f32 speed_min = 0.0f;
    f32 speed_max = 0.0f;
    f32 lift = 0.0f;
    f32 drag = 0.0f;
    f32 size_start = 0.0f;
    f32 size_end = 0.0f;
    f32 lifetime_min = 0.0f;
    f32 lifetime_max = 0.0f;
    math::vec4 color_start{0.0f, 0.0f, 0.0f, 0.0f};
    math::vec4 color_end{0.0f, 0.0f, 0.0f, 0.0f};
    f32 mouth_forward = 0.0f;
    f32 mouth_down = 0.0f;
};

namespace detail {

inline f32 cold_breath_unit(f32 v, f32 def) { return season_clamp(v, def, 0.0f, 1.0f); }

// The unsigned circular distance of a year phase (0..1) from the winter centre, in [0, 0.5].
inline f32 cold_breath_dist(f32 phase) {
    const f32 d = phase - kSeasonWinterCentre;
    return std::fabs(d - std::floor(d + 0.5f));
}

// A rising pair of thresholds clamped to lo..hi; an inverted or empty pair falls back to the defaults.
inline void cold_breath_pair(f32 a, f32 b, f32 def_a, f32 def_b, f32 lo, f32 hi, f32& out_a, f32& out_b) {
    out_a = season_clamp(a, def_a, lo, hi);
    out_b = season_clamp(b, def_b, lo, hi);
    if (!(out_a < out_b)) {
        out_a = def_a;
        out_b = def_b;
    }
}

// Drag of the drifting puffs, 1/s.
inline constexpr f32 kColdBreathDrag = 1.0f;

}  // namespace detail

// The breath amount 0..1. `phase` is the year phase, `cover` the settled SnowCover.cover 0..1, `wind` the 0..1 wind. A null
// or disabled Season has no window (the cover and the manual floor still count). Exactly 0 for `enabled` 0 and `strength` 0.
inline f32 cold_breath_amount(const ColdBreath& b, const Season* season, f32 phase, f32 cover, f32 wind) {
    if (b.enabled == 0) return 0.0f;
    const f32 st = detail::cold_breath_unit(b.strength, 1.0f);
    if (!(st > 0.0f)) return 0.0f;
    f32 w = 0.0f;
    if (season != nullptr && season->enabled != 0) {
        f32 full, end;
        detail::cold_breath_pair(b.window_full, b.window_end, 0.08f, 0.16f, 0.0f, 0.5f, full, end);
        w = (1.0f - detail::season_smooth(full, end, detail::cold_breath_dist(detail::season_wrap(phase)))) *
            detail::season_strength(*season);
    }
    f32 low, top;
    detail::cold_breath_pair(b.cover_low, b.cover_full, 0.15f, 0.6f, 0.0f, 1.0f, low, top);
    const f32 snow = detail::season_smooth(low, top, detail::cold_breath_unit(cover, 0.0f));
    const f32 manual = detail::cold_breath_unit(b.manual, 0.0f);
    const f32 base = std::max(manual, std::max(w, snow));
    f32 calm, still;
    detail::cold_breath_pair(b.wind_calm, b.wind_still, 0.5f, 0.95f, 0.0f, 1.0f, calm, still);
    const f32 calm_gate = 1.0f - detail::season_smooth(calm, still, detail::cold_breath_unit(wind, 0.0f));
    return std::clamp(base * calm_gate * st, 0.0f, 1.0f);
}

// The emitter look at an amount (clamped to 0..1, NaN is 0).
inline ColdBreathLook cold_breath_look(const ColdBreath& b, f32 amount) {
    ColdBreathLook out;
    if (b.enabled == 0 || !(detail::cold_breath_unit(b.strength, 1.0f) > 0.0f)) return out;
    const f32 a = detail::cold_breath_unit(amount, 0.0f);
    if (!(a > 0.0f)) return out;
    out.active = true;
    out.rate = detail::season_clamp(b.rate, 0.8f, 0.0f, 10.0f) * a;
    const f32 sp = detail::season_clamp(b.speed, 0.35f, 0.0f, 3.0f);
    out.speed_min = 0.6f * sp;
    out.speed_max = sp;
    out.lift = detail::season_clamp(b.lift, 0.15f, 0.0f, 2.0f);
    out.drag = detail::kColdBreathDrag;
    out.size_start = detail::season_clamp(b.size_m, 0.12f, 0.01f, 1.0f);
    out.size_end = out.size_start * detail::season_clamp(b.grow, 2.5f, 1.0f, 8.0f);
    const f32 life = detail::season_clamp(b.lifetime_s, 1.2f, 0.3f, 5.0f);
    out.lifetime_min = 0.7f * life;
    out.lifetime_max = life;
    const math::vec3 rgb{detail::season_clamp(b.color.x, 0.9f, 0.0f, 1.0f), detail::season_clamp(b.color.y, 0.93f, 0.0f, 1.0f),
                         detail::season_clamp(b.color.z, 0.97f, 0.0f, 1.0f)};
    out.color_start = math::vec4{rgb.x, rgb.y, rgb.z, 0.35f * a};
    out.color_end = math::vec4{rgb.x, rgb.y, rgb.z, 0.0f};
    out.mouth_forward = detail::season_clamp(b.mouth_forward_m, 0.35f, 0.0f, 2.0f);
    out.mouth_down = detail::season_clamp(b.mouth_down_m, 0.25f, 0.0f, 2.0f);
    return out;
}

// The same helpers for a component that may be missing: a null pointer is amount 0 and the inactive look.
inline f32 cold_breath_amount(const ColdBreath* b, const Season* season, f32 phase, f32 cover, f32 wind) {
    return b ? cold_breath_amount(*b, season, phase, cover, wind) : 0.0f;
}
inline ColdBreathLook cold_breath_look(const ColdBreath* b, f32 amount) { return b ? cold_breath_look(*b, amount) : ColdBreathLook{}; }

}  // namespace mce::kit
