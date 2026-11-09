// M-GI2 50c: the irradiance volume's record helpers (mce/render/gi_probes.h), exact CPU checks.
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

#include "mce/render/gi_probes.h"

using namespace mce;
using namespace mce::render;
using math::vec3;

namespace {

GiRecord distinct_record() {
    GiRecord r;
    r.mean_pos = {0.1f, 0.2f, 0.3f};
    r.mean_neg = {0.4f, 0.5f, 0.6f};
    r.square_pos = {0.01f, 0.04f, 0.09f};
    r.square_neg = {0.16f, 0.25f, 0.36f};
    return r;
}

// A box of `half` around `c`, axis-aligned, as a solid.
GiSolid box_solid(vec3 c, vec3 half) {
    GiSolid s;
    s.model = math::translation(c);
    s.local = {-half, half};
    s.center = c;
    s.radius = math::length(half);
    return s;
}

u32 bits(f32 f) {
    u32 u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

bool same_bits(vec3 a, vec3 b) { return bits(a.x) == bits(b.x) && bits(a.y) == bits(b.y) && bits(a.z) == bits(b.z); }

}  // namespace

TEST_CASE("gi_record_at: a lobe's own value along its axis, the d_i^2 blend between axes") {
    const GiRecord r = distinct_record();
    const vec3 axes[6] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {-1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    const f32 means[6] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
    const f32 squares[6] = {0.01f, 0.04f, 0.09f, 0.16f, 0.25f, 0.36f};
    for (u32 i = 0; i < 6; ++i) {
        const GiMoments m = gi_record_at(r, axes[i] * 3.0f);  // normalised inside
        CHECK(m.mean == means[i]);
        CHECK(m.square == squares[i]);
    }
    // (1, -1, 0) / sqrt 2: half of +x, half of -y.
    const GiMoments m = gi_record_at(r, {1.0f, -1.0f, 0.0f});
    CHECK(m.mean == doctest::Approx(0.5f * 0.1f + 0.5f * 0.5f).epsilon(1e-6));
    CHECK(m.square == doctest::Approx(0.5f * 0.01f + 0.5f * 0.25f).epsilon(1e-6));
    // (-1, 2, 2) / 3: 1/9 of -x, 4/9 of +y, 4/9 of +z.
    const GiMoments k = gi_record_at(r, {-1.0f, 2.0f, 2.0f});
    CHECK(k.mean == doctest::Approx((0.4f + 4.0f * 0.2f + 4.0f * 0.3f) / 9.0f).epsilon(1e-6));
    // A uniform record reads the same everywhere; a zero direction reads +x.
    GiRecord u;
    u.mean_pos = u.mean_neg = {0.7f, 0.7f, 0.7f};
    CHECK(gi_record_at(u, {0.3f, -0.8f, 0.5f}).mean == doctest::Approx(0.7f).epsilon(1e-6));
    CHECK(gi_record_at(r, {0.0f, 0.0f, 0.0f}).mean == 0.1f);
}

TEST_CASE("gi_chebyshev: 1 in front of the mean, monotonic and cubed past it, finite for a zero variance") {
    CHECK(gi_chebyshev(1.0f, 2.0f, 4.5f) == 1.0f);
    CHECK(gi_chebyshev(2.0f, 2.0f, 4.5f) == 1.0f);
    // var = 4.5 - 4 = 0.5; r - mean = 1: (0.5 / 1.5)^3.
    CHECK(gi_chebyshev(3.0f, 2.0f, 4.5f) == doctest::Approx(1.0f / 27.0f).epsilon(1e-6));
    f32 last = 1.0f;
    for (f32 r = 2.05f; r < 6.0f; r += 0.05f) {
        const f32 v = gi_chebyshev(r, 2.0f, 4.5f);
        CHECK(v < last);
        CHECK(v > 0.0f);
        last = v;
    }
    // Zero (or negative) variance: kGiMinVariance keeps it finite and small past the mean.
    const f32 z = gi_chebyshev(2.5f, 2.0f, 4.0f), neg = gi_chebyshev(2.5f, 2.0f, 3.0f);
    CHECK(std::isfinite(z));
    CHECK(z == neg);
    const f32 t = kGiMinVariance / (kGiMinVariance + 0.25f);
    CHECK(z == doctest::Approx(t * t * t).epsilon(1e-5));
    CHECK(z < 1e-6f);
}

TEST_CASE("gi_visibility_weight: Chebyshev per axis, the smallest, crushed below kGiCrush, 0 at the back-face threshold") {
    GiRecord r;  // every lobe: mean 0.5, square 0.25 + 0.01 (of dmax = 2 m: mean 1 m, variance 0.04 m^2)
    r.mean_pos = r.mean_neg = {0.5f, 0.5f, 0.5f};
    r.square_pos = r.square_neg = {0.26f, 0.26f, 0.26f};
    const f32 dmax = 2.0f;
    // In front of the nearest surface: 1, in any direction; r = 0 (the point on the probe): 1.
    CHECK(gi_visibility_weight(r, 0.0f, {0.9f, 0, 0}, dmax) == 1.0f);
    CHECK(gi_visibility_weight(r, 0.0f, {0, -0.5f, 0.5f}, dmax) == 1.0f);
    CHECK(gi_visibility_weight(r, 0.0f, {0, 0, 0}, dmax) == 1.0f);
    // Just past the mean: Chebyshev above the crush, unchanged.
    const f32 near = gi_chebyshev(1.05f, 1.0f, 0.26f * 4.0f);
    REQUIRE(near > kGiCrush);
    CHECK(gi_visibility_weight(r, 0.0f, {0, 0, 1.05f}, dmax) == doctest::Approx(near).epsilon(1e-6));
    // Well past it: crushed to vis^3 / kGiCrush^2, smaller than the bound, never 0 before it underflows.
    const f32 far = gi_chebyshev(1.6f, 1.0f, 0.26f * 4.0f);
    REQUIRE(far < kGiCrush);
    const f32 crushed = gi_visibility_weight(r, 0.0f, {-1.6f, 0, 0}, dmax);
    CHECK(crushed == doctest::Approx(far * far * far / (kGiCrush * kGiCrush)).epsilon(1e-5));
    CHECK(crushed < far);
    CHECK(crushed > 0.0f);
    // Continuous at the crush: kGiCrush maps to itself.
    CHECK(kGiCrush * kGiCrush * kGiCrush / (kGiCrush * kGiCrush) == doctest::Approx(kGiCrush).epsilon(1e-6));
    // Classification: a share at or above kGiBackfaceOff weighs 0, even in front of the surface; just under, it counts.
    CHECK(gi_visibility_weight(r, kGiBackfaceOff, {0.1f, 0, 0}, dmax) == 0.0f);
    CHECK(gi_visibility_weight(r, 1.0f, {0.1f, 0, 0}, dmax) == 0.0f);
    CHECK(gi_visibility_weight(r, 0.249f, {0.1f, 0, 0}, dmax) == 1.0f);
    // Direction-dependent: a wall 0.2 m away along +z (mean 0.1 of dmax, no spread), open (1) elsewhere.
    GiRecord wall;
    wall.mean_pos.z = 0.1f;
    wall.square_pos.z = 0.01f;
    CHECK(gi_visibility_weight(wall, 0.0f, {0, 0, 1.0f}, dmax) < 1e-6f);  // behind the wall
    CHECK(gi_visibility_weight(wall, 0.0f, {0, 0, -1.0f}, dmax) == 1.0f);  // the open side
    CHECK(gi_visibility_weight(wall, 0.0f, {1.0f, 0, 0}, dmax) == 1.0f);
    // Behind the wall but well off to the side (the plan's d_i^2 blend of the side lobes let this one through): the
    // distance along +z decides.
    CHECK(gi_visibility_weight(wall, 0.0f, {1.5f, 0.3f, 1.0f}, dmax) < 1e-6f);
    CHECK(gi_visibility_weight(wall, 0.0f, {1.5f, 0.3f, 0.15f}, dmax) == 1.0f);  // in front of it, off to the side
    // A degenerate record (all zero: a probe on a surface) stays finite everywhere.
    const GiRecord zero{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    for (const vec3 v : {vec3{0, 0, 0}, vec3{1e-7f, 0, 0}, vec3{0.3f, -0.2f, 0.1f}, vec3{-5, 5, 5}}) {
        const f32 w = gi_visibility_weight(zero, 0.0f, v, dmax);
        CHECK(std::isfinite(w));
        CHECK(w >= 0.0f);
        CHECK(w <= 1.0f);
    }
}

TEST_CASE("gi_visibility_weight: an all-zero record (a probe not captured yet) weighs 0") {
    const GiRecord zero{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    CHECK(gi_visibility_weight(zero, 0.0f, {0, 0, 0}, 2.0f) == 0.0f);
    CHECK(gi_visibility_weight(zero, 0.0f, {0.5f, 0, 0}, 2.0f) == 0.0f);
    GiRecord one_lobe = zero;  // a captured probe has some positive mean
    one_lobe.mean_neg.y = 0.05f;
    one_lobe.square_neg.y = 0.0025f;
    CHECK(gi_visibility_weight(one_lobe, 0.0f, {0, -0.05f, 0}, 2.0f) == 1.0f);
}

TEST_CASE("gi_blend: no probe left by the factors, whatever the cell size; a small surviving weight is not dimmed") {
    // A corridor along z, 1.2 m wide (|x| < 0.6) between thin tall walls (|x| 0.6 .. 0.8), narrower than the probe
    // spacing: both probe planes stand outside its walls, at x = -cell/2 and +cell/2. Each probe's record reads the near
    // wall's face across x on the corridor's side (axial distance, no spread) and nothing elsewhere. A point on the
    // corridor's floor is behind a wall from every probe: no probe left for 2 m cells and for 16 m cells alike.
    for (const f32 cx : {2.0f, 16.0f}) {
        INFO("cell " << cx);
        const vec3 cell{cx, 2.0f, cx};
        const f32 dmax = gi_dmax(cell);
        const vec3 pb{0.2f, 0.25f, 0.3f};
        f32 base[8], vis[8], out[8];
        for (u32 k = 0; k < 8; ++k) {
            const vec3 o{f32(k & 1), f32((k >> 1) & 1), f32(k >> 2)};
            const vec3 q{(o.x - 0.5f) * cx, o.y * 2.0f, (o.z - 0.5f) * cx};
            GiRecord r;
            const f32 face = (cx * 0.5f - 0.8f) / dmax;  // the wall's outer face, toward the corridor
            if (o.x > 0.5f) {
                r.mean_neg.x = face;
                r.square_neg.x = face * face;
            } else {
                r.mean_pos.x = face;
                r.square_pos.x = face * face;
            }
            const vec3 g = (pb - vec3{-0.5f * cx, 0.0f, -0.5f * cx}) / cell;
            const vec3 tw{o.x > 0.5f ? g.x : 1.0f - g.x, o.y > 0.5f ? g.y : 1.0f - g.y, o.z > 0.5f ? g.z : 1.0f - g.z};
            base[k] = tw.x * tw.y * tw.z * 0.6f;  // times a backface term
            vis[k] = gi_visibility_weight(r, 0.0f, pb - q, dmax);
            CHECK(base[k] > 0.0f);
            CHECK(vis[k] < kGiNoProbe);
        }
        CHECK_FALSE(gi_blend(base, vis, out));
        for (const f32 w : out) CHECK(w == 0.0f);
    }
    // A surviving probe whose weight is far under 1e-6 (a tiny trilinear share) carries the point alone, undimmed.
    const f32 base[8] = {5e-7f, 0.3f, 0.2f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    const f32 vis[8] = {1.0f, 0, 0, 0, 0, 0, 0, 0};
    f32 out[8];
    REQUIRE(gi_blend(base, vis, out));
    CHECK(out[0] == 1.0f);
    for (u32 k = 1; k < 8; ++k) CHECK(out[k] == 0.0f);
    // A probe with no trilinear weight does not keep the point lit, however visible.
    const f32 base0[8] = {0.0f, 0.5f, 0.5f, 0, 0, 0, 0, 0};
    const f32 vis0[8] = {1.0f, 1e-6f, 0.0f, 0, 0, 0, 0, 0};
    CHECK_FALSE(gi_blend(base0, vis0, out));
    // All visible: the plain normalised weights.
    const f32 ones[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    REQUIRE(gi_blend(base, ones, out));
    f32 total = 0.0f;
    for (u32 k = 0; k < 8; ++k) total += out[k];
    CHECK(total == doctest::Approx(1.0f).epsilon(1e-6));
    CHECK(out[1] == doctest::Approx(0.3f / (1.0f + 5e-7f)).epsilon(1e-5));
}

TEST_CASE("gi_lobe_weight, gi_cell, gi_dmax, gi_spacing") {
    CHECK(gi_lobe_weight({1, 0, 0}, {1, 0, 0}) == 1.0f);
    CHECK(gi_lobe_weight({-1, 0, 0}, {1, 0, 0}) == 0.0f);
    CHECK(gi_lobe_weight({0, 1, 0}, {1, 0, 0}) == 0.0f);
    const f32 c = 0.70710678f;
    CHECK(gi_lobe_weight({c, c, 0}, {1, 0, 0}) == doctest::Approx(0.25f).epsilon(1e-5));  // cos^4 45
    const u32 counts[3] = {4, 3, 5};
    const vec3 cell = gi_cell({{-3, 0, -2}, {3, 2, 2}}, counts);
    CHECK(cell.x == 2.0f);
    CHECK(cell.y == 1.0f);
    CHECK(cell.z == 1.0f);
    CHECK(gi_spacing(cell) == 1.0f);
    CHECK(gi_dmax(cell) == doctest::Approx(kGiVisRangeCells * std::sqrt(6.0f)).epsilon(1e-6));
    CHECK(gi_dmax({0, 0, 0}) == 1e-3f);
}

TEST_CASE("gi_relocate: free probes stay, a shallow one leaves the face by the margin, a deep one stays") {
    const vec3 cell{2.0f, 2.0f, 2.0f};
    const GiSolid wall = box_solid({1.0f, 0.0f, 0.0f}, {1.0f, 5.0f, 5.0f});  // x 0..2
    const GiSolid solids[1] = {wall};
    // Free: no offset.
    CHECK(same_bits(gi_relocate({-1.0f, 0.0f, 0.0f}, cell, 0.4f, solids), {0, 0, 0}));
    // 0.3 m inside the +x face (x = 2): to 0.1 m outside it, an offset of 0.4 (max 0.4 * 2 = 0.8).
    const vec3 off = gi_relocate({1.7f, 0.0f, 0.0f}, cell, 0.4f, solids);
    CHECK(off.x == doctest::Approx(0.4f).epsilon(1e-5));
    CHECK(off.y == 0.0f);
    CHECK(off.z == 0.0f);
    // 1 m inside: the push (1.1 m) is beyond 0.8: refused.
    CHECK(same_bits(gi_relocate({1.0f, 0.0f, 0.0f}, cell, 0.4f, solids), {0, 0, 0}));
    // max_offset clamps to 0.45 (0.9 m) and NaN means 0.
    CHECK(same_bits(gi_relocate({1.15f, 0.0f, 0.0f}, cell, 0.45f, solids), {0, 0, 0}));  // push 0.95 > 0.9
    CHECK(gi_relocate({1.25f, 0.0f, 0.0f}, cell, 5.0f, solids).x == doctest::Approx(0.85f).epsilon(1e-5));
    CHECK(same_bits(gi_relocate({1.7f, 0.0f, 0.0f}, cell, std::nanf(""), solids), {0, 0, 0}));
    // A rotated, scaled solid: the push follows its axes in world metres.
    GiSolid turned;
    turned.model = math::translation({0, 0, 5}) * math::rotation(math::quat_from_axis_angle({0, 1, 0}, 1.5707963f)) * math::scaling({2.0f, 1.0f, 1.0f});
    turned.local = {{-1, -1, -1}, {1, 1, 1}};  // world: x +-1, z 5 +-2 (model x along world -z)
    turned.center = {0, 0, 5};
    turned.radius = 3.0f;
    const GiSolid one[1] = {turned};
    const vec3 t = gi_relocate({0.85f, 0.0f, 5.0f}, cell, 0.4f, one);  // 0.15 m inside world x = 1
    CHECK(t.x == doctest::Approx(0.25f).epsilon(1e-4));
    CHECK(std::fabs(t.z) < 1e-5f);
}

TEST_CASE("gi_relocate: solids that do not contain the probe do not matter, and two runs are bit-identical") {
    const vec3 cell{2.0f, 2.0f, 2.0f};
    const GiSolid wall = box_solid({1.0f, 0.0f, 0.0f}, {1.0f, 5.0f, 5.0f});
    const GiSolid far1 = box_solid({20.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
    const GiSolid far2 = box_solid({-9.0f, 3.0f, 7.0f}, {2.0f, 1.0f, 1.0f});
    const vec3 g{1.7f, 0.3f, -0.2f};
    const GiSolid a[1] = {wall};
    const GiSolid b[3] = {far1, wall, far2};
    const GiSolid c[3] = {far2, far1, wall};
    const vec3 ra = gi_relocate(g, cell, 0.4f, a);
    CHECK(same_bits(ra, gi_relocate(g, cell, 0.4f, b)));
    CHECK(same_bits(ra, gi_relocate(g, cell, 0.4f, c)));
    CHECK(same_bits(ra, gi_relocate(g, cell, 0.4f, a)));
    CHECK(ra.x > 0.0f);
    // A pushed position that lands inside another solid is refused.
    const GiSolid blocker = box_solid({2.325f, 0.0f, 0.0f}, {0.275f, 3.0f, 3.0f});  // x 2.05..2.6
    const GiSolid d[2] = {wall, blocker};
    CHECK(same_bits(gi_relocate({1.95f, 0.0f, 0.0f}, cell, 0.45f, d), {0, 0, 0}));
}

TEST_CASE("gi_follow_origin: rebase-invariant, steps by exactly one cell at a cell edge and not before") {
    const f64 cell = 6.0;
    const GiFollowCell a = gi_follow_origin(1234.5, -77.25, cell, cell, 32, 32);
    // The same world position split differently between the view and a 1000 m floating origin.
    const f64 lx = 234.5, lz = -1077.25, ox = 1000.0, oz = 1000.0;
    CHECK(gi_follow_origin(lx + ox, lz + oz, cell, cell, 32, 32) == a);
    CHECK(a.x == i64(std::floor(1234.5 / 6.0)) - 15);
    CHECK(a.z == i64(std::floor(-77.25 / 6.0)) - 15);
    // Walk across the edge at x = 1236 (206 cells): before it the same cell, past it one more.
    CHECK(gi_follow_origin(1235.999, -77.25, cell, cell, 32, 32).x == a.x);
    CHECK(gi_follow_origin(1236.0, -77.25, cell, cell, 32, 32).x == a.x + 1);
    CHECK(gi_follow_origin(1236.001, -77.25, cell, cell, 32, 32).x == a.x + 1);
    CHECK(gi_follow_origin(1229.999, -77.25, cell, cell, 32, 32).x == a.x - 1);
    for (f64 x = 1230.0; x < 1236.0; x += 0.25) CHECK(gi_follow_origin(x, -77.25, cell, cell, 32, 32).x == a.x);
    // Odd counts centre on the nearest probe (a tie at half a cell goes up).
    CHECK(gi_follow_origin(0.5, 0.5, 1.0, 1.0, 5, 5).x == -1);
    CHECK(gi_follow_origin(0.49, 0.5, 1.0, 1.0, 5, 5).x == -2);
}

TEST_CASE("gi_slot is a bijection for every count and offset; gi_slot_offset_pack round-trips") {
    for (u32 count = 2; count <= 32; ++count) {
        for (i64 origin = -70; origin <= 70; origin += 7) {
            u32 seen = 0;  // a bit per slot (count <= 32)
            for (u32 k = 0; k < count; ++k) {
                const u32 s = gi_slot(origin + i64(k), count);
                REQUIRE(s < count);
                CHECK((seen & (1u << s)) == 0);
                seen |= 1u << s;
            }
            CHECK(seen == (count == 32 ? ~0u : (1u << count) - 1u));
        }
    }
    CHECK(gi_slot(-1, 32) == 31);
    CHECK(gi_slot(-32, 32) == 0);
    CHECK(gi_slot(65, 32) == 1);
    for (u32 oz = 0; oz < 32; ++oz)
        for (u32 oy = 0; oy < 32; ++oy)
            for (u32 ox = 0; ox < 32; ++ox) {
                const f32 p = gi_slot_offset_pack(ox, oy, oz);
                u32 o[3];
                gi_slot_offset_unpack(p, o);
                REQUIRE(o[0] == ox);
                REQUIRE(o[1] == oy);
                REQUIRE(o[2] == oz);
            }
    CHECK(gi_slot_offset_pack(31, 31, 31) == 32767.0f);
}

// ---- M-GI2 50f / 50g

TEST_CASE("gi_offset_encode / gi_offset_decode: 0.5 is no offset, the limit maps to 0 and 1, and back") {
    const vec3 cell{2.0f, 1.0f, 4.0f};
    CHECK(same_bits(gi_offset_encode({0, 0, 0}, cell), {0.5f, 0.5f, 0.5f}));
    const vec3 lim{kGiMaxOffsetLimit * 2.0f, -kGiMaxOffsetLimit * 1.0f, 0.0f};
    const vec3 e = gi_offset_encode(lim, cell);
    CHECK(e.x == doctest::Approx(1.0f));
    CHECK(e.y == doctest::Approx(0.0f));
    CHECK(e.z == 0.5f);
    const vec3 off{0.4f, -0.3f, 1.1f};
    const vec3 d = gi_offset_decode(gi_offset_encode(off, cell), cell);
    CHECK(d.x == doctest::Approx(off.x).epsilon(1e-5));
    CHECK(d.y == doctest::Approx(off.y).epsilon(1e-5));
    CHECK(d.z == doctest::Approx(off.z).epsilon(1e-5));
    CHECK(same_bits(gi_offset_encode({1, 1, 1}, {0, 0, 0}), {0.5f, 0.5f, 0.5f}));  // a flat cell: no offset
}

TEST_CASE("gi_relocate: a relocated probe never moves more than max_offset of a cell nor into a solid") {
    // A field of solids (some overlapping, one rotated) and a lattice of probe positions through and around them: every
    // offset is 0 or keeps each component within max_offset * cell and lands outside every solid.
    const vec3 cell{2.0f, 1.5f, 2.5f};
    GiSolid solids[4] = {box_solid({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}), box_solid({1.6f, 0.2f, 0.0f}, {0.5f, 0.6f, 0.9f}),
                         box_solid({-3.0f, 0.0f, 2.0f}, {0.3f, 2.0f, 0.3f}), {}};
    solids[3].model = math::translation({2.0f, 0.0f, -3.0f}) * math::rotation(math::quat_from_axis_angle({0, 1, 0}, 0.6f));
    solids[3].local = {{-0.8f, -0.8f, -0.8f}, {0.8f, 0.8f, 0.8f}};
    solids[3].center = {2.0f, 0.0f, -3.0f};
    solids[3].radius = 0.8f * std::sqrt(3.0f);
    u32 moved = 0, refused = 0, can_move = 0;
    for (const f32 mo : {0.1f, 0.25f, 0.4f, 0.45f}) {
        for (f32 x = -4.0f; x <= 4.0f; x += 0.37f)
            for (f32 y = -1.5f; y <= 1.5f; y += 0.41f)
                for (f32 z = -4.0f; z <= 3.0f; z += 0.43f) {
                    const vec3 g{x, y, z};
                    const vec3 o = gi_relocate(g, cell, mo, solids);
                    CHECK(same_bits(o, gi_relocate(g, cell, mo, solids)));  // deterministic
                    // A probe inside exactly one of the three axis-aligned solids whose nearest-face push (to the margin past
                    // it) stays within max_offset and lands outside every solid CAN move: it must move by exactly that.
                    u32 holders = 0, holder = 0;
                    for (u32 k = 0; k < 4; ++k)
                        if (solids[k].local.contains(math::transform_point(math::inverse(solids[k].model), g))) {
                            ++holders;
                            holder = k;
                        }
                    if (holders == 1 && holder < 3) {
                        const GiSolid& s = solids[holder];
                        const vec3 lo = s.center + s.local.min, hi = s.center + s.local.max;
                        const f32 d[6] = {hi.x - g.x, g.x - lo.x, hi.y - g.y, g.y - lo.y, hi.z - g.z, g.z - lo.z};
                        u32 best = 0;  // gi_relocate's order: +x, -x, +y, -y, +z, -z; the first strictly nearest wins
                        for (u32 k = 1; k < 6; ++k)
                            if (d[k] < d[best]) best = k;
                        vec3 push{0, 0, 0};
                        const f32 len = (best & 1u ? -1.0f : 1.0f) * (d[best] + kGiRelocateMargin);
                        (best < 2 ? push.x : best < 4 ? push.y : push.z) = len;
                        bool free = std::fabs(push.x) <= mo * cell.x && std::fabs(push.y) <= mo * cell.y &&
                                    std::fabs(push.z) <= mo * cell.z;
                        for (const GiSolid& t : solids)
                            free = free && !t.local.contains(math::transform_point(math::inverse(t.model), g + push));
                        if (free) {
                            ++can_move;
                            CHECK(std::fabs(o.x - push.x) < 1e-5f);
                            CHECK(std::fabs(o.y - push.y) < 1e-5f);
                            CHECK(std::fabs(o.z - push.z) < 1e-5f);
                        }
                    }
                    if (o.x == 0.0f && o.y == 0.0f && o.z == 0.0f) {
                        bool inside = false;
                        for (const GiSolid& s : solids)
                            inside = inside || s.local.contains(math::transform_point(math::inverse(s.model), g));
                        refused += inside;
                        continue;
                    }
                    ++moved;
                    CHECK(std::fabs(o.x) <= mo * cell.x);
                    CHECK(std::fabs(o.y) <= mo * cell.y);
                    CHECK(std::fabs(o.z) <= mo * cell.z);
                    for (const GiSolid& s : solids)
                        CHECK_FALSE(s.local.contains(math::transform_point(math::inverse(s.model), g + o)));
                }
    }
    MESSAGE("lattice: " << moved << " probes moved (" << can_move << " checked against the expected push), " << refused
                        << " inside a solid and refused");
    CHECK(can_move > 0);
    CHECK(moved > 0);
    CHECK(refused > 0);
}

TEST_CASE("gi_follow_spacing and gi_follow_box: whole millimetres, the box follows the cell, a rebase moves it in render space only") {
    CHECK(gi_follow_spacing(-96.0f, 96.0f, 32) == doctest::Approx(6.194).epsilon(1e-12));  // 192 / 31 = 6.19354..
    CHECK(gi_follow_spacing(0.0f, 0.0f, 4) == 0.001);                                     // at least 1 mm
    // The same authored size placed anywhere (f32 extents differ by an ulp): the same spacing.
    CHECK(gi_follow_spacing(1000.3f - 96.0f, 1000.3f + 96.0f, 32) == gi_follow_spacing(-96.0f, 96.0f, 32));
    const u32 counts[3] = {8, 3, 6};
    const math::aabb3 authored{{-7.0f, 0.5f, -5.0f}, {7.0f, 4.5f, 5.0f}};  // 2 m cells on x and z
    const GiFollowCell f{-3, 11};
    const math::aabb3 b = gi_follow_box(f, authored, counts, 0.0, 0.0);
    CHECK(b.min.x == -6.0f);
    CHECK(b.max.x == 8.0f);
    CHECK(b.min.z == 22.0f);
    CHECK(b.max.z == 32.0f);
    CHECK(b.min.y == 0.5f);
    CHECK(b.max.y == 4.5f);
    // A floating origin of (1000, 0, -1000): the box in render space is the same world box.
    const math::aabb3 r = gi_follow_box(f, authored, counts, 1000.0, -1000.0);
    CHECK(r.min.x == -1006.0f);
    CHECK(r.min.z == 1022.0f);
    CHECK(r.max.x - r.min.x == 14.0f);
}

TEST_CASE("gi_scroll: the slots that leave are the slots that arrive, for any step, sign and offset") {
    // A CPU model of the toroidal volume: slot -> the world cell index (x, z) it holds. After a scroll, every slot gi_scroll
    // did not name still holds the world probe the box now wants there (bit for bit: kept), and every slot it named holds
    // one that left: the named slots are exactly the changed ones, and `probes` counts them over the y layers.
    for (const u32 cx : {2u, 3u, 5u, 8u, 32u}) {
        for (const u32 cz : {2u, 4u, 7u}) {
            const u32 counts[3] = {cx, 3, cz};
            const auto wanted = [&](GiFollowCell f, u32 sx, u32 sz, i64& wx, i64& wz) {  // the probe slot (sx, sz) holds
                u32 o[3];
                gi_slot_offset_of(f, counts, o);
                wx = f.x + i64(gi_local_of(sx, o[0], cx));
                wz = f.z + i64(gi_local_of(sz, o[2], cz));
                CHECK(gi_slot(wx, cx) == sx);  // world index mod count: the slot (gi_slot_of and gi_local_of agree)
                CHECK(gi_slot_of(gi_local_of(sx, o[0], cx), o[0], cx) == sx);
            };
            GiFollowCell at{-37, 12};
            Array<i64> hx, hz;  // what each slot holds
            hx.resize(usize(cx) * cz);
            hz.resize(usize(cx) * cz);
            for (u32 sz = 0; sz < cz; ++sz)
                for (u32 sx = 0; sx < cx; ++sx) wanted(at, sx, sz, hx[sz * cx + sx], hz[sz * cx + sx]);
            // Steps: one cell each way, several, mixed, a non-step, up to the box (refill).
            const i64 steps[][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {2, -1}, {-3, 2}, {0, 0}, {1, 1}, {-1, -1},
                                    {i64(cx) - 1, 0}, {0, i64(cz) - 1}, {-(i64(cx) - 1), -(i64(cz) - 1)},
                                    {i64(cx), 0}, {0, -i64(cz)}, {5 * i64(cx) + 1, 0}};
            for (const auto& st : steps) {
                const GiFollowCell to{at.x + st[0], at.z + st[1]};
                const GiScroll sc = gi_scroll(at, to, counts);
                const bool beyond = st[0] >= i64(cx) || -st[0] >= i64(cx) || st[1] >= i64(cz) || -st[1] >= i64(cz);
                CHECK(sc.refill == beyond);
                u32 changed = 0;
                for (u32 sz = 0; sz < cz; ++sz)
                    for (u32 sx = 0; sx < cx; ++sx) {
                        i64 wx, wz;
                        wanted(to, sx, sz, wx, wz);
                        const usize i = sz * cx + sx;
                        const bool differs = hx[i] != wx || hz[i] != wz;
                        changed += differs;
                        if (!sc.refill) {
                            const bool named = ((sx + cx - sc.x_first) % cx < sc.x_count) || ((sz + cz - sc.z_first) % cz < sc.z_count);
                            CHECK(named == differs);
                        }
                        hx[i] = wx;
                        hz[i] = wz;
                    }
                if (!sc.refill) CHECK(sc.probes == changed * counts[1]);
                else CHECK(sc.probes == cx * counts[1] * cz);
                at = to;
            }
        }
    }
}

TEST_CASE("gi_follow_origin with gi_scroll: a camera moving every frame, by fractions of a cell, both ways, and far away") {
    // The camera walks in steps that are not multiples of the cell, back and forth, crossing zero and far from it; the
    // snapped origin moves only at cell edges, by the cells crossed; a jump of a whole box asks for a refill.
    const u32 counts[3] = {16, 4, 16};
    const f64 cell = 2.5;
    GiFollowCell at = gi_follow_origin(-3.7, 1.2, cell, cell, 16, 16);
    f64 x = -3.7, z = 1.2;
    u32 scrolls = 0, scrolled = 0;
    for (int frame = 0; frame < 400; ++frame) {
        const f64 dx = frame < 200 ? 0.37 : -0.61, dz = frame % 3 == 0 ? -0.9 : 0.4;
        x += dx;
        z += dz;
        const GiFollowCell to = gi_follow_origin(x, z, cell, cell, 16, 16);
        CHECK(to.x == i64(std::floor(x / cell)) - 7);
        CHECK(to.z == i64(std::floor(z / cell)) - 7);
        const GiScroll sc = gi_scroll(at, to, counts);
        CHECK_FALSE(sc.refill);
        CHECK(sc.x_count <= 1);  // less than a cell per frame
        if (sc.probes) {
            ++scrolls;
            scrolled += sc.probes;
        }
        at = to;
    }
    MESSAGE(scrolls << " scrolls, " << scrolled << " probes invalidated over 400 frames");
    CHECK(scrolls > 50);
    // 1e6 m away: still exact cells; a jump of a box refills.
    const GiFollowCell far = gi_follow_origin(1.0e6 + 1.0, -1.0e6 + 1.0, cell, cell, 16, 16);
    CHECK(far.x == 400000 - 7);
    CHECK(far.z == -400000 - 7);
    CHECK(gi_scroll(at, far, counts).refill);
}

TEST_CASE("gi_follow_origin and gi_follow_step: the camera stays near the box centre for odd and even counts; sway does not flap") {
    for (u32 n = 2; n <= 9; ++n) {
        INFO("count " << n);
        const f64 cell = 2.0;
        f64 worst = 0.0;  // the largest |camera - box centre|, in cells
        for (f64 x = -13.0; x <= 13.0; x += 0.0625) {
            const GiFollowCell f = gi_follow_origin(x, -x, cell, cell, n, n);
            const f64 centre = (f64(f.x) + 0.5 * f64(n - 1)) * cell;
            worst = math::max(worst, std::fabs(x - centre) / cell);
            // The camera's distance to each edge of the box (n - 1 cells long) on both sides.
            const f64 lo = x - f64(f.x) * cell, hi = f64(f.x + i64(n) - 1) * cell - x;
            CHECK(lo >= (0.5 * f64(n - 1) - 0.5) * cell - 1e-9);
            CHECK(hi >= (0.5 * f64(n - 1) - 0.5) * cell - 1e-9);
        }
        CHECK(worst <= 0.5 + 1e-9);
        // gi_follow_step: a walk keeps the camera within kGiFollowHysteresis cells of the centre (rebase-free).
        GiFollowCell at = gi_follow_origin(0.3, 0.3, cell, cell, n, n);
        for (f64 x = 0.3; x < 40.0; x += 0.37) {
            at = gi_follow_step(at, x, 0.3, cell, cell, n, n);
            CHECK(std::fabs(x / cell - (f64(at.x) + 0.5 * f64(n - 1))) <= kGiFollowHysteresis + 1e-9);
        }
        // Sway across a snap point by up to 0.2 cell each way (under the half cell of dead band), 600 frames: the box moves
        // at most once.
        const f64 edge = (n & 1u) ? 5.0 * cell + 0.5 * cell : 5.0 * cell;  // where gi_follow_origin steps
        at = gi_follow_origin(edge - 0.3 * cell, 0.0, cell, cell, n, n);
        u32 moves = 0;
        for (int i = 0; i < 600; ++i) {
            const f64 x = edge + (i % 2 ? 0.2 : -0.2) * cell * (0.5 + 0.5 * f64(i % 7) / 6.0);
            const GiFollowCell next = gi_follow_step(at, x, 0.0, cell, cell, n, n);
            moves += !(next == at);
            at = next;
        }
        CHECK(moves <= 1);
    }
}
