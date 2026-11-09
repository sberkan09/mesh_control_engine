#pragma once
// M-GI2 (docs/milestones/M-GI2.md): the irradiance volume's per-probe record, as pure CPU helpers, and the renderer's
// state for it (GiProbeState, which Forward3D keeps behind a forward-declared pointer so forward3d.h stays as it is).
//
// A volume that asks for visibility, relocation or following (render::gi_texels_per_probe() == 10) stores 10 texels
// per probe instead of 4: texels 0..3 are the L1 irradiance as before, texels 4..9 the record (layout below). The
// record is captured by a geometry-only "vis" pass per face (kGiVisFS: the signed radial distance, negative on back
// faces) and projected by kGiProjectFS into six cosine-power lobes around +-x, +-y, +-z. Distances are normalised to
// 0..1 by gi_dmax() so the RGBM bake carries them. The shaders repeat these formulas as text (shaders3d.h); the tests
// pin the two together.
//
// f32 / f64 only, fixed evaluation order, no global state. Included by gi_probes.cpp, forward3d.cpp and the tests
// (and, from M-GI2 50h, kit3d_extract.cpp): keep it narrow (no other render header).

#include "mce/containers/array.h"
#include "mce/math/math.h"
#include "mce/rhi/types.h"
#include "mce/types.h"

namespace mce::render {

// ---- the volume texture's layout
inline constexpr u32 kGiTexelsL1 = 4;       // texels per probe of a volume without the record
inline constexpr u32 kGiTexelsRecord = 10;  // ... with it
// The record's distances are axial (M-GI2 50e): a hit at distance r in direction d counts as r * dot(d, axis) in the
// lobe around `axis` (a plane across the axis reads its own distance, with no spread).
inline constexpr u32 kGiTexelMeanPos = 4;     // rgb: the mean axial hit distance in the lobes around +x, +y, +z
inline constexpr u32 kGiTexelMeanNeg = 5;     // rgb: ... around -x, -y, -z
inline constexpr u32 kGiTexelSquarePos = 6;   // rgb: the mean squared axial distance in the lobes around +x, +y, +z
inline constexpr u32 kGiTexelSquareNeg = 7;   // rgb: ... around -x, -y, -z
inline constexpr u32 kGiTexelOffset = 8;      // rgb: the relocation offset, 0.5 + 0.5 * offset / (max_offset * cell)
// r: the back-face share; 50g writes 1 for a probe that scrolled in and is not captured yet. A cleared volume's probes
// (the fill) read 0 here and an all-zero record, which the lookup also weighs 0 (gi_visibility_weight).
inline constexpr u32 kGiTexelState = 9;
// Bits of PassU::gi_counts.w: the 10-texel layout, the visibility weight (M-GI2 50e), the relocation offsets (50f).
inline constexpr u32 kGiFlagRecord = 1;
inline constexpr u32 kGiFlagVisibility = 2;
inline constexpr u32 kGiFlagRelocate = 4;

// ---- the record's constants (tuned in 50d / 50e against the tests and the city audit)
// The vis pass draws a rigid draw only when its bounding radius is at least this times the probe spacing (the smallest
// of the volume's three cell sizes): walls, floors, buildings and terrain occlude; props, cars and characters do not.
inline constexpr f32 kGiVisMinRadius = 0.5f;
// Distances are clamped to dmax = kGiVisRangeCells * the cell's diagonal and stored as d / dmax.
inline constexpr f32 kGiVisRangeCells = 1.5f;
// The lobes' cosine power (the shader writes it as two squarings: change both together).
inline constexpr f32 kGiVisLobePower = 4.0f;
// What the vis faces are cleared to (nothing hit): beyond any dmax, so it projects as 1.
inline constexpr f32 kGiVisFar = 1.0e4f;
// A probe whose back-face share is at or above this is classified off (DDGI's threshold; read by 50e's lookup).
inline constexpr f32 kGiBackfaceOff = 0.25f;
// The smallest variance (m^2) gi_chebyshev() uses: a flat record (variance 0) stays finite.
inline constexpr f32 kGiMinVariance = 1.0e-3f;
// Weights below this are crushed as DDGI does (w^3 / kGiCrush^2; 50e).
inline constexpr f32 kGiCrush = 0.2f;
// "No probe left" (50e): when no probe with a positive trilinear weight has a visibility factor of at least this, the
// lookup gives a = 0 (the sky's ambient). A factor of 1e-4 after the crush is a Chebyshev bound of about 0.016 before it
// (the point some 1.7 standard deviations behind the lobe's mean), far below a probe that sees the point (1) or half
// sees it, and far above an occluded one behind a wall (1e-20 and less). It is compared with the factors, not with the
// summed weights, so the decision does not depend on the cell size or on how small the trilinear weights are.
inline constexpr f32 kGiNoProbe = 1.0e-4f;
// How far past a solid's face relocation puts a probe (m; 50f).
inline constexpr f32 kGiRelocateMargin = 0.1f;
// GiVolume::max_offset's bound (fraction of a cell): a relocated probe never leaves its cell.
inline constexpr f32 kGiMaxOffsetLimit = 0.45f;

// One probe's record, normalised by dmax (texels 4..7).
struct GiRecord {
    math::vec3 mean_pos{1.0f, 1.0f, 1.0f};    // lobes around +x, +y, +z
    math::vec3 mean_neg{1.0f, 1.0f, 1.0f};    // lobes around -x, -y, -z
    math::vec3 square_pos{1.0f, 1.0f, 1.0f};
    math::vec3 square_neg{1.0f, 1.0f, 1.0f};
};

struct GiMoments {
    f32 mean = 0.0f;
    f32 square = 0.0f;
};

// The record toward direction `d` (normalised here; a zero d reads the +x lobe): the ambient-cube interpolation
// sum_i d_i^2 * lobe[sign(d_i), i], for the mean and for the square. Along an axis it returns that lobe's own value;
// between axes it blends by d_i^2 (which sums to 1). (The plan's lookup formula; 50e's lookup reads the lobes per axis
// instead, gi_visibility_weight.)
GiMoments gi_record_at(const GiRecord& r, math::vec3 d);

// Chebyshev's one-sided bound, cubed as DDGI does: 1 when r <= mean (the point is in front of the nearest surface),
// else (var / (var + (r - mean)^2))^3 with var = max(square - mean^2, kGiMinVariance). Falls monotonically past the
// mean and is finite for a zero variance. Distances in metres (mean = record * dmax, square = record * dmax^2).
f32 gi_chebyshev(f32 r, f32 mean, f32 square);

// The lookup's visibility factor of one probe (M-GI2 50e; the CPU twin of gi_vis in shaders3d.h, which
// gi_irradiance2 multiplies into the probe's trilinear and backface weight): 0 when the probe is classified off
// (back_share >= kGiBackfaceOff) or when the record is all zero (a cleared volume's probe not captured yet; a captured
// probe's means are positive); otherwise, per axis i, gi_chebyshev of the point's distance along the axis |v_i|
// against the lobe on v's side (v_i >= 0: the + lobe; mean = record * dmax, square = record * dmax^2), the smallest of
// the three, crushed below kGiCrush to vis^3 / kGiCrush^2. `v` = the normal-biased lookup point minus the probe's
// position. Only the visibility factor is crushed (DDGI crushes its backface-times-Chebyshev weight; the engine's
// backface term already falls to 0.05, and crushing it too would move every open-air probe's weight).
// Why per axis and not the plan's radial test against gi_record_at(v): the six lobes cannot tell a wall in front of
// the probe from the open space beside it, so the d_i^2 blend toward a point offset sideways behind a thin wall mixed in
// the side lobe's long distance and let the light through (the thin-wall test measured no reduction). With the record
// storing axial distances, a wall across an axis reads its own distance and a point behind it is behind it along that
// axis wherever it lies sideways.
// Limits: the test is sound for walls across an axis. A thin wall at 20 to 45 degrees to the axes still leaks (its
// distance is smeared over two lobes; the plan's radial blend leaks there too); an opening (a doorway, a window) is read
// as the infinite plane of its wall, so points seen through it from the nearest probes are wrongly occluded; and points
// in the fade band outside the volume's box get no occlusion beyond what the clamped lookup point implies.
f32 gi_visibility_weight(const GiRecord& r, f32 back_share, math::vec3 v, f32 dmax);

// The lookup's blend of a point's 8 probes with the visibility weight on (the CPU twin of gi_irradiance2's
// normalisation): base[k] = the probe's trilinear times backface weight (>= 0), vis[k] its gi_visibility_weight. Returns
// false ("no probe left": a = 0, the sky's ambient) when no probe with base[k] > 0 has vis[k] >= kGiNoProbe; otherwise
// writes base[k] * vis[k] / their sum into `out` (no floor under the sum) and returns true.
bool gi_blend(const f32 (&base)[8], const f32 (&vis)[8], f32 (&out)[8]);

// The cosine-power lobe weight of direction `d` (unit) around `axis` (unit): max(dot(d, axis), 0)^kGiVisLobePower.
f32 gi_lobe_weight(math::vec3 d, math::vec3 axis);

// The volume's cell: the box's size over (counts - 1) per axis (a count below 2 counts as 2).
math::vec3 gi_cell(const math::aabb3& bounds, const u32 (&counts)[3]);
// The record's range: kGiVisRangeCells times the cell's diagonal (at least 1e-3 m).
f32 gi_dmax(math::vec3 cell);
// The vis pass's draw rule's spacing: the smallest of the cell's three sizes.
f32 gi_spacing(math::vec3 cell);

// A solid the relocation pushes probes out of: a large rigid draw's world matrix, its rest-pose model-space box, and
// its bounding sphere (world).
struct GiSolid {
    math::mat4 model = math::mat4::identity();
    math::aabb3 local{};
    math::vec3 center{};
    f32 radius = 0.0f;
};

// The relocation offset of the probe at grid position `g` (M-GI2 Design 3): 0 when no solid contains it. Otherwise the
// shortest push through the nearest face of each containing solid's oriented box to kGiRelocateMargin past it, summed
// and re-tested (at most 3 rounds, solids in order); accepted only when the result lies outside every solid and each
// component of the offset is within max_offset * cell of g (max_offset clamped to 0..kGiMaxOffsetLimit, NaN 0),
// else 0. Solids that do not contain the probe do not change the result's arithmetic.
math::vec3 gi_relocate(math::vec3 g, math::vec3 cell, f32 max_offset, Span<const GiSolid> solids);

// The following volume's snapped origin (M-GI2 Design 4): the world cell index (x, z) of its first probe column, the
// camera's cell minus half the count, in f64 world coordinates (View3D::position plus the floating origin), so a
// rebase moves nothing. It steps by exactly one when the camera crosses a cell edge.
struct GiFollowCell {
    i64 x = 0;
    i64 z = 0;
    bool operator==(const GiFollowCell&) const = default;
};
GiFollowCell gi_follow_origin(f64 camera_x, f64 camera_z, f64 cell_x, f64 cell_z, u32 count_x, u32 count_z);

// The texture slot of world cell index `i` on an axis of `count` probes: i mod count, in 0..count-1 for any i.
u32 gi_slot(i64 i, u32 count);
// The toroidal slot offset packed into one float lane (PassU::gi_min.w): ox + 32 oy + 1024 oz, each in 0..31 (exact in
// f32), and back.
f32 gi_slot_offset_pack(u32 ox, u32 oy, u32 oz);
void gi_slot_offset_unpack(f32 packed, u32 (&o)[3]);

// What Forward3D keeps for the record (created with the GI pipelines, destroyed in shutdown; forward3d.h only
// forward-declares it).
struct GiProbeState {
    rhi::PipelineHandle vis_pipes[2];  // kGiVisFS over kMeshVS / kSkinnedMeshVS: depth-tested, both faces
    rhi::TextureHandle vis_faces;      // RGBA16F kGiFaceSize^2 x (6 * kGiMaxBatchProbes) layers: the signed distances
    u32 vis_layers = 0;
    bool fill = false;                 // the current gi_capture call fills (or refills) rather than rolls
    Array<math::vec3> offsets;         // per probe: the relocation offset (50f)
    GiFollowCell follow{};             // the following volume's snapped origin (50g)
    bool follow_valid = false;
    Array<u32> scroll_queue;           // probes that scrolled in and wait for their capture (50g)
};

}  // namespace mce::render
