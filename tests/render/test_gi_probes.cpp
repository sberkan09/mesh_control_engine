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
    // Odd counts centre too.
    CHECK(gi_follow_origin(0.5, 0.5, 1.0, 1.0, 5, 5).x == -2);
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
