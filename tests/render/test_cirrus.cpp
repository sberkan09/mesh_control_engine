// M-CIRRUS 53b: the cirrus layer's CPU twin (mce/render/cirrus.h), the helpers test_render3d_cirrus_gl compares the sky
// shader's apply_cirrus with: the uniforms and their gate, the drift wrap, the wind-aligned anisotropic coordinates,
// the noise's exact wrap, the opacity cap and horizon fade, and the lighting (later sunset, dusk warmth, silver near
// the sun).

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "mce/render/cirrus.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;
using math::vec2;
using math::vec3;

namespace {

Cirrus on(f32 coverage = 0.55f) {
    Cirrus c;
    c.coverage = coverage;
    return c;
}

vec3 dir(f32 elevation_deg, f32 azimuth_deg) {
    const f32 e = elevation_deg * 0.017453292f, a = azimuth_deg * 0.017453292f;
    return vec3{std::cos(e) * std::cos(a), std::sin(e), std::cos(e) * std::sin(a)};
}

bool all_zero(const CirrusParams& p) {
    return p.coverage == 0.0f && p.density == 0.0f && p.inv_cell == 0.0f && p.height_m == 0.0f && p.wind.x == 0.0f &&
           p.wind.y == 0.0f && p.stretch == 0.0f && p.drift == 0.0f;
}

constexpr f32 kNan = std::numeric_limits<f32>::quiet_NaN();
constexpr f32 kInf = std::numeric_limits<f32>::infinity();

}  // namespace

TEST_CASE("cirrus twin: the gate: coverage 0, NaN, disabled and the Low tier give all-zero uniforms") {
    CHECK(all_zero(cirrus_shader_params(Cirrus{}, 30.0f)));  // the default carrier: coverage 0
    CHECK(all_zero(cirrus_shader_params(on(kNan), 30.0f)));
    CHECK(all_zero(cirrus_shader_params(on(-0.5f), 30.0f)));
    Cirrus off = on(0.9f);
    off.enabled = false;
    CHECK(all_zero(cirrus_shader_params(off, 30.0f)));
    CHECK(all_zero(cirrus_shader_params(apply_quality(quality_settings(QualityTier::Low), on(0.9f)), 30.0f)));
    const CirrusParams p = cirrus_shader_params(on(0.55f), 0.0f);
    CHECK(p.coverage == 0.55f);
    CHECK(p.density == 0.35f);
    CHECK(p.inv_cell == 1.0f / 3000.0f);
    CHECK(p.height_m == 9000.0f);
    CHECK(p.wind.x == 1.0f);
    CHECK(p.wind.y == 0.0f);
    CHECK(p.stretch == 6.0f);
    CHECK(p.drift == 0.0f);
    // Off: every direction is untouched.
    CHECK(cirrus_opacity(dir(40.0f, 10.0f), cirrus_shader_params(Cirrus{}, 0.0f)) == 0.0f);
    const vec3 c{0.2f, 0.4f, 0.8f};
    const vec3 out = cirrus_apply(c, dir(40.0f, 10.0f), dir(30.0f, 0.0f), vec3{2.0f, 2.0f, 2.0f},
                                  vec3{0.3f, 0.3f, 0.3f}, cirrus_shader_params(Cirrus{}, 0.0f));
    CHECK(out.x == c.x);
    CHECK(out.y == c.y);
    CHECK(out.z == c.z);
}

TEST_CASE("cirrus twin: the uniforms are clamped again, NaN falls back to the defaults, the wind is normalized") {
    Cirrus c = on(2.0f);
    c.density = kNan;
    c.scale_m = 10.0f;
    c.stretch = 99.0f;
    c.height_m = kInf;
    c.wind_dir = {0.0f, 0.0f};
    const CirrusParams p = cirrus_shader_params(c, 0.0f);
    CHECK(p.coverage == 1.0f);
    CHECK(p.density == 0.35f);
    CHECK(p.inv_cell == 1.0f / 200.0f);
    CHECK(p.stretch == 16.0f);
    CHECK(p.height_m == 30000.0f);
    CHECK(p.wind.x == 1.0f);
    CHECK(p.wind.y == 0.0f);
    c.wind_dir = {3.0f, -4.0f};
    c.scale_m = kNan;
    c.stretch = kNan;
    c.height_m = 10.0f;
    const CirrusParams q = cirrus_shader_params(c, 0.0f);
    CHECK(q.wind.x == doctest::Approx(0.6f));
    CHECK(q.wind.y == doctest::Approx(-0.8f));
    CHECK(q.inv_cell == 1.0f / 3000.0f);
    CHECK(q.stretch == 6.0f);
    CHECK(q.height_m == 1000.0f);
}

TEST_CASE("cirrus twin: the drift is time * speed * 10 / (scale * stretch) cells, wrapped in f64 to 64") {
    CHECK(cirrus_drift(0.0f, 2.0f, 3000.0f, 6.0f) == 0.0f);
    CHECK(cirrus_drift(900.0f, 2.0f, 3000.0f, 6.0f) == doctest::Approx(1.0f));  // 900 * 20 / 18000
    CHECK(cirrus_drift(kNan, 2.0f, 3000.0f, 6.0f) == 0.0f);
    CHECK(cirrus_drift(kInf, 2.0f, 3000.0f, 6.0f) == 0.0f);
    CHECK(cirrus_drift(900.0f, 0.0f, 3000.0f, 6.0f) == 0.0f);                   // speed 0 holds still
    CHECK(cirrus_drift(900.0f, kNan, 3000.0f, 6.0f) == doctest::Approx(1.0f));  // NaN speed: the default 2
    // One period (64 cells) later the drift is the same f32; a negative time wraps into 0..64.
    CHECK(cirrus_drift(20.0f, 2.0f, 3000.0f, 6.0f) == cirrus_drift(57620.0f, 2.0f, 3000.0f, 6.0f));
    const f32 back = cirrus_drift(-900.0f, 2.0f, 3000.0f, 6.0f);
    CHECK(back == doctest::Approx(63.0f));
    // A long run: still in range and still moving.
    for (const f32 t : {1.0e5f, 3.3e6f, 1.0e8f}) {
        const f32 d = cirrus_drift(t, 3.0f, 2500.0f, 7.0f);
        CHECK(d >= 0.0f);
        CHECK(d < kCirrusPeriod);
    }
    // In the uniforms.
    CHECK(cirrus_shader_params(on(), 900.0f).drift == cirrus_drift(900.0f, 2.0f, 3000.0f, 6.0f));
}

TEST_CASE("cirrus twin: coordinates are stretched along the wind and wrap at 64 cells") {
    const CirrusParams p = cirrus_shader_params(on(), 0.0f);  // wind +x, stretch 6, 3000 m cells, 9000 m up
    vec2 q;
    // Straight up: the plane point under the camera.
    REQUIRE(cirrus_coords(vec3{0.0f, 1.0f, 0.0f}, p, q));
    CHECK(q.x == 0.0f);
    CHECK(q.y == 0.0f);
    // A point 4500 m east and 4500 m south on the plane (d = (0.5, 1, 0.5) unnormalized: x = 0.5 * 9000).
    REQUIRE(cirrus_coords(vec3{0.5f, 1.0f, 0.5f}, p, q));
    CHECK(q.x == doctest::Approx(4500.0f / 3000.0f / 6.0f));  // along the wind: 6 times longer cells
    CHECK(q.y == doctest::Approx(4500.0f / 3000.0f));         // across: w' = (-w.y, w.x) = (0, 1)
    // The drift moves the pattern along the wind only.
    const CirrusParams pd = cirrus_shader_params(on(), 900.0f);  // drift 1 cell
    vec2 qd;
    REQUIRE(cirrus_coords(vec3{0.5f, 1.0f, 0.5f}, pd, qd));
    CHECK(qd.x == doctest::Approx(q.x - 1.0f + 64.0f));
    CHECK(qd.y == q.y);
    // Rays at or under the limit, non-finite and the off layer have no coordinates.
    CHECK_FALSE(cirrus_coords(vec3{1.0f, kCirrusMinUp, 0.0f}, p, q));
    CHECK_FALSE(cirrus_coords(vec3{1.0f, -0.3f, 0.0f}, p, q));
    CHECK_FALSE(cirrus_coords(vec3{kNan, 0.5f, 0.0f}, p, q));
    CHECK_FALSE(cirrus_coords(vec3{0.0f, 1.0f, 0.0f}, CirrusParams{}, q));
    // A low ray far out still lands in 0..64.
    REQUIRE(cirrus_coords(math::normalize(vec3{0.7f, 0.021f, -0.7f}), p, q));
    CHECK(q.x >= 0.0f);
    CHECK(q.x < 64.0f);
    CHECK(q.y >= 0.0f);
    CHECK(q.y < 64.0f);
}

TEST_CASE("cirrus twin: the noise is in 0..1 and seamless across the 64-cell period on both axes") {
    f32 lo = 1.0f, hi = 0.0f;
    for (u32 i = 0; i < 64; ++i)
        for (u32 j = 0; j < 64; ++j) {
            const f32 n = cirrus_noise(vec2{f32(i) + 0.37f, f32(j) * 0.99f + 0.11f}, 1.0f);
            lo = n < lo ? n : lo;
            hi = n > hi ? n : hi;
        }
    CHECK(lo >= 0.0f);
    CHECK(hi <= 1.0f);
    CHECK(hi - lo > 0.4f);  // it varies
    // The wrap: the lattice at 0 and at 64 (the far edge of the period) is the same point; exactly along the wind,
    // and across it up to the rounding of the bent fibre coordinate (6 q.y + 3 b loses the low bits of 3 b at 384).
    for (const f32 y : {0.0f, 7.25f, 31.5f, 63.0f}) {
        CHECK(cirrus_noise(vec2{0.0f, y}, 1.0f) == cirrus_noise(vec2{64.0f, y}, 1.0f));
        CHECK(cirrus_noise(vec2{y, 0.0f}, 1.0f) == doctest::Approx(cirrus_noise(vec2{y, 64.0f}, 1.0f)).epsilon(1e-4));
    }
    // fine 0: the fibres at their mean, so the noise is the broad octave's alone, a smoother field.
    CHECK(cirrus_noise(vec2{12.5f, 3.25f}, 0.0f) != cirrus_noise(vec2{12.5f, 3.25f}, 1.0f));
    CHECK(cirrus_noise(vec2{kNan, 1.0f}, 1.0f) == 0.0f);
}

TEST_CASE("cirrus twin: the opacity is capped at 0.5, grows with coverage and density, and fades to the horizon") {
    Cirrus full = on(1.0f);
    full.density = 1.0f;
    const CirrusParams pf = cirrus_shader_params(full, 0.0f);
    f32 most = 0.0f;
    u32 covered[3] = {0, 0, 0};
    const f32 cov[3] = {0.2f, 0.5f, 0.9f};
    for (u32 i = 0; i < 40; ++i)
        for (u32 j = 0; j < 72; ++j) {
            const vec3 d = dir(3.0f + 2.0f * f32(i), 5.0f * f32(j));
            const f32 a = cirrus_opacity(d, pf);
            most = a > most ? a : most;
            CHECK(a <= kCirrusMaxOpacity);
            for (u32 k = 0; k < 3; ++k) covered[k] += cirrus_opacity(d, cirrus_shader_params(on(cov[k]), 0.0f)) > 0.0f;
        }
    CHECK(most > 0.45f);
    CHECK(covered[0] > 0u);
    CHECK(covered[1] > covered[0]);
    CHECK(covered[2] > covered[1]);
    // The horizon fade: the same plane point is fainter seen lower (a higher plane puts it lower in the sky).
    Cirrus near = full;
    near.height_m = 9000.0f;
    const vec3 up = math::normalize(vec3{1.0f, 0.15f, 0.0f});
    const vec3 low = math::normalize(vec3{1.0f, 0.05f, 0.0f});
    Cirrus far = full;
    far.height_m = 3000.0f;  // the same plane xz along `low` (3000 / 0.05 = 9000 / 0.15)
    const f32 a_up = cirrus_opacity(up, cirrus_shader_params(near, 0.0f));
    const f32 a_low = cirrus_opacity(low, cirrus_shader_params(far, 0.0f));
    CHECK(a_up > 0.0f);
    CHECK(a_low < a_up);
    // Density scales it.
    Cirrus thin = full;
    thin.density = 0.5f;
    CHECK(cirrus_opacity(dir(40.0f, 30.0f), cirrus_shader_params(thin, 0.0f)) ==
          doctest::Approx(0.5f * cirrus_opacity(dir(40.0f, 30.0f), pf)));
    Cirrus none = full;
    none.density = 0.0f;
    CHECK(cirrus_opacity(dir(40.0f, 30.0f), cirrus_shader_params(none, 0.0f)) == 0.0f);
}

TEST_CASE("cirrus twin: lighting: brighter toward the sun, lit after the low layer's sunset, warm at dusk") {
    const vec3 light{1.0f, 1.0f, 1.0f}, amb{0.1f, 0.1f, 0.1f};
    const vec3 high_sun = math::normalize(vec3{0.0f, 0.6f, -1.0f});
    // Toward the sun (cs 1) against away from it (cs -1): the side term and the silver.
    const vec3 toward = cirrus_color(high_sun, 1.0f, light, amb), away = cirrus_color(high_sun, -1.0f, light, amb);
    CHECK(toward.x > 2.5f * away.x);
    // The silver is wider than the low layer's lining (pow 24): 20 degrees off the sun it still has a quarter of its
    // peak 0.5 * 1.5 (cos 20 degrees ^ 8 = 0.61, ^ 24 = 0.22).
    const f32 c20 = std::cos(20.0f * 0.017453292f);
    const f32 silver20 = cirrus_color(high_sun, c20, light, amb).x - cirrus_color(high_sun, 0.0f, light, amb).x -
                         0.5f * 0.3f * c20;  // minus the side term's share
    CHECK(silver20 > 0.25f * 0.5f * 1.5f);
    CHECK(silver20 < 0.5f * 1.5f);
    // Day: white (no tint) and no dusk gain.
    const vec3 day = cirrus_color(high_sun, 0.0f, light, amb);
    CHECK(day.x == day.y);
    CHECK(day.y == day.z);
    CHECK(day.x == doctest::Approx(0.1f + 0.5f * 0.7f));
    // Sun 3 degrees under the horizon: the low layer's sun term (smoothstep(-0.1, 0.1, y)) is about 0.2, the cirrus'
    // is lit and warm (more red than blue).
    const vec3 dusk_sun = math::normalize(vec3{0.0f, -0.05f, -1.0f});
    const vec3 dusk = cirrus_color(dusk_sun, 0.9f, light, amb);
    CHECK(dusk.x - amb.x > 0.3f);
    CHECK(dusk.x > dusk.y);
    CHECK(dusk.y > dusk.z);
    // Under -0.16: dark (the ambient only).
    const vec3 night = cirrus_color(math::normalize(vec3{0.0f, -0.3f, -1.0f}), 0.9f, light, amb);
    CHECK(night.x == amb.x);
    CHECK(night.z == amb.z);
    // Composited: mix(c, colour, a) where the opacity is above 0.
    Cirrus full = on(1.0f);
    full.density = 1.0f;
    const CirrusParams p = cirrus_shader_params(full, 0.0f);
    const vec3 d = dir(45.0f, 20.0f);
    const f32 a = cirrus_opacity(d, p);
    REQUIRE(a > 0.0f);
    const vec3 bg{0.2f, 0.4f, 0.8f};
    const vec3 col = cirrus_color(high_sun, d.x * high_sun.x + d.y * high_sun.y + d.z * high_sun.z, light, amb);
    const vec3 out = cirrus_apply(bg, d, high_sun, light, amb, p);
    CHECK(out.x == doctest::Approx(bg.x + (col.x - bg.x) * a));
    CHECK(out.z == doctest::Approx(bg.z + (col.z - bg.z) * a));
}

TEST_CASE("cirrus twin: the fibres fade where a fibre cell spans few pixels") {
    const CirrusParams p = cirrus_shader_params(on(1.0f), 0.0f);  // wind +x: across the wind is z
    // Straight up a fibre cell is 500 m at 9 km: 0.0556 rad, 27.8 pixels at 2 mrad per pixel.
    CHECK(cirrus_fibre_pixels(vec3{0.0f, 1.0f, 0.0f}, p, 0.002f) == doctest::Approx(500.0f / 9000.0f / 0.002f));
    // Toward the horizon it shrinks: with the slant distance (d.y), and more where the view runs across the wind (the
    // cross-wind extent is foreshortened along the view).
    const f32 along_view = cirrus_fibre_pixels(math::normalize(vec3{1.0f, 0.1f, 0.0f}), p, 0.002f);
    const f32 across_view = cirrus_fibre_pixels(math::normalize(vec3{0.0f, 0.1f, 1.0f}), p, 0.002f);
    CHECK(along_view < 3.0f);
    CHECK(across_view < 0.2f * along_view);
    // pix 0 or not finite: full detail.
    CHECK(std::isinf(cirrus_fibre_pixels(vec3{0.0f, 1.0f, 0.0f}, p, 0.0f)));
    CHECK(std::isinf(cirrus_fibre_pixels(vec3{0.0f, 1.0f, 0.0f}, p, kNan)));
    // A coarse footprint takes the fibres out: the opacity is then the broad octave's with s at its mean.
    Cirrus c = on(0.7f);
    c.density = 1.0f;
    const CirrusParams pc = cirrus_shader_params(c, 0.0f);
    const vec3 d = dir(50.0f, 30.0f);
    vec2 q;
    REQUIRE(cirrus_coords(d, pc, q));
    const f32 t = 0.8f - 0.6f * 0.7f;
    const f32 n0 = cirrus_noise(q, 0.0f);
    const f32 x = (n0 - t) / 0.2f;
    const f32 m = x <= 0.0f ? 0.0f : (x >= 1.0f ? 1.0f : x * x * (3.0f - 2.0f * x));
    CHECK(cirrus_opacity(d, pc, 1.0f) == doctest::Approx(m * 0.5f * 1.0f));  // 1 rad per pixel: no fibre detail
}
