#pragma once
// M-REFLECT2 48b: the pure CPU twin of the screen-space reflection blur that the lit main pass (kMeshFS, shaders3d.h)
// gathers over the half-size SSR target. A rough reflecting pixel takes kSsrTaps taps on a rotated Vogel disc of radius
// ssr_blur_radius() half-size texels instead of the single tap it took before; a roughness at or below kSsrBlurMinRough
// keeps that single tap bit for bit. The shader repeats these formulas as text (MCE_GLSL_MESH_FS); a GL test pins the
// two together. f32 only, fixed evaluation order, no global state. Included by ssr_blur.cpp, forward3d.cpp and the
// tests only: keep it narrow (no other render header).

#include "mce/types.h"

namespace mce::render {

// Roughness at or below which the reflection stays one sharp tap (radius 0).
inline constexpr f32 kSsrBlurMinRough = 0.1f;
// Radius in half-size SSR texels per unit of GGX alpha (roughness squared) above the threshold.
inline constexpr f32 kSsrBlurScale = 24.0f;
// The largest radius, half-size texels.
inline constexpr f32 kSsrBlurMax = 8.0f;
// Taps of the gather (a shader constant, not a setting).
inline constexpr u32 kSsrTaps = 8;
// Extra alpha per unit of normal-map tilt (1 - dot(mapped, geometric)); used by M-REFLECT2 48d.
inline constexpr f32 kSsrBumpGain = 3.0f;

// The gather radius in half-size texels: kSsrBlurScale * (rough^2 - kSsrBlurMinRough^2 + bump), capped at kSsrBlurMax.
// 0 when rough is at or below kSsrBlurMinRough, or NaN; a negative or NaN bump counts as 0; rough above 1 counts as 1.
f32 ssr_blur_radius(f32 rough, f32 bump);

// The extra alpha a normal map adds: kSsrBumpGain * (1 - n_dot_geo), n_dot_geo clamped to [-1, 1]; 0 for a flat map
// (n_dot_geo = 1) and for NaN.
f32 ssr_bump_term(f32 n_dot_geo);

}  // namespace mce::render
