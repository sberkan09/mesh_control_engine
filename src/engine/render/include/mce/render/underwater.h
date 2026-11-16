#pragma once
// M-UNDERWATER 44b: the pure CPU helpers of the camera under the water. The crossing band and the submersion blend (the
// surface-crossing transition), the per-channel absorption, the scatter colour far things fade to, the sun's light on the
// water, the Snell window and the lid distance, and everything the underwater pass needs from the water carrier, the
// settings, the camera and the sun. f32 only (the wobble phases are turned into radians in f64), fixed evaluation order, no
// rand, no state: the blend is a pure function of the camera height, so a replay, a pause or a teleport give the same frame.
// Included by underwater.cpp, forward3d.cpp (44c), the tests only.
//
// This header must stay narrow (it does not include scene3d.h, a wide header), so the render types are forward-declared.

#include "mce/math/math.h"
#include "mce/types.h"

namespace mce::render {

struct WaterSurface;
struct Underwater;
struct DirectionalLight;
struct View3D;

// cos(48.6 degrees): the critical angle of water (index 1.333), as the cosine of the angle from straight up.
inline constexpr f32 kUnderwaterWindowCos = 0.6614f;
// The softness (in cosine) of the Snell window's edge.
inline constexpr f32 kUnderwaterWindowSoftness = 0.08f;
// The longest distance any helper returns, in metres (the fog's far cap).
inline constexpr f32 kUnderwaterFarCap = 1.0e4f;
// The fixed distance of the flat Low-tier tint, in metres.
inline constexpr f32 kUnderwaterFlatDistance = 6.0f;

// The crossing blend half-width in metres: `band_setting` when > 0 (clamped to 0.05..2), else clamp(1.5 * amplitude, 0.15,
// 1.0), wider than the swell so the wave-free test cannot flicker. NaN amplitude counts as 0, NaN setting as 0 (automatic).
f32 underwater_band(f32 amplitude, f32 band_setting);

// smoothstep(-band, band, cam_depth) with cam_depth positive below the mean surface: exactly 0 at or above -band, exactly 1
// at or below +band, monotone and C1. band is clamped to 0.05..2 (NaN 0.15); NaN cam_depth counts as 0 (in the middle).
f32 underwater_submersion(f32 cam_depth, f32 band);

// Absorption per metre and channel: -ln(clamp(shallow_c, 0.02, 1)) / (depth_scale_m * fog_scale), never negative. The shallow
// tint is the fraction of each channel that survives one absorption length. depth_scale_m is clamped to 0.5..200 (NaN 8),
// fog_scale to 0.25..4 (NaN 1), a NaN channel counts as the default shallow tint's.
math::vec3 underwater_sigma(math::vec3 shallow, f32 depth_scale_m, f32 fog_scale);

// exp(-sigma * dist) per channel, dist clamped to 0..1e4 (NaN 0), sigma to >= 0 (NaN 0): each channel in 0..1.
math::vec3 underwater_transmittance(math::vec3 sigma, f32 dist);

// The colour far things fade to: mix(deep, shallow, exp(-max(cam_depth, 0) / depth_scale_m)) * light, lighter near the
// surface and darker with depth. depth_scale_m is clamped to 0.5..200 (NaN 8), light to 0..1 (NaN 0.04), a NaN cam_depth
// counts as 0.
math::vec3 underwater_scatter(const WaterSurface& water, f32 cam_depth, f32 light);

// The sun's light on the water, 0.04..1: clamp(luma(color) * intensity * max(0, -direction.y) / 2 + luma(ambient_sky), 0.04, 1)
// with the direction normalized (a zero or non-finite direction or colour gives no sun term).
f32 underwater_light(const DirectionalLight& sun);

// The Snell window for a world ray direction looking up: smoothstep(c - softness, c + softness, dir_y), c = kUnderwaterWindowCos;
// 1 straight up, 0 beyond the cone. softness is clamped to 0.0001..0.3 (NaN kUnderwaterWindowSoftness), NaN dir_y gives 0.
f32 underwater_window(f32 dir_y, f32 softness);

// The distance along an up-going ray to the mean surface: cam_depth / max(dir_y, 1e-3), clamped to 0..1e4 (NaN 0).
f32 underwater_lid_distance(f32 cam_depth, f32 dir_y);

// Everything the pass reads, in the units the shader wants.
struct UnderwaterFrame {
    f32 submersion = 0.0f;    // 0..1, the surface-crossing blend times the setting's strength
    f32 aspect = 1.0f;        // width / height
    f32 slope_x = 0.0f;       // 1 / proj[0][0] (the ray slope at ndc x = 1)
    f32 slope_y = 0.0f;       // 1 / proj[1][1]
    math::vec3 right{1.0f, 0.0f, 0.0f};    // the camera's world axes
    math::vec3 up{0.0f, 1.0f, 0.0f};
    math::vec3 forward{0.0f, 0.0f, -1.0f};
    f32 cam_depth = 0.0f;     // metres below the mean surface (render-relative), positive below
    math::vec3 sigma;         // absorption per metre (detail 0: already times the flat distance)
    f32 wobble_amp = 0.0f;    // screen wobble amplitude in view heights (0 at detail 0)
    math::vec3 scatter;       // the fog colour
    u32 detail = 0;           // 0 flat tint, 1 per-pixel distance, lid and one wobble octave, 2 two octaves
    math::vec3 lid;           // the window colour (the lid straight up)
    f32 window_softness = kUnderwaterWindowSoftness;
    f32 phase[4] = {};        // radians in 0..2 pi: octave 1 (x, y), octave 2 (x, y)
};

// Fills `out` and returns true when the pass must run. False (out untouched) when the water is not on (enabled and a finite
// level), the setting is off or strength is 0, the camera, the matrices or the origin are not finite, an orthographic view
// (proj[3][3] != 0, unsupported) or the submersion is 0. `origin` is the floating origin (f64, xyz); `view.position` is
// camera-relative to it, so cam_depth = (level_y - origin[1]) - position.y, positive below the mean surface (the wave-free
// test: the waves are not read). `time_s` is the scene clock (NaN or infinite counts as 0), `aspect` width / height (NaN or
// <= 0 counts as 1), `detail` the tier's underwater_detail (clamped to 0..2). The wobble phases are whole-cycle: 3, 5, 7 and 11
// cycles per 64 s (octave 1 x, y, octave 2 x, y), taken from the clock wrapped to 64 s in f64, so a wrapped clock repeats
// exactly. Detail 0 is the flat tint: sigma is multiplied by kUnderwaterFlatDistance, the wobble is 0 and the scatter is the
// camera's depth's.
bool underwater_frame(const WaterSurface& water, const Underwater& setting, const View3D& view, const f64 origin[3],
                      const DirectionalLight& sun, f32 time_s, f32 aspect, u32 detail, UnderwaterFrame& out);

}  // namespace mce::render
