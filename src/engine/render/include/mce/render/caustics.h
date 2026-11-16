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
// The web's line width in cells (on the F2 - F1 distance of the cellular noise): v = 1 - smoothstep(0, width, F2 - F1),
// not squared (M-CAUSTICS 52d: the soft falloff reads as light on the floor rather than hard white lines).
inline constexpr f32 kCausticsLineWidth = 0.16f;
// M-CAUSTICS 52d: the surface-orientation factor. A face receives sun light in proportion to dot(n, toward_sun); the web
// is scaled by smoothstep(kCausticsFacingLo, kCausticsFacingHi, dot(n, toward_sun) / toward_sun.y), its irradiance
// relative to a flat floor's, which is also 1 / the web's stretch on the face (projected along the sun from the plane): a
// floor (n up) gets exactly 1 at any sun height (Hi below 1 leaves room for a half-float normal), a face that would show the
// web stretched more than 1 / kCausticsFacingLo (a steep wall under a high sun: long streaks), turned away from the sun or
// lit edge-on gets nothing.
inline constexpr f32 kCausticsFacingLo = 0.6f;
inline constexpr f32 kCausticsFacingHi = 0.95f;
// M-CAUSTICS 52d review: the light is reflected by the surface, so it takes the surface's colour. The pass adds
// colour * luma(term) * the pixel's chroma, src / luma(src) clamped to 0..kCausticsMaxChroma (1 for a pixel darker than
// kCausticsMinLuma, or not finite): warm sun light on sand rather than white lines. luma = 0.2126 r + 0.7152 g + 0.0722 b.
inline constexpr f32 kCausticsMaxChroma = 4.0f;
inline constexpr f32 kCausticsMinLuma = 1.0e-4f;
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
// v = 1 - smoothstep(0, kCausticsLineWidth, F2 - F1). 0..1.
f32 caustics_cells(math::vec2 q, u32 m, u32 seed, f32 phase);

// The web at q (first-octave cells; wrapped to the period here): 0.6 v(q) + 0.3 v(2q) + 0.1 v(4q), `octaves` of them
// (1..3; the reduced sums renormalized; 0 = none). 0..1; 0 for non-finite input.
f32 caustics_pattern(math::vec2 q, f32 phase, u32 octaves);

// M-CAUSTICS 52d. The surface-orientation factor 0..1 for the world-space surface normal `normal` (any length >= 0.5:
// the prepass stores a unit normal; normalized here) and the unit direction toward the sun: smoothstep(kCausticsFacingLo,
// kCausticsFacingHi, dot(n, toward_sun) / toward_sun.y). 1 for a floor; 0 for a face turned away or edge-on, a normal
// shorter than 0.5 or not finite (nothing safe to light), and a sun under kCausticsMinSunY.
f32 caustics_facing(math::vec3 normal, math::vec3 toward_sun);

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
// (caustics_term passes the sun path only: the view path is the water's own business, kWaterFS's.)
f32 caustics_absorption(f32 absorption, f32 d_sun, f32 d_view);

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
};

// Where the sun ray from the camera-relative point `p` meets the plane: `plane_xz` (camera-relative) and the point's
// `depth` below the plane. False when p is not below the plane, the sun is under kCausticsMinSunY, the crossing lies
// outside the water square (|plane_xz - centre| > half_extent on x or z) or any value is not finite.
bool caustics_plane_point(math::vec3 p, const CausticsParams& c, math::vec2& plane_xz, f32& depth);

// The per-pixel term the pass multiplies its colour by (0..1 per channel, all three equal) for the camera-relative point
// `p` with the world-space surface normal `normal` (the prepass's view normal turned to world space; up by default):
// pattern * exp(-absorption * sun path) * depth falloff * caustics_facing * the fog the surface went through in the main
// pass: the exp² fog's exp(-(fog * |p|)²) or, with the volumetric fog in its place (fog_density 0), its transmittance at the
// point, `vol_point` (the volume's vol_fog(uv, view depth).a, read by the shader from the volume texture; 1 without it).
// M-CAUSTICS 52f: no water transmission. Seen from above, the pass draws into the copy of the opaque frame that kWaterFS
// refracts, and the water applies its own Fresnel (wave normal), extinction, foam, reflection and fog to the web as to the
// floor; seen from under the water the underwater composite follows. 0 where caustics_plane_point is false, at or below
// max_depth and for non-finite input. The pass adds the light as caustics_composite does, colour = sun colour *
// intensity * strength * sun share * caustics_sun_fade.
math::vec3 caustics_term(math::vec3 p, const CausticsParams& c, f32 vol_point = 1.0f,
                         math::vec3 normal = math::vec3{0.0f, 1.0f, 0.0f});

// M-CAUSTICS 52d review. The pass's composite: the pixel's colour `src` (linear HDR) with the light `color` (the gate's)
// times `term` (caustics_term) added as the surface reflects it: src + color * luma(term) * chroma(src), chroma(src) =
// clamp(src / luma(src), 0, kCausticsMaxChroma) per channel, or 1 when luma(src) is not in (kCausticsMinLuma, 1e30) (black,
// negative, infinite or NaN). Only term's luma is used. A term of 0 returns src exactly.
math::vec3 caustics_composite(math::vec3 src, math::vec3 color, math::vec3 term);

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
