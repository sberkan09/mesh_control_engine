#pragma once
// M-CLOUDSHADOW 51b: the pure CPU helpers of the cloud shadows. Where a world point's ray to the sun meets the sky's cloud
// plane, the cloud density there (the CPU twin of the sky's noise in kSkyFS, `apply_clouds`), the drift in noise cells
// (the CPU twin of `cloud_params`' p_cloud2) and the factor the pass multiplies the pixel by. f32 / u32 only, fixed
// evaluation order, no rand, no state; a non-finite input gives a factor of exactly 1 (no shadow). Included by
// cloud_shadow.cpp, forward3d.cpp (51c), the kit extract (51d) and the tests only.
//
// This header must stay narrow (it does not include scene3d.h, a wide header).

#include "mce/math/math.h"
#include "mce/types.h"

namespace mce::render {

// The sun's smallest sine of elevation at which a point still has a hit on the cloud plane (below it: no hit).
inline constexpr f32 kCloudShadowMinSunY = 0.02f;
// The noise period in first-octave cells (kSkyFS's 64.0, kCloudPeriod in forward3d.cpp).
inline constexpr f32 kCloudShadowPeriod = 64.0f;
// The cloud mask's edge width in noise units at softness 0 (the sky's smoothstep(t, t + 0.12, n)).
inline constexpr f32 kCloudShadowEdge = 0.12f;
// What `softness` 1 adds to the edge width (so the shadow's edge is wider than the cloud's own at softness > 0).
inline constexpr f32 kCloudShadowSoftEdge = 0.3f;

// The cloud layer's parameters as the sky shader sees them (p_cloud, p_cloud2). `coverage` and `density` are 0..1,
// `scale_m` the noise cell in meters (50..20000), `drift_cells` the drift in first-octave cells (cloud_shadow_drift),
// `octaves` how many of the sky's three octaves are read (1..3; 0 = no cloud; the reduced sum is renormalized),
// `softness` 0..1 (0 reproduces the sky's own edge exactly).
struct CloudDensityParams {
    f32 coverage = 0.0f;
    f32 density = 0.7f;
    f32 scale_m = 900.0f;
    math::vec2 drift_cells{0.0f, 0.0f};
    u32 octaves = 3;
    f32 softness = 0.0f;
};

// The point `world` moved along the sun direction `toward_sun` (normalized here) to the plane `height_m` above the camera
// `cam` (same plane as the sky layer); `plane_xz` is the camera-relative xz of that point (the sky's d.xz * height / d.y).
// False (no hit) when toward_sun.y is below kCloudShadowMinSunY, the point is at or above the plane (nothing between it
// and the sun), `height_m` is not positive, or any input is not finite.
bool cloud_shadow_point(math::vec3 world, math::vec3 toward_sun, math::vec3 cam, f32 height_m, math::vec2& plane_xz);

// The drift in first-octave noise cells, wrapped to the period in f64 like cloud_params: wind_dir (normalized here, NaN
// -> +x) * speed * 10 m/s * time / scale_m. A non-finite time counts as 0, speed is clamped 0..10 (NaN 1), scale_m 50..20000
// (NaN 900).
math::vec2 cloud_shadow_drift(f32 time, f32 speed, f32 scale_m, math::vec2 wind_dir);

// The sky's noise n (0..1) at the camera-relative plane xz in meters: 0.55 v(q) + 0.30 v(2q) + 0.15 v(4q), q = xz / scale
// - drift mod 64; `octaves` as in CloudDensityParams. 0 for non-finite input.
f32 cloud_noise(math::vec2 plane_xz, const CloudDensityParams& p);

// The cloud's opacity 0..1 at the plane xz: the sky's `a` without its horizon fade, mask * (0.55 + 0.45 density), mask =
// smoothstep(t, t + edge, n), t = 0.78 - 0.6 coverage, edge = 0.12 + 0.3 softness. 0 for coverage <= 0 (the sky skips the
// layer then) and for non-finite input.
f32 cloud_density(math::vec2 plane_xz, const CloudDensityParams& p);

// The factor the pass multiplies a pixel by: 1 - strength * density * sun_share * fade, fade = smoothstep(0.02, 0.25,
// sun_elevation) (sun_elevation is the sun's y, the sine of its elevation; the shadow fades to 1 toward the horizon where
// the projected point leaves the layer). density, strength, sun_share clamp to 0..1. 1.0 for any non-finite input.
f32 cloud_shadow_factor(f32 density, f32 strength, f32 sun_share, f32 sun_elevation);

// M-CLOUDSHADOW 51c: the sun's share (0..1) of the light an upward-facing surface receives, the `sun_share` the pass passes
// to cloud_shadow_factor: S / (S + A) with S = luma(sun_color) * intensity (the sun's light) and A = luma(ambient_sky)
// (the hemisphere ambient's value straight up), luma = 0.2126 r + 0.7152 g + 0.0722 b. Negative channels and a negative
// intensity count as 0. 0 when S is 0 (no sun: nothing to shade), 1 when A is 0 and S > 0; 0 for any non-finite input
// and when S or S + A overflows.
// A dim overcast or dusk sun against the same sky ambient gives a smaller share, so its cloud shadows are shallower.
f32 cloud_shadow_sun_share(math::vec3 sun_color, f32 intensity, math::vec3 ambient_sky);

// M-CLOUDSHADOW 51c review. The pass's view gate: true for a perspective projection the pass can rebuild positions under
// (proj[3][3] == 0, finite proj[0][0], proj[1][1], proj[2][0], proj[2][1] with |proj[0][0]|, |proj[1][1]| > 1e-6); false
// for an orthographic or degenerate one (the pass is then not recorded).
bool cloud_shadow_perspective(const math::mat4& proj);

// The share of a pixel that is surface rather than the main pass's exp² fog at `distance` m: T = exp(-(density *
// distance)²), the main pass's own term (it squares the density, so a negative density fogs too). Exactly 1 for a density
// of 0 or a non-finite density or distance (the pass then treats the pixel as unfogged).
f32 cloud_shadow_fog_visibility(f32 fog_density, f32 distance);

// The shaded pixel. The main pass wrote pixel = s T + fog (1 - T) (s the lit surface, T cloud_shadow_fog_visibility); the
// cloud shades only s: out = pixel - k (pixel - (1 - T) fog), k = 1 - factor, which is s factor T + fog (1 - T). With
// T >= 1 (no fog) it is pixel * factor exactly, the pass's unfogged path. factor clamps to 0..1 (non-finite: 1).
math::vec3 cloud_shadow_shade(math::vec3 pixel, f32 factor, f32 fog_visibility, math::vec3 fog_color);

}  // namespace mce::render
