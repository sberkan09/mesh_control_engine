#pragma once
// M-LENSRAIN 42b: the pure CPU helpers of the rain on the camera lens. The wetness curve the kit applies, everything the
// lens rain pass needs from the LensRain setting, and the integer hash the shader mirrors (so a CPU test pins the GLSL port
// constant for constant). f32 / u32 only, fixed evaluation order, no rand, no state; a NaN input gives an amount of exactly 0.
// Included by lens_rain.cpp, quality.cpp, forward3d.cpp, the kit extract and install and the tests only.
//
// This header must stay narrow (it does not include scene3d.h, a wide header), so LensRain is forward-declared.

#include "mce/math/math.h"
#include "mce/types.h"

namespace mce::render {

struct LensRain;

// The wetting and drying curve of the lens, one step of `dt` seconds. While `raining`, wetness rises linearly toward
// `intensity` (clamped to 0..1, NaN = 0) at a rate of 1/4 per second, so a dry lens reaches full wetness in about 4 s of
// rain (it stops at `intensity`, and falls back toward it if it is above it). Otherwise it falls linearly to 0 at 1/40 per
// second (about 40 s from fully wet). The result is clamped to 0..1; a NaN wetness or intensity counts as 0; dt <= 0 or
// NaN changes nothing (the wetness is only clamped).
f32 lens_rain_wetness_step(f32 wetness, bool raining, f32 intensity, f32 dt);

// Everything the pass needs. `amount` is exactly 0 for a disabled setting, amount 0 and a NaN amount (the pass skips on 0).
// The other fields are clamped to the LensRain ranges (NaN -> the default); `time` is the renderer's frame time in seconds
// (NaN or infinite -> 0), never the simulation tick; `aspect` is width / height (NaN or <= 0 -> 1).
struct LensRainFrame {
    f32 amount = 0.0f;
    f32 density = 1.0f;
    f32 size = 1.0f;
    f32 refraction = 1.0f;
    f32 blur = 0.35f;
    f32 trail = 0.5f;
    f32 fall_speed = 0.25f;
    f32 time = 0.0f;
    f32 aspect = 1.0f;
    u32 seed = 1;
};

LensRainFrame lens_rain_frame(const LensRain& rain, f32 time_s, f32 aspect);

// The integer hash of one drop cell, a lowbias32 chain applied in this fixed order: h = cell_x; mix(h); h ^= cell_y *
// 0x9e3779b9; mix(h); h ^= seed; mix(h), where mix(x) is x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b;
// x ^= x >> 16. All arithmetic is u32 and wraps.
u32 lens_rain_hash(u32 cell_x, u32 cell_y, u32 seed);

}  // namespace mce::render
