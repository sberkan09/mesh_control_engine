#pragma once
// M-SUNSHAFTS 45b: the pure CPU helpers of the sun light shafts. The sun's screen position (unclamped, so an off-screen sun
// keeps a direction), the fades (elevation, view), the share of the picture that has air to scatter in, the ray tint, the
// soft clamp the shader mirrors, the tier's sample count, the integer hash the shader's jitter mirrors, and everything the
// pass needs from the setting, the sun, the fog and the camera. f32 / u32 only, fixed evaluation order, no rand, no state;
// a non-finite input gives a strength of exactly 0 (or false). Included by sun_shafts.cpp, forward3d.cpp (45c), the kit
// extract and install (45d) and the tests only.
//
// This header must stay narrow (it does not include scene3d.h, a wide header), so the render types are forward-declared.

#include "mce/math/math.h"
#include "mce/types.h"

namespace mce::render {

struct SunShafts;
struct Fog;
struct Volumetrics;
struct DirectionalLight;

// The largest magnitude the projected sun uv keeps (a sun almost on the camera's side plane stays finite).
inline constexpr f32 kSunShaftsMaxUv = 4.0f;
// The elevation fade's floor at noon (the rays are short there and a flat white veil is not wanted).
inline constexpr f32 kSunShaftsHighFloor = 0.35f;
// The share of the medium factor that stays where the volumetric fog is drawn.
inline constexpr f32 kSunShaftsVolumeShare = 0.4f;

// The sun's uv in the fullscreen-pass convention (uv = ndc * 0.5 + 0.5, row 0 is v = 0), not clamped to the screen (each
// component is limited to +-kSunShaftsMaxUv), and `facing` = dot(camera forward, toward_sun) read from the projection (the
// clip w of the direction). toward_sun is normalized here. False when clip w <= 0 (behind the camera), the direction is zero
// or any input is not finite.
bool sun_shafts_project(const math::mat4& view_proj, math::vec3 toward_sun, math::vec2& uv, f32& facing);

// smoothstep(-0.02, 0.04, y) * mix(1, high_floor, smoothstep(0.35, 0.9, y)): 0 at and below the horizon band, 1 for a low
// sun, a gentle drop toward high_floor (clamped to 0..1, NaN 0.35) at noon. NaN y gives 0.
f32 sun_shafts_elevation_fade(math::vec3 toward_sun, f32 high_floor);

// smoothstep(0, 0.3, facing) * (1 - smoothstep(0, 0.75, border_distance(sun_uv))); border_distance is the distance the uv
// lies outside the 0..1 square (0 inside). Non-finite input gives 0.
f32 sun_shafts_view_fade(math::vec2 sun_uv, f32 facing);

// The share of the picture that has air to scatter in: 0.35 + 0.65 * (1 - exp(-(density * 40)^2)) for the exp^2 fog (density
// clamped to >= 0), times kSunShaftsVolumeShare where the volumetric fog is drawn. A non-finite density gives 0.
f32 sun_shafts_medium(const Fog& fog, const Volumetrics& volumetrics, bool volumetrics_drawn);

// sun_color * mix(1, fog_color / max(luma(fog_color), 1e-3), 0.5 * fog_amount) (fog_amount clamped to 0..1): the fog changes
// the hue of the rays, not their brightness. A non-finite input gives (0, 0, 0).
math::vec3 sun_shafts_tint(math::vec3 sun_color, math::vec3 fog_color, f32 fog_amount);

// cap * (1 - exp(-x / cap)) for cap > 0 (x clamped to >= 0), 0 for cap <= 0 or NaN: monotone, C1, equal to x for small x,
// never above cap. The shader applies the same function per channel.
f32 sun_shafts_soft_clamp(f32 x, f32 cap);

// The samples per pixel of a tier's detail: 0 (Low, off), 16 (Medium), 32 (High); anything else clamps to 32.
u32 sun_shafts_samples(u32 detail);

// The integer hash the shader's jitter mirrors: the lowbias32 mix of heat_haze_hash,
// h = mix(x); h = mix(h ^ y * 0x85ebca6b); h = mix(h ^ seed * 0xc2b2ae35).
u32 sun_shafts_hash(u32 x, u32 y, u32 seed);

// Everything the pass reads, in the units the shader wants.
struct SunShaftsFrame {
    math::vec2 sun_uv{0.5f, 0.5f};  // the sun on screen, possibly outside 0..1
    f32 aspect = 1.0f;              // width / height
    f32 length = 0.85f;
    f32 decay = 0.97f;
    f32 threshold = 1.0f;
    f32 cap = 1.5f;
    u32 samples = 0;                // 16 or 32
    math::vec3 color;               // tint * intensity * strength
    f32 scatter_m = 60.0f;          // the veil distance
    f32 strength = 0.0f;            // elevation_fade * view_fade * medium * visibility * (1 - submersion)
};

// Fills `out` and returns true when the pass must run. False (out untouched) when the setting is off, intensity or
// visibility is 0, the tier has no samples, the sun is behind the camera or below the horizon, the strength is 0 or any
// input is not finite. The sun is the way `sun.direction` travels, reversed. `submersion` is 0..1 (the underwater blend; NaN
// gives false), `aspect` width / height (NaN or <= 0 counts as 1), `detail` the tier's detail (0 off, 1 Medium, 2 High).
bool sun_shafts_frame(const SunShafts& setting, const DirectionalLight& sun, const Fog& fog, const Volumetrics& volumetrics,
                      bool volumetrics_drawn, const math::mat4& view_proj, f32 aspect, u32 detail, f32 submersion,
                      SunShaftsFrame& out);

}  // namespace mce::render
