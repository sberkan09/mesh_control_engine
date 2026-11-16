#pragma once
// Decal scatter as data (M-DECALSCATTER step 35a). Any number of DecalScatter3D entities; install_kit3d registers the
// component next to Decal3D. There is no system and nothing is stored between ticks.
//
//   DecalScatter3D  Authoring (all Save, clamped to the Range when read and never rewritten): `enabled` (1); `texture`
//                   (an atlas: rgb colour, a coverage; empty = plain colour) split into `atlas_cols` x `atlas_rows` cells
//                   (1..8 each, default 1); `count` (16, 0..256) decals; `area` (4 4, x and z half extents in metres, the
//                   decals project down the entity's local -Y like Decal3D); `depth_m` (1, 0.05..20) projection depth;
//                   `size_min` (0.4) and `size_max` (1.2), 0.05..20 metres (an inverted or NaN pair falls back to the
//                   defaults); `seed` (1); `color` (1 1 1 1, linear rgb, a opacity); `tint_jitter` (0.2, 0..1) how far each
//                   decal's brightness may drop below `color`; `roughness` (0.7), `metallic` (0), `material_weight` (1);
//                   `angle_fade` (0.2) and `edge_fade` (0.2), all 0..1; `fade_distance_m` (60, 0..2000) the camera distance
//                   at which the decals have faded out (0 = no fade); `strength` (1, 0..1) scales the opacity, 0 = off.
//
// decal_scatter_item(scatter, i) gives decal `i` from an integer hash of (seed, i) and the component only: decal i never
// depends on `count`, on another entity or on any state, so the same scene gives the same stains on every machine. Fixed
// evaluation order, no rand, no floating point state. decal_scatter_fade(distance, fade_distance_m) is 1 near, 0 at the
// limit (a smoothstep on the second half of the range, so near decals stay fully opaque), and 1 for fade_distance_m <= 0
// or NaN. NaN and out-of-range authoring fall back to the defaults. Nothing reads the component yet (the extract feed is
// 35b).

#include <algorithm>
#include <cmath>

#include "mce/asset/handle.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct DecalScatter3D {
    asset::AssetRef<asset::Texture> texture;
    math::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    math::vec2 area{4.0f, 4.0f};
    f32 depth_m = 1.0f;
    f32 size_min = 0.4f;
    f32 size_max = 1.2f;
    f32 tint_jitter = 0.2f;
    f32 roughness = 0.7f;
    f32 metallic = 0.0f;
    f32 material_weight = 1.0f;
    f32 angle_fade = 0.2f;
    f32 edge_fade = 0.2f;
    f32 fade_distance_m = 60.0f;
    f32 strength = 1.0f;
    u32 seed = 1;
    u32 count = 16;
    u32 atlas_cols = 1;
    u32 atlas_rows = 1;
    u8 enabled = 1;
    u8 pad_[3] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(DecalScatter3D, (enabled, Save, Range{0, 1}, Help{"0: the scatter is off"}),
              (texture, Save, Help{"atlas: rgb colour, a coverage; empty = plain colour"}),
              (atlas_cols, Save, Range{1, 8}, Help{"atlas cells across"}),
              (atlas_rows, Save, Range{1, 8}, Help{"atlas cells down"}),
              (count, Save, Range{0, 256}, Help{"decals spread over the area"}),
              (area, Save, Help{"x and z half extents in metres"}),
              (depth_m, Save, Range{0.05f, 20}, Help{"projection depth, metres"}),
              (size_min, Save, Range{0.05f, 20}, Help{"smallest decal size, metres"}),
              (size_max, Save, Range{0.05f, 20}, Help{"largest decal size, metres"}),
              (seed, Save, Help{"hash seed of the placement"}),
              (color, Save, Help{"linear rgb, a opacity"}),
              (tint_jitter, Save, Range{0, 1}, Help{"how far each decal's brightness may drop below color"}),
              (roughness, Save, Range{0, 1}), (metallic, Save, Range{0, 1}),
              (material_weight, Save, Range{0, 1}, Help{"how far roughness and metallic follow the decal"}),
              (angle_fade, Save, Range{0, 1}), (edge_fade, Save, Range{0, 1}),
              (fade_distance_m, Save, Range{0, 2000}, Help{"camera distance at which the decals have faded out; 0 = no fade"}),
              (strength, Save, Range{0, 1}, Help{"scales the opacity; 0 is off exactly"}))

// One scattered decal in the entity's local space: the centre offset (x, z; the decal sits at y 0), the yaw about local Y in
// radians (0..2pi), the square size in metres, the atlas cell and a per-channel brightness multiplier (1 - tint_jitter * u).
struct ScatterDecal {
    f32 x = 0.0f;
    f32 z = 0.0f;
    f32 yaw = 0.0f;
    f32 size = 1.0f;
    u32 cell_x = 0;
    u32 cell_y = 0;
    math::vec3 tint{1.0f, 1.0f, 1.0f};
};

namespace detail {

inline f32 decal_scatter_clamp(f32 v, f32 def, f32 lo, f32 hi) { return std::isnan(v) ? def : std::clamp(v, lo, hi); }

// A 32-bit finaliser (lowbias32 style): the integer hash of the (seed, i, k) triple. k is the draw index of the decal.
inline u32 decal_scatter_hash(u32 seed, u32 i, u32 k) {
    u32 h = seed * 0x9E3779B1u + 0x7F4A7C15u;
    h ^= i * 0x85EBCA6Bu + 0xC2B2AE35u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= k * 0x27D4EB2Fu + 0x165667B1u;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

// 0..1 (exclusive of 1) from the top 24 bits.
inline f32 decal_scatter_unit(u32 h) { return static_cast<f32>(h >> 8) * (1.0f / 16777216.0f); }

inline u32 decal_scatter_cells(u32 n) { return std::clamp<u32>(n, 1u, 8u); }

}  // namespace detail

// Decal `i` of the scatter, from (seed, i) and the authoring only. The offset lies in the area (a NaN or negative half extent
// falls back to 4), the size in [size_min, size_max] (an inverted or NaN pair falls back to 0.4..1.2), the cell inside the
// atlas. Draw order is fixed: x, z, yaw, size, cell_x, cell_y, tint r, g, b.
inline ScatterDecal decal_scatter_item(const DecalScatter3D& s, u32 i) {
    const f32 ax = std::isnan(s.area.x) || s.area.x < 0.0f ? 4.0f : std::min(s.area.x, 10000.0f);
    const f32 az = std::isnan(s.area.y) || s.area.y < 0.0f ? 4.0f : std::min(s.area.y, 10000.0f);
    f32 lo = detail::decal_scatter_clamp(s.size_min, 0.4f, 0.05f, 20.0f);
    f32 hi = detail::decal_scatter_clamp(s.size_max, 1.2f, 0.05f, 20.0f);
    if (lo > hi) {
        lo = 0.4f;
        hi = 1.2f;
    }
    const f32 jitter = detail::decal_scatter_clamp(s.tint_jitter, 0.2f, 0.0f, 1.0f);
    const u32 cols = detail::decal_scatter_cells(s.atlas_cols);
    const u32 rows = detail::decal_scatter_cells(s.atlas_rows);
    const u32 seed = s.seed;

    const f32 ux = detail::decal_scatter_unit(detail::decal_scatter_hash(seed, i, 0));
    const f32 uz = detail::decal_scatter_unit(detail::decal_scatter_hash(seed, i, 1));
    const f32 uy = detail::decal_scatter_unit(detail::decal_scatter_hash(seed, i, 2));
    const f32 us = detail::decal_scatter_unit(detail::decal_scatter_hash(seed, i, 3));
    const u32 hc = detail::decal_scatter_hash(seed, i, 4);
    const u32 hr = detail::decal_scatter_hash(seed, i, 5);
    const f32 tr = detail::decal_scatter_unit(detail::decal_scatter_hash(seed, i, 6));
    const f32 tg = detail::decal_scatter_unit(detail::decal_scatter_hash(seed, i, 7));
    const f32 tb = detail::decal_scatter_unit(detail::decal_scatter_hash(seed, i, 8));

    ScatterDecal d;
    d.x = (ux * 2.0f - 1.0f) * ax;
    d.z = (uz * 2.0f - 1.0f) * az;
    d.yaw = uy * 6.2831853f;
    d.size = std::clamp(lo + (hi - lo) * us, lo, hi);
    d.cell_x = static_cast<u32>((static_cast<u64>(hc) * cols) >> 32);
    d.cell_y = static_cast<u32>((static_cast<u64>(hr) * rows) >> 32);
    d.tint = math::vec3{1.0f - jitter * tr, 1.0f - jitter * tg, 1.0f - jitter * tb};
    return d;
}

// The distance fade 0..1: 1 up to half of `fade_distance_m`, a smoothstep down to exactly 0 at the limit and beyond. A
// fade distance of 0 or less (or NaN) means no fade: 1. A NaN or negative distance counts as near (1).
inline f32 decal_scatter_fade(f32 distance, f32 fade_distance_m) {
    if (!(fade_distance_m > 0.0f)) return 1.0f;
    if (!(distance > 0.0f)) return 1.0f;
    const f32 lo = fade_distance_m * 0.5f;
    if (!(distance > lo)) return 1.0f;
    if (!(distance < fade_distance_m)) return 0.0f;
    const f32 t = (distance - lo) / (fade_distance_m - lo);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

}  // namespace mce::kit
