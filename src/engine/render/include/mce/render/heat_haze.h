#pragma once
// M-HEATHAZE 43b: the pure CPU helpers of the heat haze. The screen-space footprint of one haze sphere, everything the
// heat haze pass needs from the HeatHaze setting, the soft-rim falloff, and the integer hash the shader's noise mirrors (so
// a CPU test pins the GLSL port constant for constant). f32 / u32 only, fixed evaluation order, no rand, no state; a
// non-finite input drops the volume. Included by heat_haze.cpp, quality.cpp, forward3d.cpp, the kit extract and install and
// the tests only.
//
// This header must stay narrow (it does not include scene3d.h, a wide header), so HeatHaze is forward-declared and the
// volume array bound is the literal 8. kHeatHazeMaxVolumes (scene3d.h) is the one definition of that constant;
// heat_haze.cpp static_asserts that it is 8, so the two cannot drift apart.

#include "mce/math/math.h"
#include "mce/types.h"

namespace mce::render {

struct HeatHaze;

// A haze sphere on screen. `center` is in uv (fullscreen-pass convention: uv = ndc * 0.5 + 0.5, row 0 is v = 0), `radius`
// is the projected sphere's radius as a fraction of the view height, `near_depth` is the view-space distance to the sphere's
// nearest surface (distance to the centre minus the radius), never below the near plane (kHeatHazeNearPlane).
struct HazeScreen {
    math::vec2 center;
    f32 radius = 0.0f;
    f32 near_depth = 0.0f;
};

// The near plane the nearest depth is clamped to, in metres.
inline constexpr f32 kHeatHazeNearPlane = 0.05f;

// One volume as the pass reads it: the HazeScreen fields and the clamped look parameters.
struct HeatHazeVolumeFrame {
    math::vec2 center;
    f32 near_depth = 0.0f;
    f32 screen_radius = 0.0f;
    f32 strength = 0.5f;
    f32 rise = 0.6f;
    f32 scale = 1.0f;
    f32 softness = 0.5f;
};

struct HeatHazeFrame {
    u32 count = 0;  // volumes kept (0 = the pass is skipped)
    f32 time = 0.0f;
    f32 aspect = 1.0f;
    HeatHazeVolumeFrame volumes[8];  // kHeatHazeMaxVolumes
};

// Projects the sphere (world-space centre and radius) to `out`. `view` is the world-to-view matrix (right-handed, the camera
// looks down -z) and `view_proj` the world-to-clip one. The screen radius uses the angular radius of the sphere, so it holds
// near the camera. A camera inside the sphere gives a radius of 2 (more than the screen covers) centred at the projected
// centre, or at the middle of the screen when the centre is behind the camera. False (out untouched) when the sphere is fully
// behind the camera, when its centre is behind the camera plane while the camera is outside it, when it is fully off
// screen, when radius is not > 0, or when any input is not finite.
bool heat_haze_project(const math::mat4& view_proj, const math::mat4& view, math::vec3 center, f32 radius, HazeScreen& out);

// Culls the setting's volumes (at most min(count, 8)) and fills `out`; returns the number kept (out.count). A volume is
// dropped by heat_haze_project returning false. The look parameters are clamped to the HeatHaze ranges (radius is used by the
// projection; strength 0..2 (NaN 0.5), rise 0..4 (NaN 0.6), scale 0.25..4 (NaN 1), softness 0..1 (NaN 0.5)). Order is the
// setting's order (nothing is sorted: the shader sums the displacements). `time_s` is the renderer's wrapped scene clock
// (NaN or infinite -> 0); `aspect` is width / height (NaN or <= 0 -> 1).
u32 heat_haze_frame(const HeatHaze& haze, const math::mat4& view_proj, const math::mat4& view, f32 aspect, f32 time_s,
                    HeatHazeFrame& out);

// The weight of a volume at distance `d_over_r` (distance to the centre in screen radii) with a rim `softness`:
// 1 - smoothstep(1 - softness, 1, d_over_r), clamped to 0..1. 1 inside, 0 outside the sphere, a soft rim between. softness is
// clamped to 0..1 (NaN 0.5); 0 is a hard edge (1 below d = 1, 0 from there). A negative d counts as 0; NaN d gives 0.
f32 heat_haze_falloff(f32 d_over_r, f32 softness);

// The integer hash of one noise lattice cell, applied in this fixed order: h = mix(x); h = mix(h ^ y * 0x85ebca6b);
// h = mix(h ^ seed * 0xc2b2ae35), where mix(v) is v ^= v >> 16; v *= 0x7feb352d; v ^= v >> 15; v *= 0x846ca68b; v ^= v >> 16.
// All arithmetic is u32 and wraps.
u32 heat_haze_hash(u32 x, u32 y, u32 seed);

}  // namespace mce::render
