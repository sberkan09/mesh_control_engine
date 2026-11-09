#pragma once
// M-WATER2 49b: the pure CPU twins of what the water's fragment shader (kWaterFS, shaders3d.h) does with the scene
// behind and above it, and of the renderer's flow phase step. 49c: the refraction (a screen-space offset of this
// frame's opaque copy by the wave normal's tilt, accepted only where the prepass depth there is behind the water). 49d:
// the reflection trace's confidence (screen edge, distance, rays toward the camera). 49f: the Doppler term of a wave on
// a current, rounded to whole cycles per kit::kWaterPeriodSeconds. The shader repeats the constants and formulas as
// text; the GL tests (test_render3d_water_optics_gl.cpp) pin the two together. f32 only (the flow cycles in f64, as
// water_params), fixed evaluation order, no global state. Included by water_optics.cpp, forward3d.cpp and the tests
// only: keep it narrow (no other render header).

#include "mce/math/math.h"
#include "mce/types.h"

namespace mce::render {

// The reflection march: samples along the reflected ray, spaced quadratically (dense near the surface) up to
// kWaterTraceMaxDist metres, then kWaterTraceRefine bisection steps at the first hit.
inline constexpr u32 kWaterTraceSteps = 24;
inline constexpr u32 kWaterTraceRefine = 4;
inline constexpr f32 kWaterTraceMaxDist = 80.0f;
// The confidence fades over the last kWaterEdgeFade of the screen on every side, over the last (1 -
// kWaterTraceFadeStart) of kWaterTraceMaxDist, and over rays pointing toward the camera (view-space z of the ray 0 ..
// kWaterFacingFade).
inline constexpr f32 kWaterEdgeFade = 0.1f;
inline constexpr f32 kWaterTraceFadeStart = 0.7f;
inline constexpr f32 kWaterFacingFade = 0.6f;
// The refraction offset in screen uv per unit of (horizontal tilt * path / view depth); the path counts up to
// kWaterRefractMaxPath metres, and the offset's length is capped at kWaterRefractMaxUv of the screen.
inline constexpr f32 kWaterRefractScale = 0.5f;
inline constexpr f32 kWaterRefractMaxPath = 4.0f;
inline constexpr f32 kWaterRefractMaxUv = 0.05f;
// The wave clock's period (kit::kWaterPeriodSeconds; forward3d.cpp kWaterPeriod): every frequency is a whole multiple
// of 2 pi / this.
inline constexpr f64 kWaterOpticsPeriod = 64.0;

// The screen-uv offset of the refracted lookup: the wave normal n's horizontal tilt (n.x, 0, n.z) turned into view
// space by `view` (its upper 3 x 3), its xy, times kWaterRefractScale * min(path, kWaterRefractMaxPath) / max(zw,
// 0.05), the length capped at kWaterRefractMaxUv. `path` is the distance along the view ray from the surface to the
// opaque surface behind (negative counts as 0), `zw` the surface's view depth. Exactly zero for an up normal (n.x = n.z
// = 0) and for path 0; a NaN anywhere gives zero.
math::vec2 water_refract_offset(const math::mat4& view, math::vec3 n, f32 path, f32 zw);

// Whether a refracted lookup may use the surface found there: its prepass view depth `z_scene` lies behind the water's
// view depth `zw` (z_scene > zw > 0) and its world height `y_scene` is under the water's level `level` (y_scene <
// level). A nearer value (an object in front of the water), 0 (nothing drawn there) and a surface above the level (a
// post standing in the water, seen past the surface) are rejected, as is NaN: no foreground leak. kWaterFS applies it
// to each of the 2 x 2 half-size texels around the lookup.
bool water_refract_accept(f32 z_scene, f32 zw, f32 y_scene, f32 level);

// The reflection hit's confidence, 0..1: smoothstep(0, kWaterEdgeFade, distance from uv to the nearest screen border) *
// (1 - smoothstep(kWaterTraceFadeStart * kWaterTraceMaxDist, kWaterTraceMaxDist, travel)) * (1 - smoothstep(0,
// kWaterFacingFade, toward)), with `travel` the metres along the reflected ray and `toward` the ray's view-space z
// (positive: toward the camera). 1 in the middle of the screen near the surface, 0 on and past the border and at and
// past kWaterTraceMaxDist; NaN gives 0.
f32 water_reflect_fade(math::vec2 uv, f32 travel, f32 toward);

// The Doppler shift of a wave with wave vector `k` (rad/m on xz) on a current `velocity` (m/s on xz), as whole cycles
// per kWaterOpticsPeriod: round(dot(k, velocity) * period / 2 pi), the dot in f64 of the f32 inputs. A whole number, 0
// for a zero velocity, odd in the velocity (round half away from zero); NaN or infinite input gives 0.
f64 water_flow_cycles(math::vec2 k, math::vec2 velocity);

}  // namespace mce::render
