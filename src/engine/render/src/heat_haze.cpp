#include "mce/render/heat_haze.h"

#include <cmath>

#include "mce/render/scene3d.h"
#include "unit_clamp.h"

namespace mce::render {

// The header declares the volume array with a literal bound (it cannot include the wide scene3d.h).
static_assert(kHeatHazeMaxVolumes == 8, "update HeatHazeFrame::volumes in heat_haze.h");

namespace {

u32 mix(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

bool finite3(math::vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

bool finite_matrix(const math::mat4& m) {
    for (usize c = 0; c < 4; ++c)
        for (usize r = 0; r < 4; ++r)
            if (!std::isfinite(m.c[c][r])) return false;
    return true;
}

f32 row_len(const math::mat4& m, usize row) {
    const f32 x = m.c[0][row], y = m.c[1][row], z = m.c[2][row];
    return std::sqrt(x * x + y * y + z * z);
}

}  // namespace

bool heat_haze_project(const math::mat4& view_proj, const math::mat4& view, math::vec3 center, f32 radius, HazeScreen& out) {
    if (!finite3(center) || !finite_matrix(view_proj) || !finite_matrix(view)) return false;
    if (!(radius > 0.0f) || !std::isfinite(radius)) return false;
    const math::vec3 v = math::transform_point(view, center);
    const f32 dist = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    const f32 depth = -v.z;  // distance along the view axis
    if (!std::isfinite(dist) || !std::isfinite(depth)) return false;
    if (depth + radius <= 0.0f) return false;  // fully behind the camera
    const f32 near_depth = dist - radius > kHeatHazeNearPlane ? dist - radius : kHeatHazeNearPlane;

    const math::vec4 clip = view_proj * math::vec4(center, 1.0f);
    if (dist <= radius) {  // the camera is inside the sphere: it covers the screen
        math::vec2 c{0.5f, 0.5f};
        if (clip.w > 0.0f) {
            const math::vec2 p{clip.x / clip.w * 0.5f + 0.5f, clip.y / clip.w * 0.5f + 0.5f};
            if (std::isfinite(p.x) && std::isfinite(p.y)) c = p;
        }
        out.center = c;
        out.radius = 2.0f;
        out.near_depth = near_depth;
        return true;
    }
    if (!(depth > 0.0f) || !(clip.w > 0.0f)) return false;  // centre behind the camera plane, camera outside

    // Angular radius: tan(asin(r / d)) = r / sqrt(d^2 - r^2).
    const f32 t = radius / std::sqrt(dist * dist - radius * radius);
    const f32 sx = row_len(view_proj, 0), sy = row_len(view_proj, 1);
    const f32 ndc_x = clip.x / clip.w, ndc_y = clip.y / clip.w;
    const f32 rx = sx * t, ry = sy * t;  // radii in ndc
    if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y) || !std::isfinite(rx) || !std::isfinite(ry)) return false;
    if (std::fabs(ndc_x) - rx >= 1.0f || std::fabs(ndc_y) - ry >= 1.0f) return false;  // fully off screen
    out.center = math::vec2{ndc_x * 0.5f + 0.5f, ndc_y * 0.5f + 0.5f};
    out.radius = 0.5f * ry;  // ndc height 2 = one view height
    out.near_depth = near_depth;
    return true;
}

u32 heat_haze_frame(const HeatHaze& haze, const math::mat4& view_proj, const math::mat4& view, f32 aspect, f32 time_s,
                    HeatHazeFrame& out) {
    out = HeatHazeFrame{};
    out.time = std::isfinite(time_s) ? time_s : 0.0f;
    out.aspect = (std::isfinite(aspect) && aspect > 0.0f) ? aspect : 1.0f;
    const u32 n = haze.count < kHeatHazeMaxVolumes ? haze.count : kHeatHazeMaxVolumes;
    u32 kept = 0;
    for (u32 i = 0; i < n; ++i) {
        const HeatHazeVolume& v = haze.volumes[i];
        const f32 radius = detail::ranged(v.radius, 0.05f, 50.0f, 1.0f);
        HazeScreen s;
        if (!heat_haze_project(view_proj, view, v.center, radius, s)) continue;
        HeatHazeVolumeFrame& o = out.volumes[kept++];
        o.center = s.center;
        o.near_depth = s.near_depth;
        o.screen_radius = s.radius;
        o.strength = detail::ranged(v.strength, 0.0f, 2.0f, 0.5f);
        o.rise = detail::ranged(v.rise, 0.0f, 4.0f, 0.6f);
        o.scale = detail::ranged(v.scale, 0.25f, 4.0f, 1.0f);
        o.softness = detail::ranged(v.softness, 0.0f, 1.0f, 0.5f);
    }
    out.count = kept;
    return kept;
}

f32 heat_haze_falloff(f32 d_over_r, f32 softness) {
    if (!(d_over_r == d_over_r)) return 0.0f;
    const f32 d = d_over_r > 0.0f ? d_over_r : 0.0f;
    const f32 s = detail::ranged(softness, 0.0f, 1.0f, 0.5f);
    if (s <= 0.0f) return d < 1.0f ? 1.0f : 0.0f;
    const f32 lo = 1.0f - s;
    const f32 x = detail::ranged((d - lo) / s, 0.0f, 1.0f, 0.0f);
    const f32 w = 1.0f - x * x * (3.0f - 2.0f * x);
    return detail::ranged(w, 0.0f, 1.0f, 0.0f);
}

u32 heat_haze_hash(u32 x, u32 y, u32 seed) {
    u32 h = mix(x);
    h = mix(h ^ (y * 0x85ebca6bu));
    h = mix(h ^ (seed * 0xc2b2ae35u));
    return h;
}

}  // namespace mce::render
