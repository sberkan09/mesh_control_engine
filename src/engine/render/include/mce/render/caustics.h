#pragma once
// M-CAUSTICS 52b: the pure CPU helpers of the caustics pass, its CPU twin. The integer-wrapped clock, the camera's world
// position folded into the pattern's period, the procedural web (a cellular "ridged" noise on an integer hash), the depth
// falloff, the sun-elevation fade, the view-path absorption, the point on the water plane a pixel's sun ray crosses, the
// whole per-pixel term and the validity gate the renderer records the pass under. f32 / u32 only (the clock and the camera
// fold in f64), fixed evaluation order, no rand, no state; a non-finite input gives a term of exactly 0 (nothing added).
// Included by caustics.cpp, forward3d.cpp and the tests only; kCausticsFS (shaders3d.h) repeats every step as GLSL.
//
// This header must stay narrow (it does not include scene3d.h, a wide header).

#include "mce/math/math.h"
#include "mce/types.h"

namespace mce::render {

// The sun's smallest sine of elevation at which the pass draws (below it the fade is 0; the shader divides by sun.y).
inline constexpr f32 kCausticsMinSunY = 0.02f;
// The sine of elevation from which the sun-elevation fade is 1 (smoothstep(kCausticsMinSunY, kCausticsFullSunY, sun_y)).
inline constexpr f32 kCausticsFullSunY = 0.2f;
// The clock: ticks per second at speed 1 and the pattern's period in ticks (4 s at speed 1). The scene time times the
// speed is turned into whole ticks and wrapped to the period in f64 before it becomes a float, so the pattern repeats
// exactly after the period and never drifts with a long session.
inline constexpr u32 kCausticsTicksPerSecond = 1000u;
inline constexpr u32 kCausticsPeriodTicks = 4000u;
// The first octave's lattice cells per period (the second has 2x, the third 4x): the pattern tiles every 64 cells.
inline constexpr u32 kCausticsCells = 64u;
// The web's line width in cells (on the F2 - F1 distance of the cellular noise).
inline constexpr f32 kCausticsLineWidth = 0.1f;
// The feature points: jitter 0.2..0.8 of the cell, a closed loop of this radius (cells) around it.
inline constexpr f32 kCausticsLoopRadius = 0.18f;
// The depth falloff: fade in over the first kCausticsSurfaceFade metres under the plane, fade out over the last
// kCausticsDeepFade share of max_depth.
inline constexpr f32 kCausticsSurfaceFade = 0.25f;
inline constexpr f32 kCausticsDeepFade = 0.2f;

// The clock's phase, 0..1: (floor(time * speed * kCausticsTicksPerSecond) mod kCausticsPeriodTicks) / kCausticsPeriodTicks,
// the tick count taken and wrapped in f64. A non-finite time counts as 0; speed clamps 0..4 (NaN 1). Speed 0: 0.
f32 caustics_phase(f32 time, f32 speed);

// The camera's world xz (floating origin + local position, in f64) divided by `scale_m` and wrapped to the 64-cell period:
// the shader adds it to the camera-relative plane xz / scale, so the web is anchored to the world (it stays on the floor as
// the camera moves) while only camera-relative values and a wrapped offset reach the GPU (a rebase is bit for bit).
// scale_m clamps 0.25..16 (NaN 2); a non-finite position counts as 0.
math::vec2 caustics_offset(f64 world_x, f64 world_z, f32 scale_m);

// The centre of the drawn water square relative to the camera (kWaterVS: a camera-following square of 16 x 16 cells,
// 2 * half_extent / 16 m each, its centre snapped to whole cells), from the camera's floating-origin-local xz.
math::vec2 caustics_water_centre(math::vec2 camera_local_xz, f32 half_extent);

// One octave of the web at q (cells, 0..m): the distances to the nearest and second-nearest feature point of the 3 x 3
// lattice cells around q (m cells per period, wrapped), each point jittered by an integer hash (pcg2d) and moved on a
// closed loop whose phase is k * phase + h (k a whole 1..3 from the hash, so the loop closes after the period);
// v = 1 - smoothstep(0, kCausticsLineWidth, F2 - F1), returned squared. 0..1.
f32 caustics_cells(math::vec2 q, u32 m, u32 seed, f32 phase);

// The web at q (first-octave cells; wrapped to the period here): 0.6 v(q) + 0.3 v(2q) + 0.1 v(4q), `octaves` of them
// (1..3; the reduced sums renormalized; 0 = none). 0..1; 0 for non-finite input.
f32 caustics_pattern(math::vec2 q, f32 phase, u32 octaves);

// smoothstep(kCausticsMinSunY, kCausticsFullSunY, sun_y): 0 toward the horizon, 1 above. 0 for NaN.
f32 caustics_sun_fade(f32 sun_y);

// The depth falloff 0..1 at `depth` m below the plane: smoothstep(0, kCausticsSurfaceFade, depth) * (1 - smoothstep((1 -
// kCausticsDeepFade) max_depth, max_depth, depth)); 0 at or above the plane, at or below max_depth and for non-finite input.
f32 caustics_depth_fade(f32 depth, f32 max_depth);

// The view ray's length in water: for a camera above the plane (plane_h < 0, plane_h the plane's height above the camera)
// the part of the ray from the camera to the camera-relative point `p` (below the plane) that lies under the plane:
// |p| * depth / (-p.y), depth = plane_h - p.y. 0 when the camera is at or under the plane (the underwater pass applies its
// own absorption afterwards), when p is not below the plane, and for non-finite input.
f32 caustics_view_path(math::vec3 p, f32 plane_h);

// exp(-absorption * (d_sun + d_view)); absorption clamps 0..4 (NaN 0.15). 0 for a non-finite or negative path.
// (caustics_term passes the sun path only: the view path is the water's own transmission, caustics_transmission.)
f32 caustics_absorption(f32 absorption, f32 d_sun, f32 d_view);

// M-CAUSTICS 52b review. The water body's extinction per metre along the view path, per channel, as kWaterFS applies it to
// what it shows under the water (M-WATER2 49c): -log(clamp(shallow_color, 0.02, 1)) / max(depth_scale_m, 0.5), with
// shallow_color clamped 0..1 per channel (NaN 0) and depth_scale_m 0.1..200 (NaN 8) as water_params clamps them first.
math::vec3 caustics_water_sigma(math::vec3 shallow_color, f32 depth_scale_m);

// The frame's values, camera-relative (the pass's CausticsU carries the same).
struct CausticsParams {
    math::vec3 toward_sun{0.0f, 1.0f, 0.0f};  // unit, y >= kCausticsMinSunY
    f32 plane_h = 0.0f;                       // the water level minus the camera's y (negative: camera above the water)
    math::vec2 centre{0.0f, 0.0f};            // caustics_water_centre
    f32 half_extent = 500.0f;                 // the water square's half size (m)
    f32 inv_scale = 0.5f;                     // 1 / the pattern's cell (1/m)
    math::vec2 offset{0.0f, 0.0f};            // caustics_offset
    f32 phase = 0.0f;                         // caustics_phase
    u32 octaves = 3;                          // 1..3 (the tier's)
    f32 absorption = 0.15f;                   // per metre
    f32 max_depth = 30.0f;                    // m
    f32 fog_density = 0.0f;                   // the main pass's exp² fog (0: none, or the volumetric fog in its place)
    math::vec3 water_sigma{0.0f, 0.0f, 0.0f};  // caustics_water_sigma (the view path's extinction, camera above the water)
    f32 reflectivity = 1.0f;                   // WaterSurface::reflectivity (scales the surface's Fresnel)
};

// M-CAUSTICS 52b review. What of the light at the camera-relative point `p` (below the plane) reaches the camera through
// the water, per channel, and the distance the exp² fog acts over. With the camera above the plane and the view ray
// crossing it inside the water square, the water pixel shows p as kWaterFS composites it: (1 - F) * exp(-water_sigma *
// d_view), F = (0.02 + 0.98 (1 - NoV)^5) * reflectivity (Schlick on the flat plane, NoV = -p.y / |p|), d_view =
// caustics_view_path; the main pass fogs that pixel at the water surface's distance |p| - d_view. Otherwise (the camera at
// or under the plane, or the view ray crossing outside the square) 1 and |p|. (1, |p|) for non-finite input.
math::vec3 caustics_transmission(math::vec3 p, const CausticsParams& c, f32& fog_distance);

// Where the sun ray from the camera-relative point `p` meets the plane: `plane_xz` (camera-relative) and the point's
// `depth` below the plane. False when p is not below the plane, the sun is under kCausticsMinSunY, the crossing lies
// outside the water square (|plane_xz - centre| > half_extent on x or z) or any value is not finite.
bool caustics_plane_point(math::vec3 p, const CausticsParams& c, math::vec2& plane_xz, f32& depth);

// The per-pixel term the pass multiplies its colour by (0..1 per channel) for the camera-relative point `p`: pattern *
// exp(-absorption * sun path) * depth falloff * caustics_transmission * the exp² fog's surface share exp(-(fog * fog
// distance)²). 0 where caustics_plane_point is false, at or below max_depth and for non-finite input. The pass adds
// colour * term to the pixel, colour = sun colour * intensity * strength * sun share * caustics_sun_fade.
math::vec3 caustics_term(math::vec3 p, const CausticsParams& c);

// The pass's validity gate: the renderer records the pass only when this is true. All plain values (no scene3d.h).
struct CausticsGate {
    bool enabled = false;         // PostFx::caustics.enabled
    bool tier = false;            // QualitySettings::caustics
    u32 octaves = 0;              // QualitySettings::caustics_octaves (0: off)
    bool cvar = true;             // r.caustics
    bool water = false;           // a water plane is drawn this frame (WaterSurface on, finite level, its pipeline made)
    bool render_texture = false;  // the view renders to a texture (GI captures, probes, monitors): no caustics
    bool post_source = false;     // the post chain has a source frame of non-zero size
    bool perspective = false;     // cloud_shadow_perspective(proj)
    math::vec3 toward_sun{0.0f, 1.0f, 0.0f};  // the direction toward the sun (any length; normalized here)
    math::vec3 sun_color{1.0f, 1.0f, 1.0f};   // DirectionalLight::color
    f32 intensity = 0.0f;                     // DirectionalLight::intensity
    f32 strength = 0.0f;                      // the clamped setting
    f32 sun_share = 0.0f;                     // cloud_shadow_sun_share
    // M-CAUSTICS 52b review: false when the camera is above the water and the water hides what lies under it (no M-WATER2
    // refraction this frame: calm water without foam, no prepass, r.water.refract 0): the floor is not drawn there, so
    // nothing may be added on the water's opaque tint.
    bool see_through = true;
};

// True when the pass draws; then `sun` is the unit direction toward the sun and `color` the colour the pass adds per unit
// of term (sun colour * intensity * strength * sun share * sun fade, each channel finite and >= 0, at least one > 0).
// False for: the setting off, the tier off or 0 octaves, r.caustics 0, no water, water that hides what lies under it seen
// from above (see_through false), a render-texture view, no post source,
// an orthographic or degenerate projection, a zero, NaN or below-kCausticsMinSunY sun, or a colour that is all 0 (no
// strength, share or light) or not finite.
bool caustics_gate(const CausticsGate& g, math::vec3& sun, math::vec3& color);

}  // namespace mce::render
