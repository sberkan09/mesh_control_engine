// M-WATER2 49b: the water's refraction offset, its acceptance, the reflection trace's confidence and the flow phase
// step; kWaterFS (shaders3d.h: water_refract_offset, water_reflect_fade) is the shader twin.
#include "mce/render/water_optics.h"

#include <cmath>

namespace mce::render {

namespace {

f32 clamp01(f32 x) { return x > 0.0f ? (x < 1.0f ? x : 1.0f) : 0.0f; }

// GLSL smoothstep, in the same steps (NaN x gives 0).
f32 smooth(f32 e0, f32 e1, f32 x) {
    const f32 t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

math::vec2 water_refract_offset(const math::mat4& view, math::vec3 n, f32 path, f32 zw) {
    if (!(path == path) || !(zw == zw)) return {0.0f, 0.0f};
    // (mat3(view) * vec3(n.x, 0, n.z)).xy: columns 0 and 2 of the view matrix
    const f32 tx = view.c[0][0] * n.x + view.c[2][0] * n.z;
    const f32 ty = view.c[0][1] * n.x + view.c[2][1] * n.z;
    const f32 p = path > 0.0f ? (path < kWaterRefractMaxPath ? path : kWaterRefractMaxPath) : 0.0f;
    const f32 z = zw > 0.05f ? zw : 0.05f;
    const f32 s = kWaterRefractScale * p / z;
    f32 ox = tx * s, oy = ty * s;
    const f32 len = std::sqrt(ox * ox + oy * oy);
    if (!(len == len) || !std::isfinite(len)) return {0.0f, 0.0f};
    if (len > kWaterRefractMaxUv) {
        const f32 k = kWaterRefractMaxUv / len;
        ox *= k;
        oy *= k;
    }
    return {ox, oy};
}

bool water_refract_accept(f32 z_scene, f32 zw, f32 y_scene, f32 level) {
    return zw > 0.0f && z_scene > zw && y_scene < level;
}

f32 water_reflect_fade(math::vec2 uv, f32 travel, f32 toward) {
    if (!(uv.x == uv.x) || !(uv.y == uv.y) || !(travel == travel) || !(toward == toward)) return 0.0f;
    const f32 ex = uv.x < 1.0f - uv.x ? uv.x : 1.0f - uv.x;
    const f32 ey = uv.y < 1.0f - uv.y ? uv.y : 1.0f - uv.y;
    const f32 edge = smooth(0.0f, kWaterEdgeFade, ex < ey ? ex : ey);
    const f32 far = 1.0f - smooth(kWaterTraceFadeStart * kWaterTraceMaxDist, kWaterTraceMaxDist, travel);
    const f32 facing = 1.0f - smooth(0.0f, kWaterFacingFade, toward);
    return edge * far * facing;
}

f64 water_flow_cycles(math::vec2 k, math::vec2 velocity) {
    constexpr f64 kTau = 6.283185307179586477;
    const f64 d = f64(k.x) * f64(velocity.x) + f64(k.y) * f64(velocity.y);
    if (!std::isfinite(d)) return 0.0;
    const f64 c = std::round(d * kWaterOpticsPeriod / kTau);
    return c == 0.0 ? 0.0 : c;  // no negative zero
}

}  // namespace mce::render
