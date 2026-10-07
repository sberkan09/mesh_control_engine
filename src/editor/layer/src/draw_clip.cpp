#include "mce/editor/draw_clip.h"

#include <algorithm>

namespace mce::editor {
namespace {

ClipVertex lerp_vertex(const ClipVertex& a, const ClipVertex& b, f32 t) {
    ClipVertex r;
    r.x = a.x + (b.x - a.x) * t;
    r.y = a.y + (b.y - a.y) * t;
    r.u = a.u + (b.u - a.u) * t;
    r.v = a.v + (b.v - a.v) * t;
    r.rgba = 0;
    for (u32 shift = 0; shift < 32; shift += 8) {
        const f32 ca = static_cast<f32>((a.rgba >> shift) & 0xFFu);
        const f32 cb = static_cast<f32>((b.rgba >> shift) & 0xFFu);
        const u32 c = static_cast<u32>(ca + (cb - ca) * t + 0.5f);
        r.rgba |= std::min(c, 255u) << shift;
    }
    return r;
}

// One Sutherland-Hodgman stage: keeps the part of `in` where sign * (coord - edge) >= 0.
u32 clip_edge(const ClipVertex* in, u32 n, ClipVertex* out, bool use_x, f32 edge, f32 sign) {
    auto dist = [&](const ClipVertex& v) { return sign * ((use_x ? v.x : v.y) - edge); };
    u32 m = 0;
    for (u32 i = 0; i < n; ++i) {
        const ClipVertex& a = in[i];
        const ClipVertex& b = in[(i + 1) % n];
        const f32 da = dist(a);
        const f32 db = dist(b);
        if (da >= 0) out[m++] = a;
        if ((da >= 0) != (db >= 0)) out[m++] = lerp_vertex(a, b, da / (da - db));
    }
    return m;
}

}  // namespace

u32 clip_triangles(Span<const ClipVertex> verts, u32 out_base, Span<const u16> indices, ClipRect rect,
                   Array<ClipVertex>& out_vertices, Array<u32>& out_indices) {
    if (rect.empty()) return 0;
    const usize start = out_indices.size();
    for (usize t = 0; t + 2 < indices.size(); t += 3) {
        const u16 i0 = indices[t], i1 = indices[t + 1], i2 = indices[t + 2];
        const ClipVertex& a = verts[i0];
        const ClipVertex& b = verts[i1];
        const ClipVertex& c = verts[i2];
        const f32 min_x = std::min({a.x, b.x, c.x}), max_x = std::max({a.x, b.x, c.x});
        const f32 min_y = std::min({a.y, b.y, c.y}), max_y = std::max({a.y, b.y, c.y});
        if (max_x <= rect.x0 || min_x >= rect.x1 || max_y <= rect.y0 || min_y >= rect.y1) continue;
        if (min_x >= rect.x0 && max_x <= rect.x1 && min_y >= rect.y0 && max_y <= rect.y1) {
            out_indices.push_back(out_base + i0);
            out_indices.push_back(out_base + i1);
            out_indices.push_back(out_base + i2);
            continue;
        }
        // A triangle cut by 4 half-planes has at most 7 vertices.
        ClipVertex poly_a[8] = {a, b, c};
        ClipVertex poly_b[8];
        u32 n = 3;
        n = clip_edge(poly_a, n, poly_b, true, rect.x0, 1.0f);
        n = clip_edge(poly_b, n, poly_a, true, rect.x1, -1.0f);
        n = clip_edge(poly_a, n, poly_b, false, rect.y0, 1.0f);
        n = clip_edge(poly_b, n, poly_a, false, rect.y1, -1.0f);
        if (n < 3) continue;
        const u32 first = static_cast<u32>(out_vertices.size());
        for (u32 i = 0; i < n; ++i) out_vertices.push_back(poly_a[i]);
        for (u32 i = 1; i + 1 < n; ++i) {  // convex polygon: fan
            out_indices.push_back(first);
            out_indices.push_back(first + i);
            out_indices.push_back(first + i + 1);
        }
    }
    return static_cast<u32>(out_indices.size() - start);
}

}  // namespace mce::editor
