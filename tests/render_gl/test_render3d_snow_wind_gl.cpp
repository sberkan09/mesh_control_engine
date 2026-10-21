// M-WINDDIR 8c against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: the snow's lee bias
// leans along SnowCover::wind_dir (p_snow2.xy) instead of a hard-wired +x. The default (1, 0) is the pre-8c frame bit
// for bit (the goldens hold it); turning the wind moves the extra snow to the slopes facing the new way. The scene is
// the snow plaza (fixture3d.h). One frame per render.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "fixture3d.h"
#include "mce/render/quality.h"

using namespace mce;
using namespace mce::render;
using namespace mce::render_test;

namespace {

f64 luma_at(const Fixture3D& f, const Array<u8>& px, vec3 p) {
    const math::vec4 c = f.view.view_proj * math::vec4(p, 1.0f);
    const f32 x = (c.x / c.w * 0.5f + 0.5f) * kW, y = (0.5f - c.y / c.w * 0.5f) * kH;
    REQUIRE(x >= 0);
    REQUIRE(y >= 0);
    REQUIRE(x < kW);
    REQUIRE(y < kH);
    const usize i = (usize(y) * kW + usize(x)) * 4;
    return 0.2126 * px[i] + 0.7152 * px[i + 1] + 0.0722 * px[i + 2];
}

// The open ground in front of the box and the sphere, on a 0.1 m grid (as the snow test's).
Array<f64> ground_lumas(const Fixture3D& f, const Array<u8>& px) {
    Array<f64> l;
    for (f32 x = -1.8f; x <= 1.8f; x += 0.1f)
        for (f32 z = 0.9f; z <= 2.5f; z += 0.1f) l.push_back(luma_at(f, px, {x, 0.0f, z}));
    return l;
}

// The slopes: a rough dark dielectric ball (mesh 52, as the snow test's wind_bias case) on the plaza's open ground,
// where more snow is always brighter (on the plaza's metal sphere it is not: its mirror-bright side darkens under
// snow). slope_luma is the mean luma of the ball's cap on its +x-facing (side = 1) or -x-facing (side = -1) slope:
// normals (side * 0.35..0.55, 1, 0.1..0.3), 22 to 29 degrees off +y (fully up-facing for the mask), tilted toward
// the camera so the points are visible. Both sides have the same z, so a wind along z gives them the same mask.
constexpr vec3 kBall{-1.2f, 0.6f, 1.4f};
constexpr f32 kBallRadius = 0.6f;

f64 slope_luma(const Fixture3D& f, const Array<u8>& px, f32 side) {
    f64 s = 0.0;
    u32 n = 0;
    for (f32 a = 0.35f; a <= 0.56f; a += 0.1f)
        for (f32 b = 0.1f; b <= 0.31f; b += 0.1f, ++n)
            s += luma_at(f, px, kBall + math::normalize(vec3{side * a, 1.0f, b}) * (kBallRadius * 0.98f));
    return s / f64(n);
}

template <class T>
bool same(const Array<T>& a, const Array<T>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

f64 mean(const Array<f64>& v) {
    f64 s = 0.0;
    for (f64 x : v) s += x;
    return v.empty() ? 0.0 : s / f64(v.size());
}

f64 max_abs_diff(const Array<f64>& a, const Array<f64>& b) {
    f64 m = 0.0;
    for (usize i = 0; i < a.size() && i < b.size(); ++i) m = std::max(m, std::fabs(a[i] - b[i]));
    return m;
}

// Mean |luma difference| over the whole frame, of full scale.
f64 frame_change(const Array<u8>& a, const Array<u8>& b) {
    f64 s = 0.0;
    for (usize i = 0; i < a.size(); i += 4) {
        const f64 la = 0.2126 * a[i] + 0.7152 * a[i + 1] + 0.0722 * a[i + 2];
        const f64 lb = 0.2126 * b[i] + 0.7152 * b[i + 1] + 0.0722 * b[i + 2];
        s += std::fabs(la - lb);
    }
    return s / (f64(a.size()) / 4.0 * 255.0);
}

SnowCover windy(SnowCover s, f32 x, f32 z) {
    s.wind_dir = {x, z};
    return s;
}

// No bloom: the white ground's glow would spill onto the slopes (not what is measured). `ball`: add mesh 52.
Array<u8> draw_plaza(Fixture3D& f, const SnowCover& snow, bool ball = false) {
    f.render(0.0f, true, [&](Scene3D& s) {
        snow_plaza_scene(s, snow, {});
        if (ball) {
            MeshInstance m;
            m.mesh = 52;
            m.tint = {0.15f, 0.15f, 0.15f, 1.0f};
            s.add(m);
        }
        PostFx post = s.post();
        post.bloom = false;
        s.set_post(post);
    });
    return f.pixels;
}

}  // namespace

TEST_CASE("render3d_gl: wind_dir (1, 0) draws the pre-8c snow frame; cover 0 stays the snowless one in any wind") {
    Fixture3D f;
    upload_wet_plaza(f);
    const SnowCover s = snow_plaza_snow(1.0f);  // wind_dir defaults to (1, 0)
    const Array<u8> plain = draw_plaza(f, s);
    CHECK(same(draw_plaza(f, windy(s, 1.0f, 0.0f)), plain));
    CHECK(same(draw_plaza(f, windy(s, 3.0f, 0.0f)), plain));  // normalized on upload: the same direction
    CHECK(same(draw_plaza(f, windy(s, 0.0f, 0.0f)), plain));  // zero length falls back to +x
    // In the golden's full cover the plaza sphere's cap lies inside one 2 m noise patch whose mask is 1 on both
    // sides whatever the wind, so (-1, 0) draws that frame too (the golden snow_plaza_west takes the even layer).
    CHECK(same(draw_plaza(f, windy(s, -1.0f, 0.0f)), plain));
    SnowCover even = s;
    even.noise_enabled = false;
    even.coverage = 0.6f;
    CHECK_FALSE(same(draw_plaza(f, windy(even, -1.0f, 0.0f)), draw_plaza(f, even)));
    const Array<u8> bare = draw_plaza(f, SnowCover{});
    CHECK(same(draw_plaza(f, windy(snow_plaza_snow(0.0f), -1.0f, 0.0f)), bare));
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: turning the wind moves the lee snow to the slopes facing the new way; flat ground keeps it") {
    Fixture3D f;
    upload_wet_plaza(f);
    upload_sphere(f, 52, kBall, kBallRadius);
    SnowCover s = snow_plaza_snow(1.0f);
    s.noise_enabled = false;  // an even layer: the bias alone sets how white a point is
    s.coverage = 0.6f;        // below 1 on both slopes: neither side saturates
    const Array<u8> bare = draw_plaza(f, SnowCover{}, true);
    const f64 bare_e = slope_luma(f, bare, 1.0f), bare_w = slope_luma(f, bare, -1.0f);
    struct Side {
        f64 east, west;  // whitening over the bare frame of the +x- and the -x-facing slope
    };
    const auto side = [&](f32 x, f32 z) {
        const Array<u8> px = draw_plaza(f, windy(s, x, z), true);
        return Side{slope_luma(f, px, 1.0f) - bare_e, slope_luma(f, px, -1.0f) - bare_w};
    };
    const Side east = side(1.0f, 0.0f), west = side(-1.0f, 0.0f), north = side(0.0f, 1.0f);
    MESSAGE("ball slope whitening (+x-facing / -x-facing): wind +x " << east.east << " / " << east.west << ", wind -x "
                                                                     << west.east << " / " << west.west << ", wind +z "
                                                                     << north.east << " / " << north.west);
    // The lee slope takes the extra snow: with the wind toward +x the +x-facing one, toward -x the other one.
    // Measured on llvmpipe: wind +x 84.7 / 69.8, wind -x 64.2 / 90.0, wind +z 80.1 / 85.8 (the sun lights the -x
    // side more, so the same mask whitens it more: the two sides' gap is +15.0 / -25.8 / -5.6, not mirror-equal).
    CHECK(east.east > east.west + 10.0);
    CHECK(west.west > west.east + 10.0);
    CHECK(east.east > west.east + 10.0);
    CHECK(west.west > east.west + 10.0);
    // The mirror: what turning the wind 180 degrees takes from a slope's windward value to its lee value is the same
    // on both sides (measured 20.5 on +x, 20.2 on -x).
    CHECK(std::fabs((east.east - west.east) - (west.west - east.west)) < 3.0);
    // A wind along z treats both slopes alike (same z): each lands between its lee and its windward value and the
    // two sides' gap closes to what the lighting alone makes of the same mask.
    CHECK(north.east < east.east);
    CHECK(north.east > west.east);
    CHECK(north.west < west.west);
    CHECK(north.west > east.west);
    CHECK(std::fabs(north.east - north.west) < 0.5 * std::fabs(east.east - east.west));
    CHECK(std::fabs(north.east - north.west) < 0.5 * std::fabs(west.east - west.west));

    // Flat ground (ng.xz = 0) takes the same snow whichever way the wind blows (with the noise, as in the golden).
    const SnowCover full = snow_plaza_snow(1.0f);
    const Array<f64> g_east = ground_lumas(f, draw_plaza(f, full));
    const Array<f64> g_west = ground_lumas(f, draw_plaza(f, windy(full, -1.0f, 0.0f)));
    const Array<f64> g_north = ground_lumas(f, draw_plaza(f, windy(full, 0.0f, 1.0f)));
    const f64 d_west = max_abs_diff(g_east, g_west), d_north = max_abs_diff(g_east, g_north);
    MESSAGE("flat ground mean " << mean(g_east) << " / " << mean(g_west) << " / " << mean(g_north)
                                << " (largest sample difference to +x: -x " << d_west << ", +z " << d_north << ")");
    CHECK(d_west <= 1.0);  // measured 0 for both: the ground's interpolated normal is exactly (0, 1, 0)
    CHECK(d_north <= 1.0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the Low tier keeps the wind direction; the patches stay put across an origin shift") {
    Fixture3D f;
    upload_wet_plaza(f);
    const SnowCover s = windy(snow_plaza_snow(1.0f), -1.0f, 0.0f);
    SnowCover flat = s;
    flat.noise_enabled = false;
    const Array<u8> even = draw_plaza(f, flat);
    const SnowCover low = apply_quality(quality_settings(QualityTier::Low), s);
    CHECK_FALSE(low.noise_enabled);
    CHECK(low.wind_dir.x == -1.0f);
    CHECK(low.wind_dir.y == 0.0f);
    CHECK(same(draw_plaza(f, low), even));
    CHECK_FALSE(same(draw_plaza(f, windy(flat, 1.0f, 0.0f)), even));

    // As the snow test's shift check, in a west wind: the direction does not touch the noise's anchoring.
    SnowCover half = s;
    half.noise_scale = 1.0f;
    half.coverage = 0.5f;
    f.render(0.0f, false, [&](Scene3D& sc) { snow_plaza_scene(sc, half, {}); });
    const Array<u8> home = f.pixels;
    const vec3 shift{1000.0f, 0.0f, 1000.0f};
    f.world_offset = -shift;
    f.render(0.0f, false, [&](Scene3D& sc) {
        snow_plaza_scene(sc, half, {}, f.world_offset);
        sc.set_origin_shift(shift);
    });
    const Array<u8> shifted = f.pixels;
    f.render(0.0f, false, [&](Scene3D& sc) { snow_plaza_scene(sc, SnowCover{}, {}, f.world_offset); });
    const Array<u8> bare_shifted = f.pixels;
    f.world_offset = {0.0f, 0.0f, 0.0f};
    f.render(0.0f, false, [&](Scene3D& sc) { snow_plaza_scene(sc, SnowCover{}, {}); });
    const f64 moved = frame_change(home, shifted), bare_moved = frame_change(f.pixels, bare_shifted);
    MESSAGE("west wind: frame change after the shift " << moved * 100.0 << "% (snowless: " << bare_moved * 100.0
                                                       << "%)");
    CHECK(moved < bare_moved + 0.001);
    CHECK(f.device->validation_error_count() == 0);
}
