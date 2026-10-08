#include "mce/render/clusters.h"

#include <cmath>

#include "mce/render/scene3d.h"

namespace mce::render {

using namespace mce::math;

bool sphere_ndc_bounds(f32 a, f32 d, f32 r, f32 n, f32 s, f32& lo, f32& hi) {
    if (d + r < n) return false;  // entirely behind the near plane
    constexpr f32 kHuge = 1e30f;
    const f32 l2 = a * a + d * d;
    if (l2 <= r * r) {  // the eye is inside the sphere's projection on this plane: everything
        lo = -kHuge;
        hi = kHuge;
        return true;
    }
    lo = kHuge;
    hi = -kHuge;
    // Tangent points from the eye: T = (t / l²) (t C ± r perp(C)), C = (a, d), perp(C) = (-d, a).
    const f32 t = std::sqrt(l2 - r * r);
    for (f32 sign : {-1.0f, 1.0f}) {
        const f32 ta = (t / l2) * (t * a - sign * r * d);
        const f32 td = (t / l2) * (t * d + sign * r * a);
        if (td < n) continue;  // clipped: the near-plane chord below covers this side
        const f32 x = s * ta / td;
        lo = min(lo, x);
        hi = max(hi, x);
    }
    if (d - r < n) {  // the sphere crosses the near plane: the clipped cap's chord bounds it too
        const f32 h = std::sqrt(max(r * r - (d - n) * (d - n), 0.0f));
        for (f32 e : {a - h, a + h}) {
            const f32 x = s * e / n;
            lo = min(lo, x);
            hi = max(hi, x);
        }
    }
    return lo <= hi;
}

ClusterLights::ClusterLights(mem::IAllocator& alloc) : gpu_lights_(alloc), ranges_(alloc), indices_(alloc), pairs_(alloc) {}

void ClusterLights::clear() {
    gpu_lights_.clear();
    ranges_.clear();
    indices_.clear();
    pairs_.clear();
    stats_ = {};
}

u32 ClusterLights::slice_of(f32 depth) const {
    if (!(depth > 0.0f)) return 0;
    const f32 s = std::floor(std::log(depth) * slice_scale_ + slice_bias_);
    return static_cast<u32>(clamp(s, 0.0f, f32(grid_.z - 1)));
}

Span<const u32> ClusterLights::lights_in(u32 c) const {
    if (c >= ranges_.size()) return {};
    return indices_.span().subspan(ranges_[c].offset, ranges_[c].count);
}

namespace {

// Squared distance from p to the box [lo, hi].
f32 dist_sq_box(vec3 p, vec3 lo, vec3 hi) {
    f32 d = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const f32 v = p[usize(i)] < lo[usize(i)] ? lo[usize(i)] - p[usize(i)] : (p[usize(i)] > hi[usize(i)] ? p[usize(i)] - hi[usize(i)] : 0.0f);
        d += v * v;
    }
    return d;
}

u32 tile_floor(f32 ndc, u32 tiles) {
    const f32 t = std::floor((clamp(ndc, -1.0f, 1.0f) * 0.5f + 0.5f) * f32(tiles));
    return static_cast<u32>(clamp(t, 0.0f, f32(tiles - 1)));
}

}  // namespace

void ClusterLights::build(const View3D& view, Span<const PointLight> lights, const ClusterGridDesc& grid) {
    clear();
    grid_ = grid;
    grid_.x = max(grid_.x, 1u);
    grid_.y = max(grid_.y, 1u);
    grid_.z = clamp(grid_.z, 1u, 256u);
    const f32 zn = max(view.near_z, 1e-4f), zf = max(view.far_z, zn * 1.001f);
    const f32 log_ratio = std::log(zf / zn);
    slice_scale_ = f32(grid_.z) / log_ratio;
    slice_bias_ = -f32(grid_.z) * std::log(zn) / log_ratio;
    stats_.lights = static_cast<u32>(lights.size());
    const u32 clusters = cluster_count();
    ranges_.resize(clusters);
    for (ClusterRange& r : ranges_) r = {0, 0};

    // Depth at the start of each slice (slice k spans [edge[k], edge[k + 1]]).
    f32 edge[257];
    for (u32 k = 0; k <= grid_.z; ++k) edge[k] = zn * std::pow(zf / zn, f32(k) / f32(grid_.z));
    const f32 px = view.proj.c[0][0], py = view.proj.c[1][1];

    gpu_lights_.reserve(lights.size());
    for (const PointLight& l : lights) {
        GpuPointLight g;
        const f32 r = max(l.radius, 1e-3f);
        g.position_radius = vec4(l.position, r);
        g.color = vec4(l.color * l.intensity, 1.0f / (r * r));
        gpu_lights_.push_back(g);
    }

    for (u32 li = 0; li < lights.size(); ++li) {
        const PointLight& l = lights[li];
        const f32 r = max(l.radius, 1e-3f);
        if (!(l.intensity > 0.0f) || !sphere_visible(view, l.position, r)) continue;
        const vec3 c = transform_point(view.view, l.position);  // view space, camera looks down -Z
        const f32 d = -c.z;
        f32 x0, x1, y0, y1;
        if (!sphere_ndc_bounds(c.x, d, r, zn, px, x0, x1) || !sphere_ndc_bounds(c.y, d, r, zn, py, y0, y1)) continue;
        if (x1 < -1.0f || x0 > 1.0f || y1 < -1.0f || y0 > 1.0f) continue;
        const u32 tx0 = tile_floor(x0, grid_.x), tx1 = tile_floor(x1, grid_.x);
        const u32 ty0 = tile_floor(y0, grid_.y), ty1 = tile_floor(y1, grid_.y);
        const u32 s0 = slice_of(max(d - r, zn)), s1 = slice_of(min(d + r, zf));
        bool touched = false;
        for (u32 s = s0; s <= s1; ++s) {
            const f32 dn = edge[s], df = edge[s + 1];
            for (u32 ty = ty0; ty <= ty1; ++ty) {
                // View-space y range of the tile row over the slice's depth range.
                const f32 ny0 = f32(ty) / f32(grid_.y) * 2.0f - 1.0f, ny1 = f32(ty + 1) / f32(grid_.y) * 2.0f - 1.0f;
                const f32 ylo = min(ny0 * dn, ny0 * df) / py, yhi = max(ny1 * dn, ny1 * df) / py;
                for (u32 tx = tx0; tx <= tx1; ++tx) {
                    const f32 nx0 = f32(tx) / f32(grid_.x) * 2.0f - 1.0f, nx1 = f32(tx + 1) / f32(grid_.x) * 2.0f - 1.0f;
                    const f32 xlo = min(nx0 * dn, nx0 * df) / px, xhi = max(nx1 * dn, nx1 * df) / px;
                    if (dist_sq_box(c, {xlo, ylo, -df}, {xhi, yhi, -dn}) > r * r) continue;
                    const u32 ci = cluster_index(tx, ty, s);
                    if (pairs_.size() / 2 >= grid_.max_indices) {
                        ++stats_.dropped;
                        continue;
                    }
                    pairs_.push_back(ci);
                    pairs_.push_back(li);
                    ++ranges_[ci].count;
                    touched = true;
                }
            }
        }
        if (touched) ++stats_.visible;
    }

    // Prefix sum, then scatter in submission order (pairs are in light order already).
    u32 offset = 0;
    for (ClusterRange& rg : ranges_) {
        rg.offset = offset;
        offset += rg.count;
        stats_.max_per_cluster = max(stats_.max_per_cluster, rg.count);
        rg.count = 0;
    }
    indices_.resize(offset);
    for (usize i = 0; i + 1 < pairs_.size(); i += 2) {
        ClusterRange& rg = ranges_[pairs_[i]];
        indices_[rg.offset + rg.count++] = pairs_[i + 1];
    }
    stats_.indices = offset;
}

}  // namespace mce::render
