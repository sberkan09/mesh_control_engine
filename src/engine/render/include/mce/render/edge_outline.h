#pragma once
// M-OUTLINE 39b: the pure CPU helpers of the edge outline. The three per-tap terms (depth, normal, fade) are the
// reference the 39c fragment shader mirrors literally, one expression each, and the frame struct is everything the pass
// receives: only these numbers go to the GPU, so there is no CPU/GPU table to keep in sync. f32 only, fixed evaluation
// order, no rand, no state; a NaN or infinite setting gives a strength of exactly 0. Included by edge_outline.cpp,
// quality.cpp, forward3d.cpp, the kit extract and the tests only.
//
// This header must stay narrow (it does not include scene3d.h, a wide header), so EdgeOutline is forward-declared.
//
// Prepass convention: depth is the linear view depth in meters (0 = sky, the prepass clear), a normal is the unit
// view-space vertex normal (the zero vector = sky).

#include "mce/math/math.h"
#include "mce/types.h"

namespace mce::render {

struct EdgeOutline;

inline constexpr u32 kEdgeOutlineMaxTaps = 8;

// Tap offsets in units of EdgeOutline::thickness output pixels: the cross (+x, -x, +y, -y) first, then the four
// diagonals at 1/sqrt(2) length so every tap is `thickness` pixels away.
inline constexpr math::vec2 kEdgeOutlineTaps[kEdgeOutlineMaxTaps] = {
    {1.0f, 0.0f},
    {-1.0f, 0.0f},
    {0.0f, 1.0f},
    {0.0f, -1.0f},
    {0.70710678f, 0.70710678f},
    {-0.70710678f, 0.70710678f},
    {0.70710678f, -0.70710678f},
    {-0.70710678f, -0.70710678f},
};

struct EdgeOutlineFrame {
    math::vec4 color_opacity;  // linear colour, opacity in w
    math::vec2 tap_uv;         // thickness / {width, height}, in output uv
    f32 depth_threshold = 0.0f;
    f32 normal_threshold = 0.0f;
    f32 depth_weight = 0.0f;
    f32 normal_weight = 0.0f;
    f32 fade_start = 0.0f;
    f32 fade_end = 0.0f;
    f32 fog_density = 0.0f;
    u32 taps = 0;
    bool sky_silhouette = false;
    f32 strength = 0.0f;  // 0 = the pass is skipped
};

// 4 (the cross) or 8 (the cross and the diagonals).
u32 edge_outline_tap_count(bool wide);

// The silhouette strength in [0, 1] of one tap. Depth <= 0 means sky: a sky center gives 0 (lines stay on geometry);
// a sky tap gives 1 when sky_silhouette, else 0. Otherwise only the nearer side draws (d_tap > d_center), with
// smoothstep(threshold, 2 * threshold, (d_tap - d_center) / d_center * max(n_dot_v, 0.2)). NaN depths give 0.
f32 edge_outline_depth_term(f32 d_center, f32 d_tap, f32 threshold, f32 n_dot_v, bool sky_silhouette);

// The crease strength in [0, 1]: smoothstep(threshold, 1.5 * threshold, 1 - dot(n_center, n_tap)); 0 when either side
// is sky (a normal of length below 0.5).
f32 edge_outline_normal_term(math::vec3 n_center, math::vec3 n_tap, f32 threshold);

// (1 - smoothstep(fade_start, fade_end, d)) * exp(-(fog_density * d)^2), in [0, 1]; a non-finite depth gives 0, a NaN fog
// density counts as 0.
f32 edge_outline_fade(f32 view_depth, f32 fade_start, f32 fade_end, f32 fog_density);

// Everything the pass needs. Ranges are clamped to the EdgeOutline ranges and fade_end is ordered after fade_start;
// tap_uv = thickness / {width, height}. strength = opacity when the pass should draw and exactly 0 for a disabled
// setting, opacity 0, both weights 0, fade_end <= 0, a zero-size view and any NaN or infinite input (the frame is then
// otherwise zeroed). `fog_density` is the exp^2 Fog density, already 0 when fog_follow is off or Volumetrics is on.
EdgeOutlineFrame edge_outline_frame(const EdgeOutline& outline, u32 width, u32 height, f32 fog_density, bool wide);

}  // namespace mce::render
