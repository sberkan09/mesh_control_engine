#pragma once
// An opt-in water surface as data (M-WATER step 15a). One optional Water per scene (the first in ECS order is read).
// install_weather registers the component; there is no system and nothing is stored between ticks.
//
//   Water  Authoring (all Save, clamped to the Range when read and never rewritten): `enabled` (1); `level_y` (0,
//          -1000..1000) the world height of the calm surface; `half_extent` (500, 1..20000 m) the half size of the
//          camera-following square; `wave_count` (4, 1..8) the Gerstner waves summed; `amplitude` (0.4, 0..10 m) the
//          height of the largest wave; `wavelength` (24, 1..500 m) the longest wavelength; `steepness` (0.6, 0..1) the
//          Gerstner Q factor, shared out over the waves; `wind_dir_deg` (0, 0..360) the main travel direction (0 = +x,
//          counter-clockwise from above, like WindDirection); `wind_follow` (1, 0..1) 1: the extract takes the
//          WindDirection angle instead; `speed_scale` (1, 0..4) scales the deep-water frequency sqrt(g * k);
//          `shallow_color` (0.10 0.38 0.40) and `deep_color` (0.01 0.08 0.15), linear; `depth_scale_m` (8, 0.1..200);
//          `reflectivity` (1, 0..1); `roughness` (0.08, 0..1); `foam_amount` (0.5, 0..1) and `foam_width_m` (1.5,
//          0.05..20). The helpers below read the wave fields (`level_y`, `wave_count`, `amplitude`, `wavelength`,
//          `steepness`, `wind_dir_deg`, `speed_scale`); the rest is read by the extract and the shader (15b, 15c).
//
// The helpers are pure functions of the authoring, the position and the time; nothing is written anywhere. The wave
// set comes from fixed tables (no rand, no hash of pointers). Every angular frequency is a whole multiple of one base
// frequency, so all waves repeat together after kWaterPeriodSeconds and the time is wrapped to it in f64 (seamless).
// Trig is std::sin / std::cos in f32 in a fixed order; the results are never hashed.

#include <algorithm>
#include <cmath>

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct Water {
    f32 level_y = 0.0f;
    f32 half_extent = 500.0f;
    f32 amplitude = 0.4f;
    f32 wavelength = 24.0f;
    f32 steepness = 0.6f;
    f32 wind_dir_deg = 0.0f;
    f32 wind_follow = 1.0f;
    f32 speed_scale = 1.0f;
    math::vec3 shallow_color{0.10f, 0.38f, 0.40f};
    math::vec3 deep_color{0.01f, 0.08f, 0.15f};
    f32 depth_scale_m = 8.0f;
    f32 reflectivity = 1.0f;
    f32 roughness = 0.08f;
    f32 foam_amount = 0.5f;
    f32 foam_width_m = 1.5f;
    u8 wave_count = 4;
    u8 enabled = 1;
    u8 pad_[2] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(Water, (enabled, Save, Help{"0: the water is off"}),
              (level_y, Save, Range{-1000, 1000}, Help{"world height of the calm surface, metres"}),
              (half_extent, Save, Range{1, 20000}, Help{"half size of the camera-following plane, metres"}),
              (wave_count, Save, Range{1, 8}, Help{"how many Gerstner waves are summed"}),
              (amplitude, Save, Range{0, 10}, Help{"height of the largest wave, metres"}),
              (wavelength, Save, Range{1, 500}, Help{"longest wavelength, metres"}),
              (steepness, Save, Range{0, 1}, Help{"Gerstner Q factor, shared over the waves"}),
              (wind_dir_deg, Save, Range{0, 360}, Help{"main wave direction in degrees: 0 = +x, counter-clockwise"}),
              (wind_follow, Save, Range{0, 1}, Help{"1: the main direction follows WindDirection when present"}),
              (speed_scale, Save, Range{0, 4}, Help{"scales the deep-water wave speed"}),
              (shallow_color, Save, Help{"colour of shallow water, linear"}),
              (deep_color, Save, Help{"colour of deep water, linear"}),
              (depth_scale_m, Save, Range{0.1f, 200}, Help{"depth at which the tint is fully deep, metres"}),
              (reflectivity, Save, Range{0, 1}, Help{"scales the fresnel sky reflection"}),
              (roughness, Save, Range{0, 1}, Help{"blurs the reflection and the sun glint"}),
              (foam_amount, Save, Range{0, 1}, Help{"shore foam; 0 = none"}),
              (foam_width_m, Save, Range{0.05f, 20}, Help{"width of the shore foam band, metres"}))

// The seconds after which every wave of every set repeats exactly.
inline constexpr f64 kWaterPeriodSeconds = 64.0;
inline constexpr u32 kWaterMaxWaves = 8;
inline constexpr f32 kWaterGravity = 9.81f;

// One Gerstner wave. `dir` is a unit vector on the (x, z) plane, `k` = 2 pi / wavelength, `steepness` the effective Q
// (already limited so that the waves together never fold over), `omega` a whole multiple of 2 pi / kWaterPeriodSeconds.
struct WaterWave {
    math::vec2 dir{1.0f, 0.0f};
    f32 k = 0.0f;
    f32 amplitude = 0.0f;
    f32 steepness = 0.0f;
    f32 omega = 0.0f;
    f32 phase_seed = 0.0f;
};

struct WaterWaveSet {
    f32 level_y = 0.0f;
    u32 count = 0;
    WaterWave wave[kWaterMaxWaves];
};

namespace detail {

// NaN falls back to `def`, anything else clamps to [lo, hi].
inline f32 water_clamp(f32 v, f32 def, f32 lo, f32 hi) { return std::isnan(v) ? def : std::clamp(v, lo, hi); }

inline constexpr f64 kWaterTwoPi = 6.283185307179586476925286766559;

// Fixed tables, indexed by wave number: direction offset from the main direction (degrees), the wavelength and the
// amplitude as a fraction of the longest / largest, and the phase seed (radians).
inline constexpr f32 kWaterDirDeg[kWaterMaxWaves] = {0.0f, 17.0f, -23.0f, 41.0f, -52.0f, 66.0f, -78.0f, 90.0f};
inline constexpr f32 kWaterLenRatio[kWaterMaxWaves] = {1.0f, 0.62f, 0.41f, 0.27f, 0.19f, 0.13f, 0.09f, 0.06f};
inline constexpr f32 kWaterAmpRatio[kWaterMaxWaves] = {1.0f, 0.6f, 0.4f, 0.27f, 0.18f, 0.12f, 0.08f, 0.05f};
inline constexpr f32 kWaterPhaseSeed[kWaterMaxWaves] = {0.0f, 1.3f, 2.7f, 4.1f, 5.2f, 0.9f, 3.6f, 5.9f};

}  // namespace detail

// The deep-water angular frequency sqrt(g * k) in rad/s for the wavenumber `k` (rad/m). Not below 0; NaN gives 0.
inline f32 water_phase_speed(f32 k) {
    if (!(k > 0.0f)) return 0.0f;
    return std::sqrt(kWaterGravity * k);
}

// The time wrapped to [0, kWaterPeriodSeconds) in f64; NaN and infinity count as 0.
inline f32 water_wrap_time(f64 t) {
    if (!std::isfinite(t)) return 0.0f;
    const f64 w = t - std::floor(t / kWaterPeriodSeconds) * kWaterPeriodSeconds;
    return static_cast<f32>(w >= kWaterPeriodSeconds ? 0.0 : w);
}

// Expands the authoring into at most 8 waves for the main direction `wind_dir_deg` (0 = +x, counter-clockwise; NaN
// counts as 0, any value wraps). Wave i has the direction offset, wavelength fraction and amplitude fraction of table
// row i; omega = speed_scale * sqrt(g * k) rounded to a whole multiple of the base frequency (at least one, unless the
// speed is 0, which gives 0). The Q of wave i is min(1, steepness / (count * k * amplitude)), so the sum of
// Q * k * amplitude never exceeds `steepness` and the surface never folds over. NaN authoring means the default, other
// values clamp to the Range; `wave_count` 0 counts as 1.
inline WaterWaveSet water_wave_set(const Water& w, f32 wind_dir_deg) {
    using detail::water_clamp;
    WaterWaveSet s;
    s.level_y = water_clamp(w.level_y, 0.0f, -1000.0f, 1000.0f);
    s.count = std::clamp<u32>(w.wave_count, 1u, kWaterMaxWaves);
    const f32 amp = water_clamp(w.amplitude, 0.4f, 0.0f, 10.0f);
    const f32 len = water_clamp(w.wavelength, 24.0f, 1.0f, 500.0f);
    const f32 steep = water_clamp(w.steepness, 0.6f, 0.0f, 1.0f);
    const f32 speed = water_clamp(w.speed_scale, 1.0f, 0.0f, 4.0f);
    const f32 main_deg = std::isfinite(wind_dir_deg) ? std::fmod(wind_dir_deg, 360.0f) : 0.0f;
    const f64 base = detail::kWaterTwoPi / kWaterPeriodSeconds;
    for (u32 i = 0; i < s.count; ++i) {
        WaterWave& v = s.wave[i];
        const f32 a = math::radians(main_deg + detail::kWaterDirDeg[i]);
        v.dir = math::vec2{std::cos(a), std::sin(a)};
        v.k = static_cast<f32>(detail::kWaterTwoPi) / (len * detail::kWaterLenRatio[i]);
        v.amplitude = amp * detail::kWaterAmpRatio[i];
        const f32 ka = v.k * v.amplitude * static_cast<f32>(s.count);
        v.steepness = ka > steep ? steep / ka : 1.0f;
        const f64 omega = static_cast<f64>(speed) * static_cast<f64>(water_phase_speed(v.k));
        const f64 mult = omega > 0.0 ? std::max(1.0, std::round(omega / base)) : 0.0;
        v.omega = static_cast<f32>(mult * base);
        v.phase_seed = detail::kWaterPhaseSeed[i];
    }
    return s;
}

inline WaterWaveSet water_wave_set(const Water& w) { return water_wave_set(w, w.wind_dir_deg); }

namespace detail {

inline f32 water_theta(const WaterWave& v, f32 x, f32 z, f32 t) {
    return v.k * (v.dir.x * x + v.dir.y * z) - v.omega * t + v.phase_seed;
}

// The undisplaced position that the Gerstner displacement carries onto (x, z): two fixed-point steps of u = p - D(u).
inline math::vec2 water_source(const WaterWaveSet& s, math::vec2 p, f32 t) {
    math::vec2 u = p;
    for (u32 it = 0; it < 2; ++it) {
        f32 dx = 0.0f, dz = 0.0f;
        for (u32 i = 0; i < s.count; ++i) {
            const WaterWave& v = s.wave[i];
            const f32 c = v.steepness * v.amplitude * std::cos(water_theta(v, u.x, u.y, t));
            dx += c * v.dir.x;
            dz += c * v.dir.y;
        }
        u = math::vec2{p.x - dx, p.y - dz};
    }
    return u;
}

}  // namespace detail

// The surface height at the world position `xz` (x, z) at scene time `t` seconds (wrapped to the period in f64 inside).
// The horizontal Gerstner displacement is undone by a fixed 2-step inverse, so this is the height of the drawn surface
// at that point. Exactly the level with a zero amplitude. Within level_y +- the sum of the amplitudes.
inline f32 water_height(const WaterWaveSet& s, math::vec2 xz, f32 t) {
    const f32 tw = water_wrap_time(static_cast<f64>(t));
    const math::vec2 u = detail::water_source(s, xz, tw);
    f32 h = 0.0f;
    for (u32 i = 0; i < s.count; ++i) {
        const WaterWave& v = s.wave[i];
        h += v.amplitude * std::sin(detail::water_theta(v, u.x, u.y, tw));
    }
    return s.level_y + h;
}

// The unit surface normal at `xz` (the Gerstner normal at the undisplaced point; y is always above 0). Exactly
// (0, 1, 0) with a zero amplitude.
inline math::vec3 water_normal(const WaterWaveSet& s, math::vec2 xz, f32 t) {
    const f32 tw = water_wrap_time(static_cast<f64>(t));
    const math::vec2 u = detail::water_source(s, xz, tw);
    f32 nx = 0.0f, nz = 0.0f, ny = 1.0f;
    for (u32 i = 0; i < s.count; ++i) {
        const WaterWave& v = s.wave[i];
        const f32 th = detail::water_theta(v, u.x, u.y, tw);
        const f32 ka = v.k * v.amplitude;
        const f32 c = ka * std::cos(th);
        nx -= v.dir.x * c;
        nz -= v.dir.y * c;
        ny -= v.steepness * ka * std::sin(th);
    }
    constexpr f32 kMinUp = 1.0e-4f;
    ny = std::max(ny, kMinUp);
    const f32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
    return math::vec3{nx / len, ny / len, nz / len};
}

inline f32 water_height(const Water& w, math::vec2 xz, f32 t) { return water_height(water_wave_set(w), xz, t); }
inline math::vec3 water_normal(const Water& w, math::vec2 xz, f32 t) { return water_normal(water_wave_set(w), xz, t); }

// Gameplay queries (step 15d): the drawn surface at the world position `xz` at the scene time `scene_time` seconds,
// wrapped to the period in f64 here, so a caller passes its raw clock. `wind_dir_deg` is the main direction the extract
// uses (the WindDirection heading when wind_follow > 0, see Kit3DExtract); the overloads without it use the authored
// `wind_dir_deg`. Pure: the same inputs give the same bits. (The scene time lives outside the World, so there is no
// world-reading convenience here; the Luau binding feeds tick * dt.)
inline f32 water_height_at(const Water& w, math::vec2 xz, f64 scene_time, f32 wind_dir_deg) {
    return water_height(water_wave_set(w, wind_dir_deg), xz, water_wrap_time(scene_time));
}
inline math::vec3 water_normal_at(const Water& w, math::vec2 xz, f64 scene_time, f32 wind_dir_deg) {
    return water_normal(water_wave_set(w, wind_dir_deg), xz, water_wrap_time(scene_time));
}
inline f32 water_height_at(const Water& w, math::vec2 xz, f64 scene_time) {
    return water_height_at(w, xz, scene_time, w.wind_dir_deg);
}
inline math::vec3 water_normal_at(const Water& w, math::vec2 xz, f64 scene_time) {
    return water_normal_at(w, xz, scene_time, w.wind_dir_deg);
}

}  // namespace mce::kit
