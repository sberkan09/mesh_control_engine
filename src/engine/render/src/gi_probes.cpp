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

f32 gi_visibility_weight(const GiRecord& r, f32 back_share, vec3 v, f32 dmax) {
    if (back_share >= kGiBackfaceOff) return 0.0f;
    const vec3 means = r.mean_pos + r.mean_neg;
    if (means.x + means.y + means.z <= 0.0f) return 0.0f;  // a probe not captured yet
    f32 vis = 1.0f;
    for (u32 i = 0; i < 3; ++i) {
        const f32 vi = axis_of(v, i);
        const bool pos = vi >= 0.0f;
        const f32 mean = axis_of(pos ? r.mean_pos : r.mean_neg, i) * dmax;
        const f32 square = axis_of(pos ? r.square_pos : r.square_neg, i) * dmax * dmax;
        vis = math::min(vis, gi_chebyshev(std::fabs(vi), mean, square));
    }
    return vis < kGiCrush ? vis * vis * vis / (kGiCrush * kGiCrush) : vis;
}

bool gi_blend(const f32 (&base)[8], const f32 (&vis)[8], f32 (&out)[8]) {
    f32 vm = 0.0f, wsum = 0.0f;
    for (u32 k = 0; k < 8; ++k) {
        if (base[k] > 0.0f) vm = math::max(vm, vis[k]);
        out[k] = base[k] * vis[k];
        wsum += out[k];
    }
    if (vm < kGiNoProbe) {
        for (f32& w : out) w = 0.0f;
        return false;
    }
    for (f32& w : out) w /= wsum;
    return true;
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
    // A solid is tested only when its bounding sphere holds the point (the sphere encloses the box), so the inverse is
    // taken for those only (M-GI2 50f: the fill relocates every probe against every large draw).
    const auto inside_of = [](const GiSolid& s, vec3 p, vec3& q) {
        return math::length_sq(p - s.center) <= s.radius * s.radius && solid_contains(s, math::inverse(s.model), p, q);
    };
    vec3 p = g;
    bool moved = false;
    for (u32 round = 0; round < 3; ++round) {
        vec3 push{0.0f, 0.0f, 0.0f};
        bool inside = false;
        for (usize k = 0; k < solids.size(); ++k) {
            const GiSolid& s = solids[k];
            vec3 q;
            if (!inside_of(s, p, q)) continue;
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
    for (const GiSolid& s : solids) {
        vec3 q;
        if (inside_of(s, p, q)) return none;
    }
    const vec3 off = p - g;
    for (u32 i = 0; i < 3; ++i)
        if (std::fabs(axis_of(off, i)) > mo * axis_of(cell, i)) return none;
    return off;
}

GiFollowCell gi_follow_origin(f64 camera_x, f64 camera_z, f64 cell_x, f64 cell_z, u32 count_x, u32 count_z) {
    // An even count's box centre lies half a cell past a probe: the camera's cell (floor) puts the camera within half a
    // cell of it. An odd count's centre is a probe: the nearest probe (floor of c + 0.5) does.
    const auto snap = [](f64 c, f64 cell, u32 count) {
        const u32 n = math::max(count, 2u);
        const i64 at = cell > 0.0 ? static_cast<i64>(std::floor(c / cell + ((n & 1u) ? 0.5 : 0.0))) : 0;
        return at - static_cast<i64>((n - 1) / 2);
    };
    return {snap(camera_x, cell_x, count_x), snap(camera_z, cell_z, count_z)};
}

GiFollowCell gi_follow_step(GiFollowCell at, f64 camera_x, f64 camera_z, f64 cell_x, f64 cell_z, u32 count_x,
                            u32 count_z) {
    const GiFollowCell snapped = gi_follow_origin(camera_x, camera_z, cell_x, cell_z, count_x, count_z);
    // The box centre in cells: the first column plus half the box ((count - 1) / 2 cells).
    const auto keep = [](i64 first, f64 c, f64 cell, u32 count) {
        if (!(cell > 0.0)) return true;
        const f64 centre = f64(first) + 0.5 * f64(math::max(count, 2u) - 1);
        return std::fabs(c / cell - centre) <= kGiFollowHysteresis;
    };
    return {keep(at.x, camera_x, cell_x, count_x) ? at.x : snapped.x, keep(at.z, camera_z, cell_z, count_z) ? at.z : snapped.z};
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

f64 gi_follow_spacing(f32 lo, f32 hi, u32 count) {
    const f64 cell = (f64(hi) - f64(lo)) / f64(math::max(count, 2u) - 1);
    return math::max(std::round(cell * 1000.0), 1.0) / 1000.0;  // whole millimetres, at least one
}

void gi_slot_offset_of(GiFollowCell f, const u32 (&counts)[3], u32 (&o)[3]) {
    o[0] = gi_slot(f.x, counts[0]);
    o[1] = 0;
    o[2] = gi_slot(f.z, counts[2]);
}

math::aabb3 gi_follow_box(GiFollowCell f, const math::aabb3& bounds, const u32 (&counts)[3], f64 ox, f64 oz) {
    const f64 sx = gi_follow_spacing(bounds.min.x, bounds.max.x, counts[0]);
    const f64 sz = gi_follow_spacing(bounds.min.z, bounds.max.z, counts[2]);
    const f64 x0 = f64(f.x) * sx - ox, z0 = f64(f.z) * sz - oz;
    const f64 x1 = f64(f.x + i64(math::max(counts[0], 2u)) - 1) * sx - ox;
    const f64 z1 = f64(f.z + i64(math::max(counts[2], 2u)) - 1) * sz - oz;
    return {{f32(x0), bounds.min.y, f32(z0)}, {f32(x1), bounds.max.y, f32(z1)}};
}

GiScroll gi_scroll(GiFollowCell from, GiFollowCell to, const u32 (&counts)[3]) {
    GiScroll s;
    const i64 dx = to.x - from.x, dz = to.z - from.z;
    const i64 cx = i64(counts[0]), cz = i64(counts[2]);
    if (dx >= cx || -dx >= cx || dz >= cz || -dz >= cz) {
        s.refill = true;
        s.probes = counts[0] * counts[1] * counts[2];
        return s;
    }
    // Moving by +k, the columns from.x .. from.x + k - 1 leave and from.x + c .. from.x + c + k - 1 arrive: the same
    // slots. Moving by -k, to.x .. to.x + k - 1 arrive (and to.x + c .. leave): the same slots again.
    s.x_count = u32(dx < 0 ? -dx : dx);
    s.x_first = s.x_count ? gi_slot(dx > 0 ? from.x : to.x, counts[0]) : 0;
    s.z_count = u32(dz < 0 ? -dz : dz);
    s.z_first = s.z_count ? gi_slot(dz > 0 ? from.z : to.z, counts[2]) : 0;
    s.probes = (s.x_count * counts[2] + s.z_count * counts[0] - s.x_count * s.z_count) * counts[1];
    return s;
}

vec3 gi_offset_encode(vec3 offset, vec3 cell) {
    const auto e = [](f32 o, f32 c) {
        return c > 0.0f ? math::clamp(0.5f + 0.5f * o / (kGiMaxOffsetLimit * c), 0.0f, 1.0f) : 0.5f;
    };
    return {e(offset.x, cell.x), e(offset.y, cell.y), e(offset.z, cell.z)};
}

vec3 gi_offset_decode(vec3 texel, vec3 cell) {
    return {(texel.x * 2.0f - 1.0f) * kGiMaxOffsetLimit * cell.x, (texel.y * 2.0f - 1.0f) * kGiMaxOffsetLimit * cell.y,
            (texel.z * 2.0f - 1.0f) * kGiMaxOffsetLimit * cell.z};
}

}  // namespace mce::render
