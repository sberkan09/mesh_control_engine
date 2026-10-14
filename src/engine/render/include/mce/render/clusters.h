#pragma once
// Forward+ clustered light assignment on the CPU (rhi-renderer.md §3.6, FR-R3D-030).
//
// The view frustum is cut into a grid of clusters: `x × y` screen tiles times `z` depth slices
// spaced exponentially between the camera's near and far planes. Every point light whose sphere
// touches a cluster is listed in it. The fragment shader finds its cluster from gl_FragCoord and
// view depth and loops over that cluster's lights only, so light count is not a special case.
//
//   per light: frustum test -> clipped-sphere screen bounds (tangent planes, Mara & McGuire 2013)
//              -> tile rectangle × slice range -> exact sphere-vs-cluster-AABB test per cluster
//   then:      count per cluster -> prefix sum -> scatter (light order kept inside a cluster)
//
// The result is three std430 arrays for banks 4–6: per-cluster {offset, count}, the light pool,
// and the flat light-index list. Deterministic: a pure function of (view, lights, grid).
//
// build() = prepare() + assign(). prepare() is the O(lights + tiles) part every path needs: the
// light pool, the slice constants, the per-slice tile extents and the culled view-space light list.
// assign() is the per-cluster work; Forward3D runs its GPU twin (cluster_build.comp, M-VISUAL 1)
// over prepare()'s outputs instead when the device has compute, and Scene3D only calls assign()
// when the CPU lists are actually read.

#include "mce/containers/array.h"
#include "mce/math/math.h"

namespace mce::render {

struct View3D;

// A point light, or a spot light when `spot_outer` > 0 (clustered by its bounding sphere either way).
struct PointLight {
    math::vec3 position;
    f32 radius = 5.0f;              // influence range in meters: the light is exactly 0 beyond it
    math::vec3 color{1.0f, 1.0f, 1.0f};  // linear
    f32 intensity = 1.0f;           // radiance scale; see ADR-0024 for the unit convention
    math::vec3 direction{0.0f, -1.0f, 0.0f};  // spot only: the way the cone points
    f32 spot_inner = 0.0f;          // spot only: half-angle (radians) of full intensity
    f32 spot_outer = 0.0f;          // spot half-angle (radians) where it reaches 0; 0 = point light
    // M-VISUAL 4: asks for a shadow map (one atlas tile for a spot, six cube faces for a point).
    // Granted per frame by screen importance within LocalShadowSettings' budget.
    bool cast_shadows = false;
};

struct ClusterGridDesc {
    u32 x = 32;  // screen tiles across
    u32 y = 16;  // screen tiles down
    u32 z = 24;  // exponential depth slices (at most 256)
    u32 max_indices = 192 * 1024;  // light-index list capacity (overflowing pairs are dropped + counted)
};

// std430 rows (bank 5 LightPool).
struct GpuPointLight {
    math::vec4 position_radius;  // xyz world position, w radius
    math::vec4 color;            // rgb = color * intensity, w = 1 / radius²
    math::vec4 spot{0.0f, 0.0f, 0.0f, 1.0f};  // cone = saturate(dot(-L, xyz) + w): xyz = dir * scale, w = offset
    math::vec4 shadow{-1.0f, 0.0f, 0.0f, 0.0f};  // x: first local shadow record (-1 = unshadowed), y: records (1 spot, 6 point)
};
static_assert(sizeof(GpuPointLight) == 64);

// std430 rows of the culled light list prepare() makes: lights that pass the frustum test and
// have intensity > 0, in submission order. The GPU assignment reads these.
struct ClusterCullLight {
    math::vec4 center_radius;  // view-space centre (camera looks down -Z), w radius
    u32 light;                 // index into lights()
    u32 pad[3] = {};
};
static_assert(sizeof(ClusterCullLight) == 32);

// std430 rows (bank 4 Clusters).
struct ClusterRange {
    u32 offset;
    u32 count;
};

class ClusterLights {
public:
    struct Stats {
        u32 lights = 0;           // submitted
        u32 visible = 0;          // touching at least one cluster
        u32 indices = 0;          // light-index list length
        u32 max_per_cluster = 0;
        u32 dropped = 0;          // (light, cluster) pairs over max_indices
    };

    explicit ClusterLights(mem::IAllocator& alloc = mem::heap());

    void build(const View3D& view, Span<const PointLight> lights, const ClusterGridDesc& grid = {});
    // Light pool, grid constants, tile bounds and the culled list; ranges() and indices() stay
    // empty until assign().
    void prepare(const View3D& view, Span<const PointLight> lights, const ClusterGridDesc& grid = {});
    // The CPU light assignment over prepare()'s outputs (no-op when already assigned).
    void assign();
    bool assigned() const { return assigned_; }
    // Points light `light` at its local shadow records (Scene3D, after prepare()).
    void set_shadow(u32 light, u32 first_record, u32 records) {
        gpu_lights_[light].shadow = math::vec4(f32(first_record), f32(records), 0.0f, 0.0f);
    }
    void clear();

    const ClusterGridDesc& grid() const { return grid_; }
    u32 cluster_count() const { return grid_.x * grid_.y * grid_.z; }
    // Cluster of (tile x, tile y, slice): x fastest, then y, then z — the shader's indexing.
    u32 cluster_index(u32 tx, u32 ty, u32 slice) const { return tx + grid_.x * (ty + grid_.y * slice); }
    // Depth slice of a positive view depth (meters in front of the camera), clamped to the grid.
    u32 slice_of(f32 view_depth) const;
    // slice = floor(log(depth) * scale + bias)
    f32 slice_scale() const { return slice_scale_; }
    f32 slice_bias() const { return slice_bias_; }

    Span<const GpuPointLight> lights() const { return gpu_lights_.span(); }
    Span<const ClusterCullLight> culled() const { return culled_.span(); }
    // View-space cluster bounds, as floats: x extents [z][x][lo, hi], then y extents [z][y][lo, hi],
    // then the z + 1 slice edge depths. tile_bounds_y_offset()/edges_offset() index the parts.
    Span<const f32> tile_bounds() const { return bounds_.span(); }
    u32 tile_bounds_y_offset() const { return grid_.z * grid_.x * 2; }
    u32 edges_offset() const { return grid_.z * (grid_.x + grid_.y) * 2; }
    Span<const ClusterRange> ranges() const { return ranges_.span(); }
    Span<const u32> indices() const { return indices_.span(); }
    // The lights touching cluster `c`, as indices into lights().
    Span<const u32> lights_in(u32 c) const;
    const Stats& stats() const { return stats_; }

private:
    ClusterGridDesc grid_;
    f32 slice_scale_ = 0.0f, slice_bias_ = 0.0f;
    Array<GpuPointLight> gpu_lights_;
    Array<ClusterRange> ranges_;
    Array<u32> indices_;
    Array<u32> pairs_;  // scratch: (cluster, light) pairs as cluster, light, cluster, light, ...
    Array<f32> bounds_;  // tile_bounds()
    Array<ClusterCullLight> culled_;
    f32 near_ = 0.0f, far_ = 0.0f, proj_x_ = 1.0f, proj_y_ = 1.0f;
    bool assigned_ = false;
    Stats stats_;
};

// Screen-space bounds of a view-space sphere along one axis, clipped by the near plane:
// `a` is the sphere centre's coordinate on that axis (x or y), `depth` its distance in front of
// the camera (-z), `proj_scale` the projection's diagonal term for the axis (P[0][0] or P[1][1]).
// Returns false when the sphere is entirely behind the near plane. NDC results are unclamped.
bool sphere_ndc_bounds(f32 a, f32 depth, f32 radius, f32 near_z, f32 proj_scale, f32& lo, f32& hi);

}  // namespace mce::render
