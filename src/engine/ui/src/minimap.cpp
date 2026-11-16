// The Minimap widget's projection (M-MISSION step 2; mce/ui/minimap.h).

#include "mce/ui/minimap.h"

#include <cmath>

namespace mce::ui {

MinimapPoint minimap_project(vec2 world, vec2 tracked, f32 heading, bool rotate_with_heading, f32 radius, const Rect& rect,
                             bool clamp_to_rim) {
    MinimapPoint out;
    const vec2 size = rect.size();
    const f32 rim_px = 0.5f * (size.x < size.y ? size.x : size.y);
    const vec2 centre = rect.center();
    const f32 r = radius > 0.0f ? radius : 1.0f;
    const f32 dx = world.x - tracked.x, dz = world.y - tracked.y;
    f32 sx = dx, sy = dz;  // north up: east right, south down
    if (rotate_with_heading) {
        const f32 s = std::sin(heading), c = std::cos(heading);
        // forward = (-s, -c), right = (c, -s); forward is up on the screen.
        sx = dx * c - dz * s;
        sy = dx * s + dz * c;
    }
    const f32 dist = std::sqrt(sx * sx + sy * sy);
    out.inside = dist <= r;
    f32 k = rim_px / r;
    if (!out.inside && clamp_to_rim) {
        k = rim_px / dist;  // onto the circle, along the line from the centre
        out.on_rim = true;
    }
    out.point = vec2(centre.x + sx * k, centre.y + sy * k);
    return out;
}

MinimapPoint minimap_project_full(vec2 world, vec2 world_min, vec2 world_max, const Rect& rect) {
    MinimapPoint out;
    const vec2 size = rect.size();
    const f32 w = world_max.x - world_min.x, h = world_max.y - world_min.y;
    const f32 u = w > 0.0f ? (world.x - world_min.x) / w : 0.5f;
    const f32 v = h > 0.0f ? (world.y - world_min.y) / h : 0.5f;
    out.inside = u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;
    out.point = vec2(rect.min.x + u * size.x, rect.min.y + v * size.y);
    return out;
}

void minimap_layout(const Minimap& m, const MinimapView& view, Span<const MinimapMarker> markers, const Rect& rect,
                    Array<MinimapShown>& out) {
    out.clear();
    const f32 radius = minimap_radius(m);
    for (u32 i = 0; i < markers.size(); ++i) {
        const MinimapMarker& k = markers[i];
        if (k.layer < 32 && (m.layer_mask & (1u << k.layer)) == 0) continue;
        MinimapPoint p;
        if (m.full_map) {
            p = minimap_project_full(k.pos, m.world_min, m.world_max, rect);
        } else {
            p = minimap_project(k.pos, view.tracked, view.heading, m.rotate_with_heading, radius, rect,
                                m.clamp_to_rim || k.always_visible);
        }
        if (!p.inside && !p.on_rim) continue;
        out.push_back(MinimapShown{i, p.point, p.on_rim});
    }
}

}  // namespace mce::ui
