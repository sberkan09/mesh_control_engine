// M-GI2: the irradiance volume's record helpers; kGiProjectFS / kGiVisFS (shaders3d.h) are their GPU side.
#include "mce/render/gi_probes.h"

#include <cmath>

namespace mce::render {

using math::vec3;

namespace {

f32 axis_of(vec3 v, u32 i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; }

vec3 column(const math::mat4& m, u32 c) { return {m.c[c][0], m.c[c][1], m.c[c][2]}; }

// Whether world point `p` lies inside the solid's oriented box (faces included); `local` gets p in model space.
bool solid_contains(const GiSolid& s, const math::mat4& inv, vec3 p, vec3& local) {
    local = math::transform_point(inv, p);
    return s.local.contains(local);
}

}  // namespace

GiMoments gi_record_at(const GiRecord& r, vec3 d) {
    const f32 len2 = math::dot(d, d);
    if (!(len2 > 0.0f)) return {r.mean_pos.x, r.square_pos.x};
    GiMoments m;
    for (u32 i = 0; i < 3; ++i) {
        const f32 di = axis_of(d, i);
        const f32 w = di * di / len2;
        m.mean += w * (di >= 0.0f ? axis_of(r.mean_pos, i) : axis_of(r.mean_neg, i));
        m.square += w * (di >= 0.0f ? axis_of(r.square_pos, i) : axis_of(r.square_neg, i));
    }
    return m;
}

f32 gi_chebyshev(f32 r, f32 mean, f32 square) {
    if (r <= mean) return 1.0f;
    const f32 var = math::max(square - mean * mean, kGiMinVariance);
    const f32 dr = r - mean;
    const f32 t = var / (var + dr * dr);
    return t * t * t;
}

f32 gi_lobe_weight(vec3 d, vec3 axis) {
    const f32 c = math::max(math::dot(d, axis), 0.0f);
    const f32 c2 = c * c;
    return c2 * c2;  // kGiVisLobePower = 4
}

vec3 gi_cell(const math::aabb3& bounds, const u32 (&counts)[3]) {
    const vec3 size = bounds.max - bounds.min;
    return {size.x / f32(math::max(counts[0], 2u) - 1), size.y / f32(math::max(counts[1], 2u) - 1),
            size.z / f32(math::max(counts[2], 2u) - 1)};
}

f32 gi_dmax(vec3 cell) { return math::max(kGiVisRangeCells * math::length(cell), 1e-3f); }

f32 gi_spacing(vec3 cell) { return math::min(math::min(cell.x, cell.y), cell.z); }

vec3 gi_relocate(vec3 g, vec3 cell, f32 max_offset, Span<const GiSolid> solids) {
    const f32 mo = max_offset == max_offset ? math::clamp(max_offset, 0.0f, kGiMaxOffsetLimit) : 0.0f;
    const vec3 none{0.0f, 0.0f, 0.0f};
    if (solids.empty() || !(mo > 0.0f)) return none;
    Array<math::mat4> inv(mem::heap());
    inv.reserve(solids.size());
    for (const GiSolid& s : solids) inv.push_back(math::inverse(s.model));
    vec3 p = g;
    bool moved = false;
    for (u32 round = 0; round < 3; ++round) {
        vec3 push{0.0f, 0.0f, 0.0f};
        bool inside = false;
        for (usize k = 0; k < solids.size(); ++k) {
            const GiSolid& s = solids[k];
            if (math::length_sq(p - s.center) > s.radius * s.radius) continue;
            vec3 q;
            if (!solid_contains(s, inv[k], p, q)) continue;
            inside = true;
            // The nearest face in world metres (each model axis scaled by its column's length).
            f32 best = 3.0e38f;
            vec3 best_push{0.0f, 0.0f, 0.0f};
            for (u32 i = 0; i < 3; ++i) {
                const vec3 col = column(s.model, i);
                const f32 scale = math::length(col);
                if (!(scale > 0.0f)) continue;
                const vec3 dir = col * (1.0f / scale);
                const f32 to_min = (axis_of(q, i) - axis_of(s.local.min, i)) * scale;
                const f32 to_max = (axis_of(s.local.max, i) - axis_of(q, i)) * scale;
                if (to_max < best) {
                    best = to_max;
                    best_push = dir * (to_max + kGiRelocateMargin);
                }
                if (to_min < best) {
                    best = to_min;
                    best_push = dir * -(to_min + kGiRelocateMargin);
                }
            }
            push = push + best_push;
        }
        if (!inside) break;
        p = p + push;
        moved = true;
    }
    if (!moved) return none;
    for (usize k = 0; k < solids.size(); ++k) {
        vec3 q;
        if (solid_contains(solids[k], inv[k], p, q)) return none;
    }
    const vec3 off = p - g;
    for (u32 i = 0; i < 3; ++i)
        if (std::fabs(axis_of(off, i)) > mo * axis_of(cell, i)) return none;
    return off;
}

GiFollowCell gi_follow_origin(f64 camera_x, f64 camera_z, f64 cell_x, f64 cell_z, u32 count_x, u32 count_z) {
    const auto snap = [](f64 c, f64 cell, u32 count) {
        const i64 at = cell > 0.0 ? static_cast<i64>(std::floor(c / cell)) : 0;
        return at - static_cast<i64>((math::max(count, 2u) - 1) / 2);
    };
    return {snap(camera_x, cell_x, count_x), snap(camera_z, cell_z, count_z)};
}

u32 gi_slot(i64 i, u32 count) {
    if (count == 0) return 0;
    const i64 c = static_cast<i64>(count);
    const i64 m = i % c;
    return static_cast<u32>(m < 0 ? m + c : m);
}

f32 gi_slot_offset_pack(u32 ox, u32 oy, u32 oz) { return f32((ox & 31u) + 32u * (oy & 31u) + 1024u * (oz & 31u)); }

void gi_slot_offset_unpack(f32 packed, u32 (&o)[3]) {
    const u32 v = packed > 0.0f ? static_cast<u32>(packed) : 0u;
    o[0] = v & 31u;
    o[1] = (v >> 5) & 31u;
    o[2] = (v >> 10) & 31u;
}

}  // namespace mce::render
