#pragma once
// The clamps apply_quality (quality.cpp) and the pass upload (forward3d.cpp, which clamps again: a caller may skip
// apply_quality) share for the weather looks (WetSurfaces, SnowCover, Ripples, Clouds, NightSky, WaterSurface). Internal to mce_render: included
// by those two .cpp files only, never from a public header.

#include <cmath>

#include "mce/math/math.h"
#include "mce/types.h"

namespace mce::render::detail {

// 0..1, NaN -> 0.
inline f32 unit01(f32 x) { return x > 0.0f ? (x < 1.0f ? x : 1.0f) : 0.0f; }
// A noise cell in meters, 0.25..256 (NaN -> 0.25).
inline f32 noise_cell(f32 m) { return m > 0.25f ? (m < 256.0f ? m : 256.0f) : 0.25f; }
// A ripple animation rate, 0.1..10 (NaN -> the nominal 1).
inline f32 ripple_speed(f32 s) { return s == s ? (s > 0.1f ? (s < 10.0f ? s : 10.0f) : 0.1f) : 1.0f; }
// A cloud noise cell in meters, 50..20000 (NaN -> 900).
inline f32 cloud_scale(f32 m) { return m == m ? (m > 50.0f ? (m < 20000.0f ? m : 20000.0f) : 50.0f) : 900.0f; }
// A cloud drift multiplier, 0..10 (NaN -> the nominal 1).
inline f32 cloud_speed(f32 s) { return s == s ? (s > 0.0f ? (s < 10.0f ? s : 10.0f) : 0.0f) : 1.0f; }
// A cloud plane height above the camera in meters, 100..20000 (NaN -> 2000).
inline f32 cloud_height(f32 m) { return m == m ? (m > 100.0f ? (m < 20000.0f ? m : 20000.0f) : 100.0f) : 2000.0f; }

// 0..hi, NaN -> `nan_to` (the night sky's scalars).
inline f32 scaled(f32 x, f32 hi, f32 nan_to) { return x == x ? (x > 0.0f ? (x < hi ? x : hi) : 0.0f) : nan_to; }
// The cosine of the moon's angular radius, 0.9..1 (NaN -> 0.9995).
inline f32 moon_cos_size(f32 c) { return c == c ? (c > 0.9f ? (c < 1.0f ? c : 1.0f) : 0.9f) : 0.9995f; }
// A unit direction; NaN/inf or length below 1e-6 -> +y (0, 1, 0).
inline math::vec3 unit_dir3(math::vec3 d) {
    const f32 len2 = d.x * d.x + d.y * d.y + d.z * d.z;
    if (!(len2 >= 1.0e-12f) || len2 > 3.0e38f) return math::vec3{0.0f, 1.0f, 0.0f};
    const f32 len = std::sqrt(len2);
    return math::vec3{d.x / len, d.y / len, d.z / len};
}

// lo..hi, NaN -> `nan_to` (the water plane's scalars).
inline f32 ranged(f32 x, f32 lo, f32 hi, f32 nan_to) { return x == x ? (x > lo ? (x < hi ? x : hi) : lo) : nan_to; }

// A wind direction on xz as a unit vector; NaN/inf or length below 1e-6 -> +x (1, 0).
inline math::vec2 unit_wind_dir(math::vec2 d) {
    const f32 len2 = d.x * d.x + d.y * d.y;
    // len2 is NaN for NaN/inf input (inf*inf is inf, inf-free test below catches it); 1e-12 = (1e-6)^2.
    if (!(len2 >= 1.0e-12f) || len2 > 3.0e38f) return math::vec2{1.0f, 0.0f};
    const f32 len = std::sqrt(len2);
    return math::vec2{d.x / len, d.y / len};
}

}  // namespace mce::render::detail
