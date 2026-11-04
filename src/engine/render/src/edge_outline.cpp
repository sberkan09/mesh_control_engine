#include "mce/render/edge_outline.h"

#include <cmath>

#include "mce/render/scene3d.h"
#include "unit_clamp.h"

namespace mce::render {

namespace {

f32 smooth_step(f32 a, f32 b, f32 x) {
    const f32 t = detail::ranged((x - a) / (b - a), 0.0f, 1.0f, 0.0f);
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

u32 edge_outline_tap_count(bool wide) { return wide ? 8u : 4u; }

f32 edge_outline_depth_term(f32 d_center, f32 d_tap, f32 threshold, f32 n_dot_v, bool sky_silhouette) {
    if (d_center != d_center || d_tap != d_tap) return 0.0f;
    if (!(d_center > 0.0f)) return 0.0f;
    if (!(d_tap > 0.0f)) return sky_silhouette ? 1.0f : 0.0f;
    if (!(d_tap > d_center)) return 0.0f;
    const f32 grazing = n_dot_v > 0.2f ? n_dot_v : 0.2f;
    return smooth_step(threshold, 2.0f * threshold, (d_tap - d_center) / d_center * grazing);
}

f32 edge_outline_normal_term(math::vec3 n_center, math::vec3 n_tap, f32 threshold) {
    const f32 len2_center = n_center.x * n_center.x + n_center.y * n_center.y + n_center.z * n_center.z;
    const f32 len2_tap = n_tap.x * n_tap.x + n_tap.y * n_tap.y + n_tap.z * n_tap.z;
    if (!(len2_center >= 0.25f) || !(len2_tap >= 0.25f)) return 0.0f;
    const f32 d = n_center.x * n_tap.x + n_center.y * n_tap.y + n_center.z * n_tap.z;
    return smooth_step(threshold, 1.5f * threshold, 1.0f - d);
}

f32 edge_outline_fade(f32 view_depth, f32 fade_start, f32 fade_end, f32 fog_density) {
    if (!std::isfinite(view_depth)) return 0.0f;
    const f32 fog = fog_density == fog_density ? fog_density * view_depth : 0.0f;
    return (1.0f - smooth_step(fade_start, fade_end, view_depth)) * std::exp(-(fog * fog));
}

EdgeOutlineFrame edge_outline_frame(const EdgeOutline& outline, u32 width, u32 height, f32 fog_density, bool wide) {
    EdgeOutlineFrame frame;
    if (!outline.enabled || width == 0u || height == 0u) return frame;
    const f32 values[] = {outline.color.x,         outline.color.y,        outline.color.z,       outline.opacity,
                          outline.thickness,       outline.depth_threshold, outline.normal_threshold,
                          outline.depth_weight,    outline.normal_weight,  outline.fade_start,
                          outline.fade_end,        fog_density};
    for (const f32 v : values)
        if (!std::isfinite(v)) return frame;

    const f32 opacity = detail::ranged(outline.opacity, 0.0f, 1.0f, 1.0f);
    const f32 depth_weight = detail::ranged(outline.depth_weight, 0.0f, 1.0f, 1.0f);
    const f32 normal_weight = detail::ranged(outline.normal_weight, 0.0f, 1.0f, 1.0f);
    const f32 fade_start = detail::ranged(outline.fade_start, 0.0f, 10000.0f, 30.0f);
    f32 fade_end = detail::ranged(outline.fade_end, 0.0f, 10000.0f, 80.0f);
    if (fade_end < fade_start) fade_end = fade_start;
    if (!(opacity > 0.0f) || (!(depth_weight > 0.0f) && !(normal_weight > 0.0f)) || !(fade_end > 0.0f)) return frame;

    const f32 thickness = detail::ranged(outline.thickness, 0.5f, 4.0f, 1.0f);
    frame.color_opacity = math::vec4{detail::ranged(outline.color.x, 0.0f, 16.0f, 0.02f),
                                     detail::ranged(outline.color.y, 0.0f, 16.0f, 0.02f),
                                     detail::ranged(outline.color.z, 0.0f, 16.0f, 0.025f), opacity};
    frame.tap_uv = math::vec2{thickness / static_cast<f32>(width), thickness / static_cast<f32>(height)};
    frame.depth_threshold = detail::ranged(outline.depth_threshold, 0.001f, 1.0f, 0.05f);
    frame.normal_threshold = detail::ranged(outline.normal_threshold, 0.01f, 2.0f, 0.4f);
    frame.depth_weight = depth_weight;
    frame.normal_weight = normal_weight;
    frame.fade_start = fade_start;
    frame.fade_end = fade_end;
    frame.fog_density = fog_density > 0.0f ? fog_density : 0.0f;
    frame.taps = edge_outline_tap_count(wide);
    frame.sky_silhouette = outline.sky_silhouette;
    frame.strength = opacity;
    return frame;
}

}  // namespace mce::render
