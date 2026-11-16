#pragma once
// M-CIRRUS 53b: the pure CPU twin of the sky's cirrus layer (`cirrus_layer` in MCE_GLSL_SKY_LAYERS, drawn by kSkyFS's
// apply_cirrus and reflected by kWaterFS's water_sky) and of the upload that feeds it (p_cirrus / p_cirrus2,
// forward3d.cpp's cirrus_params, which calls cirrus_shader_params). The layer value from a view direction (the
// anisotropic, wind-aligned noise coordinates, the two octaves, the soft mask, the opacity with its 0.5 cap and horizon
// fade), the drift wrap, the lighting (sun and sky ambient, the later sunset at altitude, the dusk warmth, the
// forward-scatter silver near the sun) and the gate (coverage 0, NaN or `enabled` false: off). f32 / u32 only in the
// shader's evaluation order, f64 for the drift wrap as the upload; no rand, no state. Included by cirrus.cpp,
// forward3d.cpp and the tests only.
//
// This header must stay narrow: it does not include scene3d.h (a wide header); render::Cirrus is forward-declared.

#include "mce/math/math.h"
#include "mce/types.h"

namespace mce::render {

struct Cirrus;

// The noise period in cells, along and across the wind (the shader's mod 64; every octave is a whole lattice per
// period).
inline constexpr f32 kCirrusPeriod = 64.0f;
// The sine of the elevation at or under which a ray does not meet the layer (up-going rays only).
inline constexpr f32 kCirrusMinUp = 0.02f;
// The drift speed in m/s at Cirrus::speed 1 (the low layer's kCloudDrift).
inline constexpr f32 kCirrusDrift = 10.0f;
// The opacity's cap: a = mask * density * kCirrusMaxOpacity * horizon fade.
inline constexpr f32 kCirrusMaxOpacity = 0.5f;

// The layer as the shader sees it: p_cirrus = (coverage, density, inv_cell, height_m), p_cirrus2 = (wind, stretch,
// drift). All zero when the layer is off (the shader then skips it).
struct CirrusParams {
    f32 coverage = 0.0f;          // 0..1; 0 = off
    f32 density = 0.0f;           // 0..1
    f32 inv_cell = 0.0f;          // 1 / Cirrus::scale_m (1/m), the cell across the wind
    f32 height_m = 0.0f;          // the plane above the camera (m)
    math::vec2 wind{0.0f, 0.0f};  // the xz unit vector the wind blows toward
    f32 stretch = 0.0f;           // 1..16: a cell is this many times longer along the wind
    f32 drift = 0.0f;             // the drift along the wind in cells, 0..kCirrusPeriod
};

// The uniforms for carrier `c` at scene time `time` (s). Off (all zero) for `enabled` false or a coverage that is not
// above 0 (NaN included). The rest is clamped again as apply_quality does (a caller may skip it): density 0..1 (NaN
// 0.35), scale_m 200..40000 (NaN 3000), stretch 1..16 (NaN 6), speed 0..10 (NaN 2), height_m 1000..30000 (NaN 9000),
// wind_dir normalized (NaN / zero / infinite: +x); the drift is cirrus_drift.
CirrusParams cirrus_shader_params(const Cirrus& c, f32 time);

// The drift along the wind in cells: time * speed * kCirrusDrift / (scale_m * stretch), evaluated and wrapped to
// kCirrusPeriod in f64 so the pattern stays precision-stable as the clock runs. A non-finite time counts as 0; speed,
// scale_m and stretch are clamped as in cirrus_shader_params.
f32 cirrus_drift(f32 time, f32 speed, f32 scale_m, f32 stretch);

// The noise coordinates of view direction d (unit) on the layer: q = (dot(p, w) / stretch - drift, dot(p, w')) mod 64,
// p = d.xz * height / d.y * inv_cell, w = p.wind, w' = (-w.y, w.x). False (q untouched) when the layer is off, d.y is
// at or under kCirrusMinUp, or an input is not finite.
bool cirrus_coords(math::vec3 d, const CirrusParams& p, math::vec2& q);

// The layer's noise n (0..1) at coordinates q (0..64 cells): 0.55 b + 0.45 mix(0.5, s, fine), the broad octave b =
// v(2 q.x, q.y) on a 128 x 64 lattice, the fibre octave s = v(q.x, 6 q.y + 3 b) on a 64 x 384 lattice, v the low
// layer's value noise on the pcg2d lattice hash (own seeds). `fine` 0..1 fades the fibres to their mean (see
// cirrus_opacity). 0 for a non-finite q.
f32 cirrus_noise(math::vec2 q, f32 fine);

// How many pixels a fibre cell spans across the wind along view direction d (unit, up-going), `pix` the radians per
// pixel there: (cell / 6) * sqrt(1 - dot(d.xz, w')^2) * d.y / height / pix. Infinite for a `pix` that is not above 0
// or not finite (full detail).
f32 cirrus_fibre_pixels(math::vec3 d, const CirrusParams& p, f32 pix);

// The layer's opacity along view direction d: smoothstep(t, t + 0.2, n) * density * kCirrusMaxOpacity *
// smoothstep(0.02, 0.2, d.y), t = 0.8 - 0.6 coverage, the noise's `fine` = smoothstep(1.5, 4, cirrus_fibre_pixels(d,
// p, pix)) (the fibres fade where they would alias; `pix` 0: full detail). 0..0.5; 0 when cirrus_coords is false.
f32 cirrus_opacity(math::vec3 d, const CirrusParams& p, f32 pix = 0.0f);

// The layer's colour lit by the frame: `toward_sun` the unit direction toward the light (the shader's -p_light_dir),
// `cs` dot(d, toward_sun), `light` the light's colour times intensity (p_light_color), `ambient_sky` p_amb_sky:
// ambient_sky + 0.5 light tint sun_up (1 + warm) (0.7 + 0.3 cs + 1.5 max(cs, 0)^8), sun_up = smoothstep(-0.16, 0.04,
// sun.y), warm = sun_up (1 - smoothstep(-0.04, 0.3, sun.y)), tint = mix(1, (1, 0.55, 0.35), warm).
math::vec3 cirrus_color(math::vec3 toward_sun, f32 cs, math::vec3 light, math::vec3 ambient_sky);

// Sky colour `c` with the layer composited along d (`pix` as in cirrus_opacity; the shader's cirrus_layer):
// mix(c, cirrus_color, cirrus_opacity). c unchanged where the
// opacity is 0 (off, a ray at or under the horizon limit, a gap between the streaks).
math::vec3 cirrus_apply(math::vec3 c, math::vec3 d, math::vec3 toward_sun, math::vec3 light, math::vec3 ambient_sky,
                        const CirrusParams& p, f32 pix = 0.0f);

}  // namespace mce::render
