// DecalScatter3D (M-DECALSCATTER 35a): decal i comes from (seed, i) and the authoring only, so it is deterministic and
// independent of count; offsets stay in the area, sizes in [size_min, size_max], atlas cells in the atlas; the distance fade
// is 1 near, 0 at the limit and monotonic; NaN and inverted ranges fall back to the defaults; the component defaults hold.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>

#include "mce/kit/decal_scatter.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }

bool same_item(const ScatterDecal& a, const ScatterDecal& b) {
    return same_bits(a.x, b.x) && same_bits(a.z, b.z) && same_bits(a.yaw, b.yaw) && same_bits(a.size, b.size) &&
           a.cell_x == b.cell_x && a.cell_y == b.cell_y && same_bits(a.tint.x, b.tint.x) && same_bits(a.tint.y, b.tint.y) &&
           same_bits(a.tint.z, b.tint.z);
}

}  // namespace

TEST_CASE("decal scatter: defaults") {
    const DecalScatter3D s;
    CHECK(s.enabled == 1);
    CHECK(s.atlas_cols == 1);
    CHECK(s.atlas_rows == 1);
    CHECK(s.count == 16);
    CHECK(s.area.x == 4.0f);
    CHECK(s.area.y == 4.0f);
    CHECK(s.depth_m == 1.0f);
    CHECK(s.size_min == 0.4f);
    CHECK(s.size_max == 1.2f);
    CHECK(s.seed == 1);
    CHECK(s.color.x == 1.0f);
    CHECK(s.color.w == 1.0f);
    CHECK(s.tint_jitter == 0.2f);
    CHECK(s.roughness == 0.7f);
    CHECK(s.metallic == 0.0f);
    CHECK(s.material_weight == 1.0f);
    CHECK(s.angle_fade == 0.2f);
    CHECK(s.edge_fade == 0.2f);
    CHECK(s.fade_distance_m == 60.0f);
    CHECK(s.strength == 1.0f);
    CHECK(s.pad_[0] == 0);
    CHECK(s.pad_[1] == 0);
    CHECK(s.pad_[2] == 0);
}

TEST_CASE("decal scatter: the same seed and index give the same decal, another seed another one") {
    DecalScatter3D s;
    s.atlas_cols = 4;
    s.atlas_rows = 4;
    int differing = 0;
    for (u32 i = 0; i < 64; ++i) {
        CHECK(same_item(decal_scatter_item(s, i), decal_scatter_item(s, i)));
        DecalScatter3D t = s;
        t.seed = 2;
        if (!same_item(decal_scatter_item(s, i), decal_scatter_item(t, i))) ++differing;
    }
    CHECK(differing == 64);
    CHECK_FALSE(same_item(decal_scatter_item(s, 0), decal_scatter_item(s, 1)));
}

TEST_CASE("decal scatter: offsets, sizes, yaw, cells and tint stay in range") {
    DecalScatter3D s;
    s.area = {3.0f, 7.0f};
    s.size_min = 0.5f;
    s.size_max = 2.0f;
    s.atlas_cols = 3;
    s.atlas_rows = 5;
    s.tint_jitter = 0.5f;
    bool seen_cell_x[3] = {};
    bool seen_cell_y[5] = {};
    for (u32 seed : {1u, 7u, 0xFFFFFFFFu}) {
        s.seed = seed;
        for (u32 i = 0; i < 256; ++i) {
            const ScatterDecal d = decal_scatter_item(s, i);
            CHECK(std::fabs(d.x) <= 3.0f);
            CHECK(std::fabs(d.z) <= 7.0f);
            CHECK(d.size >= 0.5f);
            CHECK(d.size <= 2.0f);
            CHECK(d.yaw >= 0.0f);
            CHECK(d.yaw < 6.2832f);
            REQUIRE(d.cell_x < 3);
            REQUIRE(d.cell_y < 5);
            seen_cell_x[d.cell_x] = true;
            seen_cell_y[d.cell_y] = true;
            CHECK(d.tint.x <= 1.0f);
            CHECK(d.tint.x >= 0.5f);
            CHECK(d.tint.y >= 0.5f);
            CHECK(d.tint.z >= 0.5f);
        }
    }
    for (bool b : seen_cell_x) CHECK(b);
    for (bool b : seen_cell_y) CHECK(b);
}

TEST_CASE("decal scatter: a one-cell atlas always gives cell 0 and no jitter gives a white tint") {
    DecalScatter3D s;
    s.tint_jitter = 0.0f;
    for (u32 i = 0; i < 32; ++i) {
        const ScatterDecal d = decal_scatter_item(s, i);
        CHECK(d.cell_x == 0);
        CHECK(d.cell_y == 0);
        CHECK(d.tint.x == 1.0f);
        CHECK(d.tint.y == 1.0f);
        CHECK(d.tint.z == 1.0f);
    }
}

TEST_CASE("decal scatter: item i does not depend on count or on the other authoring that does not shape it") {
    DecalScatter3D a;
    DecalScatter3D b;
    a.count = 4;
    b.count = 256;
    b.strength = 0.3f;
    b.fade_distance_m = 10.0f;
    b.color = {0.2f, 0.3f, 0.4f, 0.5f};
    b.enabled = 0;
    for (u32 i = 0; i < 4; ++i) CHECK(same_item(decal_scatter_item(a, i), decal_scatter_item(b, i)));
}

TEST_CASE("decal scatter: the cells of a wide atlas are clamped to 8 and a zero atlas counts as one cell") {
    DecalScatter3D s;
    s.atlas_cols = 100;
    s.atlas_rows = 0;
    for (u32 i = 0; i < 64; ++i) {
        const ScatterDecal d = decal_scatter_item(s, i);
        CHECK(d.cell_x < 8);
        CHECK(d.cell_y == 0);
    }
}

TEST_CASE("decal scatter: NaN and inverted ranges fall back to the defaults") {
    DecalScatter3D bad;
    bad.size_min = 3.0f;  // inverted pair
    bad.size_max = 1.0f;
    bad.area = {kNaN, -2.0f};
    bad.tint_jitter = kNaN;
    const DecalScatter3D def;
    for (u32 i = 0; i < 64; ++i) {
        const ScatterDecal d = decal_scatter_item(bad, i);
        CHECK(same_item(d, decal_scatter_item(def, i)));
        CHECK(d.size >= 0.4f);
        CHECK(d.size <= 1.2f);
        CHECK(std::fabs(d.x) <= 4.0f);
        CHECK(std::fabs(d.z) <= 4.0f);
    }
    DecalScatter3D nan_sizes;
    nan_sizes.size_min = kNaN;
    nan_sizes.size_max = kNaN;
    for (u32 i = 0; i < 16; ++i) CHECK(same_item(decal_scatter_item(nan_sizes, i), decal_scatter_item(def, i)));
}

TEST_CASE("decal scatter: a zero-size area puts every decal at the centre and equal sizes give that size") {
    DecalScatter3D s;
    s.area = {0.0f, 0.0f};
    s.size_min = 1.0f;
    s.size_max = 1.0f;
    for (u32 i = 0; i < 16; ++i) {
        const ScatterDecal d = decal_scatter_item(s, i);
        CHECK(std::fabs(d.x) == 0.0f);
        CHECK(std::fabs(d.z) == 0.0f);
        CHECK(d.size == 1.0f);
    }
}

TEST_CASE("decal scatter: the fade is 1 near, 0 at the limit and monotonic") {
    CHECK(decal_scatter_fade(0.0f, 60.0f) == 1.0f);
    CHECK(decal_scatter_fade(10.0f, 60.0f) == 1.0f);
    CHECK(decal_scatter_fade(60.0f, 60.0f) == 0.0f);
    CHECK(decal_scatter_fade(500.0f, 60.0f) == 0.0f);
    CHECK(decal_scatter_fade(45.0f, 60.0f) == doctest::Approx(0.5f).epsilon(1e-4));
    f32 prev = 1.0f;
    for (int k = 0; k <= 200; ++k) {
        const f32 f = decal_scatter_fade(static_cast<f32>(k) * 0.4f, 60.0f);
        CHECK(f >= 0.0f);
        CHECK(f <= 1.0f);
        CHECK(f <= prev);
        prev = f;
    }
}

TEST_CASE("decal scatter: no fade distance means no fade, and NaN inputs are safe") {
    CHECK(decal_scatter_fade(1000.0f, 0.0f) == 1.0f);
    CHECK(decal_scatter_fade(1000.0f, -5.0f) == 1.0f);
    CHECK(decal_scatter_fade(1000.0f, kNaN) == 1.0f);
    CHECK(decal_scatter_fade(kNaN, 60.0f) == 1.0f);
    CHECK(decal_scatter_fade(-3.0f, 60.0f) == 1.0f);
}
