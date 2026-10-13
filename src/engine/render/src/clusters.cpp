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

ClusterLights::ClusterLights(mem::IAllocator& alloc)
    : gpu_lights_(alloc), ranges_(alloc), indices_(alloc), pairs_(alloc), bounds_(alloc), culled_(alloc) {}

void ClusterLights::clear() {
    gpu_lights_.clear();
    ranges_.clear();
    indices_.clear();
    pairs_.clear();
    bounds_.clear();
    culled_.clear();
    assigned_ = false;
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

// Squared distance from v to the interval [lo, hi].
f32 dist_sq_range(f32 v, f32 lo, f32 hi) {
    const f32 d = v < lo ? lo - v : (v > hi ? v - hi : 0.0f);
    return d * d;
}

u32 tile_floor(f32 ndc, u32 tiles) {
    const f32 t = std::floor((clamp(ndc, -1.0f, 1.0f) * 0.5f + 0.5f) * f32(tiles));
    return static_cast<u32>(clamp(t, 0.0f, f32(tiles - 1)));
}

// View-space extent of tile column/row t over the depth range [dn, df] (the frustum wedge's AABB).
void tile_extent(u32 t, u32 tiles, f32 scale, f32 dn, f32 df, f32& lo, f32& hi) {
    const f32 n0 = f32(t) / f32(tiles) * 2.0f - 1.0f, n1 = f32(t + 1) / f32(tiles) * 2.0f - 1.0f;
    lo = min(n0 * dn, n0 * df) / scale;
    hi = max(n1 * dn, n1 * df) / scale;
}

}  // namespace

void ClusterLights::build(const View3D& view, Span<const PointLight> lights, const ClusterGridDesc& grid) {
    prepare(view, lights, grid);
    assign();
}

void ClusterLights::prepare(const View3D& view, Span<const PointLight> lights, const ClusterGridDesc& grid) {
    clear();
    grid_ = grid;
    grid_.x = max(grid_.x, 1u);
    grid_.y = max(grid_.y, 1u);
    grid_.z = clamp(grid_.z, 1u, 256u);
    const f32 zn = max(view.near_z, 1e-4f), zf = max(view.far_z, zn * 1.001f);
    near_ = zn;
    far_ = zf;
    const f32 log_ratio = std::log(zf / zn);
    slice_scale_ = f32(grid_.z) / log_ratio;
    slice_bias_ = -f32(grid_.z) * std::log(zn) / log_ratio;
    stats_.lights = static_cast<u32>(lights.size());

    // Tile extents per slice, then the depth at the start of each slice (slice k spans
    // [edge[k], edge[k + 1]]).
    proj_x_ = view.proj.c[0][0];
    proj_y_ = view.proj.c[1][1];
    bounds_.resize(usize(edges_offset()) + grid_.z + 1);
    f32* edge = bounds_.data() + edges_offset();
    for (u32 k = 0; k <= grid_.z; ++k) edge[k] = zn * std::pow(zf / zn, f32(k) / f32(grid_.z));
    f32* tile_x = bounds_.data();
    f32* tile_y = bounds_.data() + tile_bounds_y_offset();
    for (u32 k = 0; k < grid_.z; ++k) {
        for (u32 t = 0; t < grid_.x; ++t) tile_extent(t, grid_.x, proj_x_, edge[k], edge[k + 1], tile_x[(usize(k) * grid_.x + t) * 2], tile_x[(usize(k) * grid_.x + t) * 2 + 1]);
        for (u32 t = 0; t < grid_.y; ++t) tile_extent(t, grid_.y, proj_y_, edge[k], edge[k + 1], tile_y[(usize(k) * grid_.y + t) * 2], tile_y[(usize(k) * grid_.y + t) * 2 + 1]);
    }

    gpu_lights_.reserve(lights.size());
    culled_.reserve(lights.size());
    for (u32 li = 0; li < lights.size(); ++li) {
        const PointLight& l = lights[li];
        GpuPointLight g;
        const f32 r = max(l.radius, 1e-3f);
        g.position_radius = vec4(l.position, r);
        g.color = vec4(l.color * l.intensity, 1.0f / (r * r));
        if (l.spot_outer > 0.0f) {  // smooth cone: 0 at the outer angle, 1 inside the inner one
            const f32 co = std::cos(min(l.spot_outer, 3.1f)), ci = std::cos(clamp(l.spot_inner, 0.0f, min(l.spot_outer, 3.1f)));
            const f32 scale = 1.0f / max(ci - co, 1e-4f);
            g.spot = vec4(normalize(l.direction) * scale, -co * scale);
        }
        gpu_lights_.push_back(g);
        if (!(l.intensity > 0.0f) || !sphere_visible(view, l.position, r)) continue;
        ClusterCullLight c;
        c.center_radius = vec4(transform_point(view.view, l.position), r);  // view space, camera looks down -Z
        c.light = li;
        culled_.push_back(c);
    }
}

void ClusterLights::assign() {
    if (assigned_ || bounds_.empty()) return;
    assigned_ = true;
    const u32 clusters = cluster_count();
    ranges_.resize(clusters);
    for (ClusterRange& r : ranges_) r = {0, 0};
    const f32 zn = near_, zf = far_;
    const f32* edge = bounds_.data() + edges_offset();
    const f32* tile_x = bounds_.data();
    const f32* tile_y = bounds_.data() + tile_bounds_y_offset();

    // Scenes are usually rebuilt per frame on a frame allocator: size the scratch once up front
    // (a light rarely touches more than ~128 clusters) instead of growing it pair by pair.
    pairs_.reserve(min(usize(grid_.max_indices), culled_.size() * 128) * 2);
    for (const ClusterCullLight& cl : culled_) {
        const vec3 c{cl.center_radius.x, cl.center_radius.y, cl.center_radius.z};
        const f32 r = cl.center_radius.w;
        const f32 d = -c.z;
        f32 x0, x1, y0, y1;
        if (!sphere_ndc_bounds(c.x, d, r, zn, proj_x_, x0, x1) || !sphere_ndc_bounds(c.y, d, r, zn, proj_y_, y0, y1)) continue;
        if (x1 < -1.0f || x0 > 1.0f || y1 < -1.0f || y0 > 1.0f) continue;
        const u32 tx0 = tile_floor(x0, grid_.x), tx1 = tile_floor(x1, grid_.x);
        const u32 ty0 = tile_floor(y0, grid_.y), ty1 = tile_floor(y1, grid_.y);
        const u32 s0 = slice_of(max(d - r, zn)), s1 = slice_of(min(d + r, zf));
        bool touched = false;
        const f32 r2 = r * r;
        // The cluster AABB test is separable: depth per slice, y per row, x per column.
        for (u32 s = s0; s <= s1; ++s) {
            const f32 dz2 = dist_sq_range(c.z, -edge[s + 1], -edge[s]);
            if (dz2 > r2) continue;
            const f32* xb = &tile_x[usize(s) * grid_.x * 2];
            const f32* yb = &tile_y[usize(s) * grid_.y * 2];
            for (u32 ty = ty0; ty <= ty1; ++ty) {
                const f32 dyz2 = dz2 + dist_sq_range(c.y, yb[ty * 2], yb[ty * 2 + 1]);
                if (dyz2 > r2) continue;
                // Column extents grow monotonically with tx, so the hits on this row are one run.
                const f32 w = std::sqrt(r2 - dyz2);
                const u32 row = (s * grid_.y + ty) * grid_.x;
                for (u32 tx = tx0; tx <= tx1; ++tx) {
                    if (xb[tx * 2 + 1] < c.x - w) continue;
                    if (xb[tx * 2] > c.x + w) break;
                    if (pairs_.size() >= usize(grid_.max_indices) * 2) {
                        ++stats_.dropped;
                        continue;
                    }
                    pairs_.push_back(row + tx);
                    pairs_.push_back(cl.light);
                    ++ranges_[row + tx].count;
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
