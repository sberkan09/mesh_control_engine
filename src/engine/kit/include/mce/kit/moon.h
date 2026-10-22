#pragma once
// A moon with phases as data (M-NIGHTSKY step 14a). One optional Moon per scene (the first in ECS order is read).
// install_weather registers the component; there is no system and nothing is stored between ticks.
//
//   Moon  Authoring (all Save, clamped to the Range when read and never rewritten): `phase_offset` (0.5, 0..1) the
//         phase at day 0 hour 0 (0.5 is a full moon); `cycle_days` (29.53, 2..120) the synodic period;
//         `freeze_phase` (-1, -1..1) 0..1 pins the phase, negative follows the clock; `inclination_deg` (5, 0..30) how
//         far the path sits off the sun's arc; `light_intensity` (0.12, 0..2), `light_color` (0.55 0.65 1.0, linear)
//         and `light_shadows` (1) are the moonlight; `disk_size` (0.999, 0.99..1, cos of the angular radius),
//         `disk_intensity` (6, 0..50), `star_density` (0.6, 0..1), `star_brightness` (1, 0..4) and `star_twinkle`
//         (0.3, 0..1) are the sky; `enabled` (1). The light and sky fields are read by the extract (14b, 14e); the
//         helpers below read only `phase_offset`, `cycle_days`, `freeze_phase`, `inclination_deg` (and not `enabled`).
//
// The helpers are pure functions of the authoring, WorldClock.day, the hour (Environment3D.time_of_day) and the
// Weather; nothing is written anywhere. The phase is computed in f64 in a fixed order and the illumination is a fixed
// polynomial (no cos), so both are bit-stable across platforms. The direction goes through render::sun_direction_at
// (std::sin and std::cos) and is not part of any hash.

#include <algorithm>
#include <cmath>

#include "mce/kit/weather.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"
#include "mce/render/atmosphere.h"

namespace mce::kit {

struct Moon {
    f32 phase_offset = 0.5f;
    f32 cycle_days = 29.53f;
    f32 freeze_phase = -1.0f;
    f32 inclination_deg = 5.0f;
    f32 light_intensity = 0.12f;
    math::vec3 light_color{0.55f, 0.65f, 1.0f};
    f32 disk_size = 0.9990f;
    f32 disk_intensity = 6.0f;
    f32 star_density = 0.6f;
    f32 star_brightness = 1.0f;
    f32 star_twinkle = 0.3f;
    u8 light_shadows = 1;
    u8 enabled = 1;
    u8 pad_[2] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(Moon, (phase_offset, Save, Range{0, 1}, Help{"moon phase at day 0, hour 0 (0 new, 0.5 full)"}),
              (cycle_days, Save, Range{2, 120}, Help{"days for one cycle of phases"}),
              (freeze_phase, Save, Range{-1, 1}, Help{"0..1 pins the phase, negative follows the clock"}),
              (inclination_deg, Save, Range{0, 30}, Help{"degrees the moon's path sits off the sun's arc"}),
              (light_intensity, Save, Range{0, 2}, Help{"directional light intensity at full moon"}),
              (light_color, Save, Help{"moonlight colour, linear"}),
              (disk_size, Save, Range{0.99f, 1}, Help{"cosine of the moon disk's angular radius"}),
              (disk_intensity, Save, Range{0, 50}, Help{"brightness of the moon disk"}),
              (star_density, Save, Range{0, 1}, Help{"how many stars; 0 = none"}),
              (star_brightness, Save, Range{0, 4}, Help{"star brightness"}),
              (star_twinkle, Save, Range{0, 1}, Help{"how much stars twinkle"}),
              (light_shadows, Save, Help{"1: the moonlight casts shadows"}),
              (enabled, Save, Help{"0: the moon is off"}))

namespace detail {

// NaN falls back to `def`, anything else clamps to [lo, hi].
inline f32 moon_clamp(f32 v, f32 def, f32 lo, f32 hi) { return std::isnan(v) ? def : std::clamp(v, lo, hi); }

inline f32 moon_smooth(f32 t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// An hour usable as a clock reading: NaN, infinite and negative (no clock) become 0, above 24 wraps.
inline f32 moon_hour(f32 hour) {
    if (!std::isfinite(hour) || hour < 0.0f) return 0.0f;
    return hour > 24.0f ? std::fmod(hour, 24.0f) : hour;
}

}  // namespace detail

// The phase 0..1 (0 new, 0.5 full; 1 wraps to 0) on `day` (WorldClock.day, 0 without a clock) at `hour`. Follows the
// clock as frac((day + hour / 24) / cycle_days + phase_offset) evaluated in f64 in that order, or is `freeze_phase`
// when that is >= 0 (1 wraps to 0). NaN authoring means the default and other values clamp to the Range; a NaN or
// negative hour counts as 0, an hour above 24 wraps. Always in [0, 1).
inline f32 moon_phase(const Moon& m, u32 day, f32 hour) {
    using detail::moon_clamp;
    const f32 freeze = moon_clamp(m.freeze_phase, -1.0f, -1.0f, 1.0f);
    f32 phase;
    if (freeze >= 0.0f) {
        phase = freeze;
    } else {
        const f64 cycle = static_cast<f64>(moon_clamp(m.cycle_days, 29.53f, 2.0f, 120.0f));
        const f64 offset = static_cast<f64>(moon_clamp(m.phase_offset, 0.5f, 0.0f, 1.0f));
        const f64 days = static_cast<f64>(day) + static_cast<f64>(detail::moon_hour(hour)) / 24.0;
        const f64 t = days / cycle + offset;
        phase = static_cast<f32>(t - std::floor(t));
    }
    return phase >= 1.0f ? 0.0f : phase;
}

// The lit fraction 0..1 of the disk for `phase`: 0.5 - 0.5 * cos(2 pi phase) as sin^2 of a fixed odd polynomial (no
// cos, so the result is the same on every platform). 0 at new, 1 at full, monotone between, symmetric about full.
// A NaN phase gives 0; the phase wraps.
inline f32 moon_illumination(f32 phase) {
    if (std::isnan(phase)) return 0.0f;
    phase -= std::floor(phase);
    const f32 u = 1.0f - std::fabs(1.0f - 2.0f * phase);  // 0 at new, 1 at full
    if (u <= 0.0f) return 0.0f;
    if (u >= 1.0f) return 1.0f;
    // sin(pi u / 2), Taylor to u^9 with the last coefficient set so that the value at u = 1 is 1.
    constexpr f64 c1 = 1.5707963267948966, c3 = 0.6459640975062462, c5 = 0.0796926262461670, c7 = 0.0046817541353187;
    constexpr f64 c9 = 1.0 - (c1 - c3 + c5 - c7);
    const f32 u2 = u * u;
    const f32 s = u * (static_cast<f32>(c1) +
                       u2 * (-static_cast<f32>(c3) + u2 * (static_cast<f32>(c5) + u2 * (-static_cast<f32>(c7) + u2 * static_cast<f32>(c9)))));
    return std::clamp(s * s, 0.0f, 1.0f);
}

// The unit direction the moonlight travels (like render::sun_direction_at; negate it for the direction toward the
// moon). The moon sits `phase * 24` hours behind the sun on the sun's own arc (a full moon rises at sunset, a new moon
// is up with the sun), tilted off the arc toward the arc's normal by `inclination_deg`. A NaN or negative hour counts
// as 0 and a NaN phase as 0.
inline math::vec3 moon_direction(const Moon& m, f32 hour, f32 phase) {
    if (std::isnan(phase)) phase = 0.0f;
    phase -= std::floor(phase);
    const math::vec3 d = render::sun_direction_at(detail::moon_hour(hour) - phase * 24.0f);
    const f32 inc = math::radians(detail::moon_clamp(m.inclination_deg, 5.0f, 0.0f, 30.0f));
    // The normal of the sun's default arc (noon elevation 60, azimuth 0): perpendicular to east-west and the arc's slant.
    constexpr f32 kSinTilt = 0.5f, kCosTilt = 0.8660254f;  // tilt = 90 - 60 degrees
    const math::vec3 n{0.0f, kSinTilt, kCosTilt};
    const f32 ci = std::cos(inc), si = std::sin(inc);
    const math::vec3 t{d.x * ci + n.x * si, d.y * ci + n.y * si, d.z * ci + n.z * si};
    const f32 len = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);
    return len > 0.0f ? math::vec3{t.x / len, t.y / len, t.z / len} : d;
}

// The effective moonlight strength 0..1 on `day` at `hour` (Environment3D.time_of_day) and the weather (nullptr: no
// damping): moon_illumination(phase) * smoothstep(0, 0.2, moon elevation) * weather_factor, where the elevation is the
// y of the direction toward the moon and weather_factor = 1 + (kind - 1) * intensity with kind Clear 1, Overcast 0.5,
// Rain 0.3, Storm 0.15, Snow 0.5. A NaN or negative hour (time_of_day -1 is "no clock") gives 0. A zero level is
// exactly +0.0f. `enabled` is not read here.
inline f32 moon_level(const Moon& m, u32 day, f32 hour, const Weather* weather) {
    using detail::moon_clamp;
    if (std::isnan(hour) || hour < 0.0f) return 0.0f;
    const f32 phase = moon_phase(m, day, hour);
    const f32 illum = moon_illumination(phase);
    if (illum <= 0.0f) return 0.0f;
    const math::vec3 d = moon_direction(m, hour, phase);
    const f32 up = detail::moon_smooth(-d.y / 0.2f);
    if (up <= 0.0f) return 0.0f;
    f32 factor = 1.0f;
    if (weather) {
        const f32 intensity = moon_clamp(weather->intensity, 0.0f, 0.0f, 1.0f);
        f32 kind = 1.0f;
        switch (weather->current) {
            case WeatherKind::Clear: kind = 1.0f; break;
            case WeatherKind::Overcast: kind = 0.5f; break;
            case WeatherKind::Rain: kind = 0.3f; break;
            case WeatherKind::Storm: kind = 0.15f; break;
            case WeatherKind::Snow: kind = 0.5f; break;
        }
        factor = 1.0f + (kind - 1.0f) * intensity;
    }
    const f32 level = std::clamp(illum * up * factor, 0.0f, 1.0f);
    return level > 0.0f ? level : 0.0f;
}

}  // namespace mce::kit
