// M-SNOWCOVER 6b against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: snow cover whitens
// up-facing surfaces under a world-anchored noise mask, from one uniform branch (cover 0 is today's frame; the goldens
// hold it). The scene is the wet plaza (fixture3d.h). One frame per render.

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

// The open ground in front of the box and the sphere, on a 0.1 m grid (in sun and in shadow).
Array<f64> ground_lumas(const Fixture3D& f, const Array<u8>& px) {
    Array<f64> l;
    for (f32 x = -1.8f; x <= 1.8f; x += 0.1f)
        for (f32 z = 0.9f; z <= 2.5f; z += 0.1f) l.push_back(luma_at(f, px, {x, 0.0f, z}));
    return l;
}

// The wall's +x face (x = -2.45), clear of its edges.
Array<f64> wall_lumas(const Fixture3D& f, const Array<u8>& px) {
    Array<f64> l;
    for (f32 y = 0.25f; y <= 1.75f; y += 0.1f)
        for (f32 z = -1.6f; z <= 0.5f; z += 0.1f) l.push_back(luma_at(f, px, {-2.45f, y, z}));
    return l;
}

// Points on the metal sphere whose normal is `n` (normalized here).
f64 sphere_luma(const Fixture3D& f, const Array<u8>& px, vec3 n) {
    return luma_at(f, px, kWetSphere + math::normalize(n) * (kWetSphereRadius * 0.98f));
}

// The sphere's cap (normals within ~20 degrees of +y, toward the camera so the points are visible).
Array<f64> sphere_top_lumas(const Fixture3D& f, const Array<u8>& px) {
    Array<f64> l;
    for (f32 a = -0.3f; a <= 0.3f; a += 0.1f)
        for (f32 b = 0.0f; b <= 0.3f; b += 0.1f) l.push_back(sphere_luma(f, px, {a, 1.0f, b}));
    return l;
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

// No bloom: the white ground's glow would spill onto the wall and the sphere (not what is measured).
Array<u8> draw_plaza(Fixture3D& f, const SnowCover& snow, const WetSurfaces& wet = {}) {
    f.render(0.0f, true, [&](Scene3D& s) {
        snow_plaza_scene(s, snow, wet);
        PostFx post = s.post();
        post.bloom = false;
        s.set_post(post);
    });
    return f.pixels;
}

}  // namespace

TEST_CASE("render3d_gl: cover 0 draws the snowless frame; cover 1 whitens the ground, not the +x wall") {
    Fixture3D f;
    upload_wet_plaza(f);
    const Array<u8> bare = draw_plaza(f, SnowCover{});
    SnowCover off = snow_plaza_snow(0.0f);  // every other field set: cover 0 alone switches it off
    off.coverage = 1.0f;
    off.wind_bias = 1.0f;
    off.noise_scale = 1.0f;
    CHECK(same(draw_plaza(f, off), bare));
    const Array<u8> snow = draw_plaza(f, snow_plaza_snow(1.0f));

    const f64 g_bare = mean(ground_lumas(f, bare)), g_snow = mean(ground_lumas(f, snow));
    const f64 w_bare = mean(wall_lumas(f, bare)), w_snow = mean(wall_lumas(f, snow));
    const f64 t_bare = mean(sphere_top_lumas(f, bare)), t_snow = mean(sphere_top_lumas(f, snow));
    const f64 rise = g_snow / g_bare - 1.0, wall_change = std::fabs(w_snow / w_bare - 1.0);
    MESSAGE("ground mean " << g_bare << " -> " << g_snow << " (+" << rise * 100.0 << "%); wall " << w_bare << " -> "
                           << w_snow << " (" << wall_change * 100.0 << "%); metal sphere top " << t_bare << " -> "
                           << t_snow);
    // Measured +9.88% on llvmpipe and lavapipe: the fixture's ground is already a light 0.7 grey, so ~80% of it going
    // to 0.9 white is +23% in linear albedo and less after the tonemap. Pinned at the measured value, +-2 points.
    CHECK(rise > 0.0788);
    CHECK(rise < 0.1188);
    // The +x wall (n.y = 0) gets no snow, and with bloom off nothing else of the white ground reaches it.
    CHECK(wall_change < 0.01);  // measured 0.00% (38.50 -> 38.50)
    // The documented exception: the metal sphere's cap is up-facing (the geometric-normal stand-in is the smooth
    // vertex normal), so it collects snow like the ground and loses its metal look there: 126 -> 171.
    CHECK(t_snow > t_bare + 20.0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: coverage is the share of the up-facing ground that whitens at full cover") {
    Fixture3D f;
    f.pitch_deg = 89.0f;  // looking down on the ground: 1 m cells put dozens of patches in view
    const auto draw = [&](const SnowCover& s) {
        f.render(0.0f, true, [&](Scene3D& sc) { sc.set_snow_cover(s); });
        return f.pixels;
    };
    const Array<u8> bare = draw(SnowCover{});
    for (f32 coverage : {0.2f, 0.5f, 0.8f}) {
        SnowCover s = snow_plaza_snow(1.0f);
        s.noise_scale = 1.0f;
        s.coverage = coverage;
        const Array<u8> px = draw(s);
        u32 up = 0, white = 0;
        for (f32 x = -3.5f; x <= 3.5f; x += 0.1f)
            for (f32 z = -2.6f; z <= 2.6f; z += 0.1f) {
                if (std::fabs(x) < 0.8f && std::fabs(z) < 0.8f) continue;  // the box and its outline
                ++up;
                white += luma_at(f, px, {x, 0.0f, z}) > luma_at(f, bare, {x, 0.0f, z}) + 4.0;  // snow brightens it
            }
        const f64 share = f64(white) / f64(up);
        MESSAGE("coverage " << coverage << " at cover 1 whitens " << share * 100.0 << "% of " << up << " samples");
        CHECK(std::fabs(share - f64(coverage)) < 0.1);  // measured 14.0%, 46.0%, 79.4% (faint patch edges miss the +4)
    }
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the Low tier draws the no-noise frame: an even layer of the same mean cover") {
    Fixture3D f;
    upload_wet_plaza(f);
    const SnowCover s = snow_plaza_snow(1.0f);
    const Array<u8> noisy = draw_plaza(f, s);
    SnowCover flat = s;
    flat.noise_enabled = false;
    const Array<u8> even = draw_plaza(f, flat);
    const SnowCover low = apply_quality(quality_settings(QualityTier::Low), s);
    CHECK_FALSE(low.noise_enabled);
    CHECK(low.cover == 1.0f);
    CHECK(same(draw_plaza(f, low), even));
    CHECK_FALSE(same(even, noisy));
    const f64 g_noisy = mean(ground_lumas(f, noisy)), g_even = mean(ground_lumas(f, even));
    MESSAGE("ground mean: noise " << g_noisy << ", even layer " << g_even);
    // Low keeps the look and drops the breakup: measured 1.3% apart (170.3 with noise, 168.0 even).
    CHECK(std::fabs(g_even / g_noisy - 1.0) < 0.05);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: wind_bias favors the lee (+x-facing) side of a sphere's cap over the windward side") {
    Fixture3D f;
    constexpr vec3 kBall{-1.2f, 0.6f, 1.4f};
    upload_sphere(f, 52, kBall, 0.6f);  // a rough dielectric, tinted dark below: the snow shows on it
    SnowCover s = snow_plaza_snow(1.0f);
    s.noise_enabled = false;  // an even layer: the bias alone sets how white a point is
    s.coverage = 0.6f;
    const auto lee_minus_windward = [&](f32 bias) {
        s.wind_bias = bias;
        f.render(0.0f, true, [&](Scene3D& sc) {
            MeshInstance m;
            m.mesh = 52;
            m.tint = {0.15f, 0.15f, 0.15f, 1.0f};
            sc.add(m);
            sc.set_snow_cover(s);
        });
        const auto at = [&](f32 x) {
            return luma_at(f, f.pixels, kBall + math::normalize(vec3{x, 1.0f, 0.2f}) * 0.59f);
        };
        return at(0.45f) - at(-0.45f);
    };
    const f64 calm = lee_minus_windward(0.0f), windy = lee_minus_windward(1.0f);
    MESSAGE("sphere cap lee - windward luma: wind_bias 0 " << calm << ", wind_bias 1 " << windy);
    CHECK(windy > calm + 15.0);  // measured 13.9 -> 37.1 (the sun lights the two points differently)
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: snow wins over puddles and the wet darkening under it") {
    Fixture3D f;
    upload_wet_plaza(f);
    SnowCover s = snow_plaza_snow(1.0f);
    s.coverage = 1.0f;
    s.noise_enabled = false;  // the mask is exactly 1 on flat ground
    WetSurfaces flood;
    flood.wetness = 1.0f;
    flood.puddles = 1.0f;
    const Array<f64> snow_only = ground_lumas(f, draw_plaza(f, s));
    const Array<f64> wet_only = ground_lumas(f, draw_plaza(f, SnowCover{}, flood));
    const Array<f64> both = ground_lumas(f, draw_plaza(f, s, flood));
    const f64 diff = max_abs_diff(snow_only, both);
    MESSAGE("ground mean: snow " << mean(snow_only) << ", flooded " << mean(wet_only) << ", both " << mean(both)
                                 << " (largest sample difference to snow alone " << diff << ")");
    CHECK(mean(wet_only) < mean(snow_only) * 0.6);
    CHECK(diff <= 1.0);  // measured 0: under full snow the ground is the snow's whatever the water did
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: snow patches stay put across a 1000 m floating-origin shift") {
    Fixture3D f;
    upload_wet_plaza(f);
    SnowCover s = snow_plaza_snow(1.0f);
    s.noise_scale = 1.0f;
    s.coverage = 0.5f;  // half cover: the most patch edges in view
    // No shadows: their fit snaps to world-space texels, which a 1000 m move does not keep (not what is tested).
    f.render(0.0f, false, [&](Scene3D& sc) { snow_plaza_scene(sc, s, {}); });
    const Array<u8> home = f.pixels;
    const vec3 shift{1000.0f, 0.0f, 1000.0f};
    f.world_offset = -shift;  // the world moved by -shift ...
    f.render(0.0f, false, [&](Scene3D& sc) { snow_plaza_scene(sc, s, {}, f.world_offset); });
    const Array<u8> unannounced = f.pixels;  // ... without telling the renderer: the patches move
    f.render(0.0f, false, [&](Scene3D& sc) {
        snow_plaza_scene(sc, s, {}, f.world_offset);
        sc.set_origin_shift(shift);  // ... and told: Forward3D carries it into the noise
    });
    const Array<u8> shifted = f.pixels;
    // The snowless plaza across the same move: what 1000 m does to the rest of the frame (float positions), for scale.
    f.render(0.0f, false, [&](Scene3D& sc) { snow_plaza_scene(sc, SnowCover{}, {}, f.world_offset); });
    const Array<u8> bare_shifted = f.pixels;
    f.world_offset = {0.0f, 0.0f, 0.0f};
    f.render(0.0f, false, [&](Scene3D& sc) { snow_plaza_scene(sc, SnowCover{}, {}); });
    const f64 moved = frame_change(home, shifted), control = frame_change(home, unannounced);
    const f64 bare_moved = frame_change(f.pixels, bare_shifted);
    MESSAGE("frame change after the shift " << moved * 100.0 << "% (snowless: " << bare_moved * 100.0
                                            << "%), without set_origin_shift " << control * 100.0 << "%");
    CHECK(moved < bare_moved + 0.001);  // measured 0.094% (snowless 0.091%), without the shift 2.26%
    CHECK(control > 10.0 * moved);
    CHECK(f.device->validation_error_count() == 0);
}
