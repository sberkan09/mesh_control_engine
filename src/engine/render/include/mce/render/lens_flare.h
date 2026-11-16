#pragma once
// M-LENSFLARE 38b: the pure CPU helpers of the sun lens flare. Everything the flare pass needs is computed here from
// the camera, the sun direction and the LensFlare setting, and only these numbers go to the GPU (so there is no
// CPU/GPU table to keep in sync). f32 only, fixed evaluation order, no rand, no state; a NaN or infinite input gives a
// strength of exactly 0. Included by lens_flare.cpp, quality.cpp, forward3d.cpp, the kit extract and the tests only.
//
// This header must stay narrow (it does not include scene3d.h, a wide header), so LensFlare is forward-declared and
// the ghost array bound is the literal 6. kLensFlareMaxGhosts (scene3d.h) is the one definition of that constant;
// lens_flare.cpp static_asserts that it is 6, so the two cannot drift apart.

#include "mce/math/math.h"
#include "mce/types.h"

namespace mce::render {

struct LensFlare;

// One ghost: its centre in uv (fullscreen-pass convention, row 0 is v = 0), its radius as a fraction of the view
// height, its intensity (table value times LensFlare::ghost_intensity) and its tint (white lerped toward the table
// tint by ghost_chroma).
struct LensFlareGhost {
    math::vec2 center;
    f32 radius = 0.0f;
    f32 intensity = 0.0f;
    math::vec3 tint;
    f32 pad_ = 0.0f;
};

struct LensFlareFrame {
    math::vec2 sun_uv;
    f32 strength = 0.0f;  // 0 = the pass is skipped
    f32 aspect = 1.0f;
    f32 halo = 0.0f;
    f32 halo_radius = 0.0f;
    f32 starburst = 0.0f;  // 0 = no starburst
    u32 rays = 0;
    f32 occlusion_radius = 0.0f;
    u32 ghost_count = 0;
    LensFlareGhost ghosts[6];  // kLensFlareMaxGhosts
};

// Projects the direction (w = 0) to uv in the fullscreen-pass convention (uv = ndc * 0.5 + 0.5, row 0 is v = 0).
// False when the direction is behind the camera (clip w <= 0) or any input is not finite; uv is untouched then.
bool lens_flare_project(const math::mat4& view_proj, math::vec3 toward_sun, math::vec2& uv);

// smoothstep(-0.02, 0.04, toward_sun.y): 0 at and below the horizon band, 1 above it. A non-finite y gives 0.
f32 lens_flare_horizon_fade(math::vec3 toward_sun);

// smoothstep(0, edge_fade, min(u, 1 - u, v, 1 - v)): 0 on and beyond the border, 1 once the sun is edge_fade of the
// screen inside. edge_fade is clamped to 0.01..0.5 (NaN -> 0.1); a non-finite sun_uv gives 0.
f32 lens_flare_edge_fade(math::vec2 sun_uv, f32 edge_fade);

// Writes min(flare.ghost_count, 6) ghosts to `out` (room for 6; may be null to only count) and returns the count.
// Ghost k sits at 0.5 + (sun_uv - 0.5) * T[k] * ghost_spacing. `aspect` is accepted for symmetry with the pass (radii
// are a fraction of the view height, so it does not change the layout).
u32 lens_flare_ghost_layout(const LensFlare& flare, math::vec2 sun_uv, f32 aspect, LensFlareGhost* out);

// Everything the pass needs. strength = intensity * visibility * horizon_fade * edge_fade, exactly 0 for a disabled
// flare, intensity 0, visibility 0, a sun behind the camera, below the horizon or off screen, and any NaN or
// infinite input (the pass skips on 0; the rest of the frame is then zeroed). `toward_sun` is normalized here.
// starburst is 0 when `starburst_allowed` is false. All other fields are clamped to the LensFlare ranges.
LensFlareFrame lens_flare_frame(const LensFlare& flare, const math::mat4& view_proj, math::vec3 toward_sun, f32 aspect,
                                bool starburst_allowed);

}  // namespace mce::render
