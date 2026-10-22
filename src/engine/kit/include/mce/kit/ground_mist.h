#pragma once
// Morning mist and ground fog as data (M-GROUNDMIST step 13a). One optional GroundMist per scene (the first in ECS
// order is read). install_weather registers the component; there is no system and nothing is stored between ticks.
//
//   GroundMist  Authoring (all Save, clamped to the Range when read and never rewritten): `strength` (1, 0..2) scales
//               the level; `peak_hour` (6, 0..24) the hour of the thickest mist; `rise_hours` (3) and `burn_hours` (3),
//               0.1..12, the ramp up before the peak and the burn-off after it; `base_humidity` (0.25, 0..1) the mist
//               on a dry day; `wet_gain` (0.75, 0..1) how much Weather.wetness adds; `weather_damp` (1, 0..1) how far
//               rain, storm and snow weather take the mist away; `density` (0.06, 0..1) volumetric extinction per metre
//               at full level; `thickness_m` (6, 0.5..200) the height scale (falloff = 1 / thickness_m); `base_height_m`
//               (0, -1000..1000) the plane the mist sits on; `exp2_density` (0.02, 0..0.2) exp2 fog added at full level
//               when volumetric fog is off; `color` (0.78 0.82 0.88, linear) the mist colour; `enabled` (1).
//               The last four are read by the extract (13b); `mist_level` does not use them (nor `enabled`).
//
// `mist_level` is a pure function of the authoring, the hour (Environment3D.time_of_day) and the Weather; evaluated in
// f32 in a fixed order with smoothstep polynomials only (no pow, exp or trig). Nothing is written anywhere.

#include <algorithm>
#include <cmath>

#include "mce/kit/weather.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct GroundMist {
    f32 strength = 1.0f;
    f32 peak_hour = 6.0f;
    f32 rise_hours = 3.0f;
    f32 burn_hours = 3.0f;
    f32 base_humidity = 0.25f;
    f32 wet_gain = 0.75f;
    f32 weather_damp = 1.0f;
    f32 density = 0.06f;
    f32 thickness_m = 6.0f;
    f32 base_height_m = 0.0f;
    f32 exp2_density = 0.02f;
    math::vec3 color{0.78f, 0.82f, 0.88f};
    u8 enabled = 1;
    u8 pad_[3] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(GroundMist, (strength, Save, Range{0, 2}, Help{"scales the mist level"}),
              (peak_hour, Save, Range{0, 24}, Help{"hour of the thickest mist"}),
              (rise_hours, Save, Range{0.1f, 12}, Help{"hours of build-up before the peak"}),
              (burn_hours, Save, Range{0.1f, 12}, Help{"hours of burn-off after the peak"}),
              (base_humidity, Save, Range{0, 1}, Help{"mist on a dry day"}),
              (wet_gain, Save, Range{0, 1}, Help{"how much Weather.wetness adds to the mist"}),
              (weather_damp, Save, Range{0, 1}, Help{"how far rain, storm and snow weather take the mist away"}),
              (density, Save, Range{0, 1}, Help{"volumetric extinction per metre at full level"}),
              (thickness_m, Save, Range{0.5f, 200}, Help{"height scale of the mist in metres"}),
              (base_height_m, Save, Range{-1000, 1000}, Help{"height of the plane the mist sits on, metres"}),
              (exp2_density, Save, Range{0, 0.2f}, Help{"exp2 fog added at full level when volumetric fog is off"}),
              (color, Save, Help{"mist colour, linear"}), (enabled, Save, Help{"0: the mist is off"}))

namespace detail {

// NaN falls back to `def`, anything else clamps to [lo, hi].
inline f32 mist_clamp(f32 v, f32 def, f32 lo, f32 hi) { return std::isnan(v) ? def : std::clamp(v, lo, hi); }

inline f32 mist_smooth(f32 t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace detail

// The mist level 0..1 for `hour` (Environment3D.time_of_day) and the weather (nullptr: no damping, wetness 0):
//   bump(hour) * clamp(base_humidity + wet_gain * wetness, 0, 1) * weather_factor * strength, clamped to 0..1.
// bump is a smoothstep pair around `peak_hour` (1 at the peak, 0 further than rise_hours before or burn_hours after it),
// wrapped over midnight. weather_factor = 1 + weather_damp * (kind - 1) * intensity with kind Clear 1, Overcast 0.8,
// Rain 0.2, Storm 0, Snow 0.4 (Weather.current). Authoring NaN falls back to the defaults and other values clamp to the
// Range; a NaN or negative hour (time_of_day -1 is "no clock") gives 0, an hour above 24 wraps. A zero level is exactly
// 0.0f. `enabled` is not read here.
inline f32 mist_level(const GroundMist& m, f32 hour, const Weather* weather) {
    using detail::mist_clamp;
    if (std::isnan(hour) || hour < 0.0f) return 0.0f;
    if (hour > 24.0f) hour = std::fmod(hour, 24.0f);
    const f32 peak = mist_clamp(m.peak_hour, 6.0f, 0.0f, 24.0f);
    const f32 rise = mist_clamp(m.rise_hours, 3.0f, 0.1f, 12.0f);
    const f32 burn = mist_clamp(m.burn_hours, 3.0f, 0.1f, 12.0f);
    f32 d = hour - peak;
    if (d > 12.0f) d -= 24.0f;
    if (d < -12.0f) d += 24.0f;
    const f32 bump = d <= 0.0f ? detail::mist_smooth(1.0f + d / rise) : detail::mist_smooth(1.0f - d / burn);
    if (bump <= 0.0f) return 0.0f;

    f32 wetness = 0.0f;
    f32 factor = 1.0f;
    if (weather) {
        wetness = mist_clamp(weather->wetness, 0.0f, 0.0f, 1.0f);
        const f32 intensity = mist_clamp(weather->intensity, 0.0f, 0.0f, 1.0f);
        f32 kind = 1.0f;
        switch (weather->current) {
            case WeatherKind::Clear: kind = 1.0f; break;
            case WeatherKind::Overcast: kind = 0.8f; break;
            case WeatherKind::Rain: kind = 0.2f; break;
            case WeatherKind::Storm: kind = 0.0f; break;
            case WeatherKind::Snow: kind = 0.4f; break;
        }
        const f32 damp = mist_clamp(m.weather_damp, 1.0f, 0.0f, 1.0f);
        factor = 1.0f + damp * ((kind - 1.0f) * intensity);
    }
    const f32 humidity =
        std::clamp(mist_clamp(m.base_humidity, 0.25f, 0.0f, 1.0f) + mist_clamp(m.wet_gain, 0.75f, 0.0f, 1.0f) * wetness, 0.0f, 1.0f);
    const f32 strength = mist_clamp(m.strength, 1.0f, 0.0f, 2.0f);
    const f32 level = std::clamp(bump * humidity * factor * strength, 0.0f, 1.0f);
    return level > 0.0f ? level : 0.0f;
}

}  // namespace mce::kit
