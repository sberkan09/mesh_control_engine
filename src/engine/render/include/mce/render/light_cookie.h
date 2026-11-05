#pragma once
// M-LIGHTCOOKIE 40b: the pure CPU helpers of the light cookies (a texture a spot light or the sun projects). The two
// projection helpers (spot_cookie_matrix + the uv/inside test, sun_cookie_uv), spot_cookie_edge and cookie_lod are the
// reference the 40d and 40e fragment shaders mirror literally, one expression each; the GPU record is cookie_record.
// f32 only, fixed evaluation order, no rand, no state; a NaN or infinite input falls back to the documented default.
// Included by light_cookie.cpp, quality.cpp, forward3d.cpp, the kit extract and the tests only.
//
// This header must stay narrow (it does not include scene3d.h or clusters.h, wide headers), so PointLight, SunCookie and
// DirectionalLight are forward-declared.
//
// Conventions. The spot cookie is a square perspective frustum of full vertical angle 2 * spot_outer along the light's
// direction, so the cone's circle is inscribed. The lookup is uv = clip.xy / clip.w * 0.5 + 0.5, a pixel is inside when
// clip.w > 0 and both coordinates are in [0, 1], and uv.y grows toward the light's up (+Y, or +Z within about one degree of
// vertical). A positive `roll` turns the projected pattern counter-clockwise as seen from the light looking along its
// direction. The sun pattern lives on the ground plane (y = 0): a point is carried along the sun's direction d down (or up)
// to y = 0, g = (x - y d.x / d.y, z - y d.z / d.y), so the pattern on flat ground never swims or snaps as the sun moves,
// and a grazing sun (|d.y| below kSunCookieMinDy) stretches it no further than that floor; a positive roll turns the
// pattern counter-clockwise in (x, z) seen from above (from +x toward +z).

#include "mce/math/math.h"
#include "mce/types.h"

namespace mce::render {

struct PointLight;
struct SunCookie;
struct DirectionalLight;

inline constexpr u32 kMaxCookieLayers = 16;
inline constexpr u32 kMaxCookieLights = 16;

// The world-to-cookie-clip matrix of a spot (GL clip space, like LocalShadowTile::view_proj): perspective with a vertical
// field of view of 2 * spot_outer, aspect 1, near 0.05, far `radius`, after a look-at along `direction` (a non-finite or
// zero direction gives straight down) with up +Y unless the cone is within about one degree of vertical (then +Z), rolled
// by `roll` radians (a non-finite roll counts as 0). spot_outer is clamped to 0.01..1.45 radians (NaN: 0.5), radius to at
// least 0.1 (NaN: 5).
math::mat4 spot_cookie_matrix(math::vec3 position, math::vec3 direction, f32 spot_outer, f32 roll, f32 radius);

// 1 when `edge` is 0 (or NaN), else smoothstep(0, edge * 0.5, min(min(uv.x, 1 - uv.x), min(uv.y, 1 - uv.y))); edge is
// clamped to 0..1. 0 at and beyond the border, and for a non-finite uv.
f32 spot_cookie_edge(math::vec2 uv, f32 edge);

// The floor of |d.y| in sun_cookie_uv: a sun lower than about 2.9 degrees projects as if it were at that height.
inline constexpr f32 kSunCookieMinDy = 0.05f;

// The texture coordinate of a world point for the sun's pattern: with d = `sun_dir` (the way the light travels; a
// non-finite or zero one counts as straight down), dy = d.y floored to -kSunCookieMinDy (or +kSunCookieMinDy for a sun
// travelling upward) and p = (x - y d.x / dy, z - y d.z / dy) the ground point (y = 0) the light reaches it from,
// uv = (c p.x + s p.y, -s p.x + c p.y) / size + offset / size with c, s the cosine and sine of `roll`, size clamped to
// 0.5..1000 (NaN: 20), roll a non-finite -> 0. Because the offset is added after the roll, shifting it by `size` leaves
// every uv on the same texel; p is linear in the world point, so an offset of the world adds its own p.
math::vec2 sun_cookie_uv(math::vec3 world, math::vec3 sun_dir, const SunCookie& cookie);
// sun_cookie_uv's dy for a unit direction's y.
inline f32 sun_cookie_dy(f32 dir_y) {
    if (dir_y <= 0.0f) return dir_y < -kSunCookieMinDy ? dir_y : -kSunCookieMinDy;
    return dir_y > kSunCookieMinDy ? dir_y : kSunCookieMinDy;
}

// The mip for a spot sample: log2(max(footprint / (2 * distance * tan(outer) / size), 1)), clamped to the chain
// (0..log2(size)). A non-finite or non-positive input, or size 0, gives 0.
f32 cookie_lod(f32 pixel_footprint_m, f32 distance_m, f32 spot_outer, u32 size);
// The same for the sun's tiling: log2(max(footprint / (size_m / texels), 1)), clamped to 0..log2(texels).
f32 cookie_lod_sun(f32 pixel_footprint_m, f32 size_m, u32 texels);

// What the frame bakes and binds. `key[layer]` is the TextureStore key held by atlas layer `layer` (first-seen order, the sun
// first, so an unchanged scene keeps its layers); `layer_of[record]` the layer of cookied light number `record`
// (records are numbered in submission order, the same numbers written into `light_record`); `sun_layer` the sun's layer or
// ~0u. `lights` counts the records. `dropped` counts the lights that asked for a cookie and did not get one (beyond
// max_lights, or whose texture did not fit max_layers); a cookie_strength of 0 or less costs nothing and is not counted.
struct CookiePlan {
    u32 layer_of[kMaxCookieLights] = {};
    u32 key[kMaxCookieLayers] = {};
    u32 count = 0;  // layers used
    u32 lights = 0;
    u32 sun_layer = ~0u;
    u32 dropped = 0;
};

// Walks the point lights in submission order. Only a spot (spot_outer > 0) with a cookie key and a strength above 0 takes
// part. The sun's cookie (texture key set, strength above 0) is not counted against `max_lights` and takes layer 0 first.
// Each distinct key gets one layer; a key that does not fit max_layers (clamped to kMaxCookieLayers) leaves its lights
// plain. `light_record[i]` receives the record index of light i or ~0u (entries past the end of the span are skipped, and
// a shorter span than `lights` means the rest get no entry).
CookiePlan plan_cookies(Span<const PointLight> lights, const DirectionalLight& sun, u32 max_layers, u32 max_lights,
                        Span<u32> light_record);

// The std430 row of one cookied light (LightCookies, 40c).
struct GpuCookieRecord {
    math::mat4 view_proj;
    math::vec4 params;  // x layer, y strength, z edge, w unused (0)
};
static_assert(sizeof(GpuCookieRecord) == 80, "std430 stride of the cookie records");

// view_proj = spot_cookie_matrix(light.position, light.direction, light.spot_outer, light.cookie_roll, light.radius);
// params = {layer, strength clamped 0..1 (NaN 1), edge clamped 0..1 (NaN 0), 0}.
GpuCookieRecord cookie_record(const PointLight& light, u32 layer);

}  // namespace mce::render
