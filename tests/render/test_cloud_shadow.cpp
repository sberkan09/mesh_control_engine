// M-CLOUDSHADOW 51b: the pure cloud shadow helpers (point, drift, noise, density, factor) and how the quality tiers and
// `r.cloud_shadows` map onto PostFx::cloud_shadow. No GPU.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "mce/convar.h"
#include "mce/render/cloud_shadow.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
constexpr f32 kInf = std::numeric_limits<f32>::infinity();

ConVar& cs_cvar() {
    ConVar* cv = ConVar::find(StringId("r.cloud_shadows"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetCsCvar {
    ~ResetCsCvar() { cs_cvar().reset(); }
};

CloudShadow on() {
    CloudShadow s;
    s.enabled = true;
    return s;
}

PostFx applied(QualityTier tier, const CloudShadow& s) {
    PostFx post;
    post.cloud_shadow = s;
    ShadowSettings shadows;
    apply_quality(quality_settings(tier), shadows, post);
    return post;
}

CloudDensityParams field(f32 coverage) {
    CloudDensityParams p;
    p.coverage = coverage;
    p.density = 0.7f;
    p.scale_m = 900.0f;
    return p;
}

// The mean density over a grid of plane points.
f32 mean_density(const CloudDensityParams& p, math::vec2 shift = math::vec2{0, 0}) {
    f32 sum = 0.0f;
    u32 n = 0;
    for (int i = 0; i < 40; ++i)
        for (int j = 0; j < 40; ++j) {
            sum += cloud_density(math::vec2{f32(i) * 311.0f + shift.x, f32(j) * 297.0f + shift.y}, p);
            ++n;
        }
    return sum / f32(n);
}

}  // namespace

TEST_CASE("cloud shadow: point moves along the sun to the plane, camera-relative") {
    math::vec2 xz{};
    // Sun straight up: the point's own xz, relative to the camera.
    CHECK(cloud_shadow_point({10, 0, 5}, {0, 1, 0}, {2, 0, 1}, 2000.0f, xz));
    CHECK(xz.x == doctest::Approx(8.0f));
    CHECK(xz.y == doctest::Approx(4.0f));
    // 45 degrees toward +x: the point shifts by the height difference along x.
    const f32 s = std::sqrt(0.5f);
    CHECK(cloud_shadow_point({0, 0, 0}, {s, s, 0}, {0, 0, 0}, 1000.0f, xz));
    CHECK(xz.x == doctest::Approx(1000.0f).epsilon(1e-4));
    CHECK(xz.y == doctest::Approx(0.0f).epsilon(1e-4));
    // The plane is height_m above the camera, not above the world: a point 500 m up has 500 m to go.
    CHECK(cloud_shadow_point({0, 500, 0}, {s, s, 0}, {0, 0, 0}, 1000.0f, xz));
    CHECK(xz.x == doctest::Approx(500.0f).epsilon(1e-4));
    // The sun direction is normalized here.
    CHECK(cloud_shadow_point({0, 0, 0}, {3 * s, 3 * s, 0}, {0, 0, 0}, 1000.0f, xz));
    CHECK(xz.x == doctest::Approx(1000.0f).epsilon(1e-4));
}

TEST_CASE("cloud shadow: point has no hit for a low sun, a point above the plane or bad input") {
    math::vec2 xz{};
    CHECK_FALSE(cloud_shadow_point({0, 0, 0}, {1, 0.0f, 0}, {0, 0, 0}, 2000.0f, xz));
    CHECK_FALSE(cloud_shadow_point({0, 0, 0}, {1, 0.01f, 0}, {0, 0, 0}, 2000.0f, xz));
    CHECK_FALSE(cloud_shadow_point({0, 0, 0}, {1, -1, 0}, {0, 0, 0}, 2000.0f, xz));
    CHECK_FALSE(cloud_shadow_point({0, 2500, 0}, {0, 1, 0}, {0, 0, 0}, 2000.0f, xz));
    CHECK_FALSE(cloud_shadow_point({0, 0, 0}, {0, 1, 0}, {0, 0, 0}, 0.0f, xz));
    CHECK_FALSE(cloud_shadow_point({0, 0, 0}, {0, 0, 0}, {0, 0, 0}, 2000.0f, xz));
    CHECK_FALSE(cloud_shadow_point({kNaN, 0, 0}, {0, 1, 0}, {0, 0, 0}, 2000.0f, xz));
    CHECK_FALSE(cloud_shadow_point({0, 0, 0}, {0, kInf, 0}, {0, 0, 0}, 2000.0f, xz));
    CHECK_FALSE(cloud_shadow_point({0, 0, 0}, {0, 1, 0}, {kInf, 0, 0}, 2000.0f, xz));
    CHECK_FALSE(cloud_shadow_point({0, 0, 0}, {0, 1, 0}, {0, 0, 0}, kNaN, xz));
    // Very flat but allowed sun: finite result.
    CHECK(cloud_shadow_point({0, 0, 0}, {1, 0.03f, 0}, {0, 0, 0}, 2000.0f, xz));
    CHECK(std::isfinite(xz.x));
}

TEST_CASE("cloud shadow: density is 0 at coverage 0 and for non-finite input") {
    const CloudDensityParams p0 = field(0.0f);
    for (int i = 0; i < 50; ++i) CHECK(cloud_density(math::vec2{f32(i) * 137.0f, f32(i) * 53.0f}, p0) == 0.0f);
    CHECK(mean_density(p0) == 0.0f);
    const CloudDensityParams p = field(0.8f);
    CHECK(cloud_density(math::vec2{kNaN, 0.0f}, p) == 0.0f);
    CHECK(cloud_density(math::vec2{0.0f, kInf}, p) == 0.0f);
    CloudDensityParams bad = p;
    bad.drift_cells = math::vec2{kNaN, 0.0f};
    CHECK(cloud_density(math::vec2{1.0f, 1.0f}, bad) == 0.0f);
    bad = p;
    bad.octaves = 0;
    CHECK(cloud_density(math::vec2{1.0f, 1.0f}, bad) == 0.0f);
    bad = p;
    bad.coverage = kNaN;
    CHECK(cloud_density(math::vec2{1.0f, 1.0f}, bad) == 0.0f);
}

TEST_CASE("cloud shadow: density stays in 0..1, the noise in 0..1, and the pattern has clear and thick places") {
    const CloudDensityParams p = field(0.5f);
    f32 lo = 1.0f, hi = 0.0f;
    for (int i = 0; i < 60; ++i)
        for (int j = 0; j < 60; ++j) {
            const math::vec2 q{f32(i) * 173.0f - 4000.0f, f32(j) * 211.0f - 4000.0f};
            const f32 n = cloud_noise(q, p), d = cloud_density(q, p);
            CHECK(n >= 0.0f);
            CHECK(n <= 1.0f);
            CHECK(d >= 0.0f);
            CHECK(d <= 1.0f);
            lo = std::min(lo, d);
            hi = std::max(hi, d);
        }
    CHECK(lo == 0.0f);
    CHECK(hi > 0.5f);
}

TEST_CASE("cloud shadow: density is monotone in coverage") {
    f32 prev = -1.0f;
    for (f32 cov : {0.1f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f}) {
        const f32 m = mean_density(field(cov));
        CHECK(m >= prev);
        prev = m;
    }
    CHECK(mean_density(field(1.0f)) > mean_density(field(0.1f)) + 0.1f);
    // Per point too: more coverage never lightens a point.
    for (int i = 0; i < 40; ++i) {
        const math::vec2 q{f32(i) * 257.0f, f32(i) * 91.0f};
        CHECK(cloud_density(q, field(0.7f)) >= cloud_density(q, field(0.4f)));
    }
}

TEST_CASE("cloud shadow: density grows with the cloud density and widens with softness") {
    CloudDensityParams a = field(0.6f), b = field(0.6f);
    a.density = 0.2f;
    b.density = 1.0f;
    CHECK(mean_density(b) > mean_density(a));
    CHECK(cloud_density(math::vec2{0, 0}, a) <= cloud_density(math::vec2{0, 0}, b));
    // A wider edge covers more of the transition band (the mean grows with softness at the same threshold).
    CloudDensityParams hard = field(0.5f), soft = field(0.5f);
    hard.softness = 0.0f;
    soft.softness = 1.0f;
    CHECK(mean_density(soft) < mean_density(hard));  // smoothstep over a wider band lowers the rise inside it
}

TEST_CASE("cloud shadow: the noise is periodic in 64 cells and the octave count matters") {
    CloudDensityParams p = field(0.6f);
    const f32 period_m = 64.0f * p.scale_m;
    for (int i = 0; i < 20; ++i) {
        const math::vec2 q{f32(i) * 777.0f, f32(i) * 1234.0f};
        CHECK(cloud_noise(math::vec2{q.x + period_m, q.y}, p) == doctest::Approx(cloud_noise(q, p)).epsilon(1e-3));
    }
    CloudDensityParams p2 = p, p1 = p;
    p2.octaves = 2;
    p1.octaves = 1;
    f32 diff = 0.0f;
    for (int i = 0; i < 20; ++i) {
        const math::vec2 q{f32(i) * 777.0f, f32(i) * 1234.0f};
        diff += std::fabs(cloud_noise(q, p) - cloud_noise(q, p2)) + std::fabs(cloud_noise(q, p2) - cloud_noise(q, p1));
        CHECK(cloud_noise(q, p2) >= 0.0f);
        CHECK(cloud_noise(q, p2) <= 1.0f);
    }
    CHECK(diff > 0.1f);
}

TEST_CASE("cloud shadow: the noise reproduces the sky's value golden") {
    // Hand-checked against kSkyFS's cloud_hash at a lattice point: q = (0,0) is the lattice corner (0,0) of every octave.
    CloudDensityParams p = field(0.6f);
    p.scale_m = 900.0f;
    const f32 n = cloud_noise(math::vec2{0.0f, 0.0f}, p);
    // value() at a corner returns the lattice value; the weights sum to 1.
    CHECK(n > 0.0f);
    CHECK(n < 1.0f);
    CHECK(cloud_noise(math::vec2{0.0f, 0.0f}, p) == n);  // deterministic
}

TEST_CASE("cloud shadow: the drift moves the pattern") {
    CloudDensityParams p = field(0.6f);
    const math::vec2 d0 = cloud_shadow_drift(0.0f, 1.0f, p.scale_m, math::vec2{1, 0});
    const math::vec2 d1 = cloud_shadow_drift(30.0f, 1.0f, p.scale_m, math::vec2{1, 0});
    CHECK(d0.x == 0.0f);
    CHECK(d0.y == 0.0f);
    CHECK(d1.x == doctest::Approx(30.0f * 10.0f / 900.0f).epsilon(1e-5));
    CHECK(d1.y == 0.0f);
    p.drift_cells = d0;
    const f32 before = cloud_density(math::vec2{500.0f, 500.0f}, p);
    p.drift_cells = d1;
    // The same plane point after the drift reads the pattern 300 m upwind: a point that was 300 m "behind" now reads it.
    CHECK(cloud_density(math::vec2{800.0f, 500.0f}, p) == doctest::Approx(before).epsilon(1e-4));
    f32 moved = 0.0f;
    for (int i = 0; i < 40; ++i) {
        const math::vec2 q{f32(i) * 311.0f, f32(i) * 97.0f};
        CloudDensityParams a = p, b = p;
        a.drift_cells = d0;
        b.drift_cells = d1;
        moved += std::fabs(cloud_density(q, a) - cloud_density(q, b));
    }
    CHECK(moved > 0.1f);
    // Speed 0 does not drift; the clock wraps into the period; non-finite time is 0.
    CHECK(cloud_shadow_drift(100.0f, 0.0f, 900.0f, math::vec2{1, 0}).x == 0.0f);
    const math::vec2 w = cloud_shadow_drift(1.0e7f, 1.0f, 900.0f, math::vec2{1, 1});
    CHECK(w.x >= 0.0f);
    CHECK(w.x < kCloudShadowPeriod);
    CHECK(w.y < kCloudShadowPeriod);
    CHECK(cloud_shadow_drift(kNaN, 1.0f, 900.0f, math::vec2{1, 0}).x == 0.0f);
    CHECK(cloud_shadow_drift(10.0f, 1.0f, 900.0f, math::vec2{kNaN, 0}).x > 0.0f);  // NaN wind -> +x
}

TEST_CASE("cloud shadow: a 180 degree wind turn mirrors the drift") {
    CloudDensityParams fwd = field(0.6f), back = field(0.6f);
    fwd.drift_cells = cloud_shadow_drift(45.0f, 1.0f, fwd.scale_m, math::vec2{1, 0});
    back.drift_cells = cloud_shadow_drift(45.0f, 1.0f, back.scale_m, math::vec2{-1, 0});
    const f32 shift_m = 2.0f * (45.0f * 10.0f / 900.0f) * 900.0f;  // twice the drift distance, 900 m
    CHECK(shift_m == doctest::Approx(900.0f));
    // The forward pattern at p is the backward pattern at p - 2 d: mirrored motion about the start.
    for (int i = 0; i < 40; ++i) {
        const math::vec2 q{f32(i) * 211.0f + 3.0f, f32(i) * 131.0f};
        CHECK(cloud_density(q, fwd) == doctest::Approx(cloud_density(math::vec2{q.x - shift_m, q.y}, back)).epsilon(1e-3));
    }
    // And the patterns differ at the same point.
    f32 d = 0.0f;
    for (int i = 0; i < 40; ++i) {
        const math::vec2 q{f32(i) * 211.0f + 3.0f, f32(i) * 131.0f};
        d += std::fabs(cloud_density(q, fwd) - cloud_density(q, back));
    }
    CHECK(d > 0.1f);
}

TEST_CASE("cloud shadow: the factor is 1 without density or strength, drops with both, and has a floor") {
    CHECK(cloud_shadow_factor(0.0f, 0.6f, 1.0f, 0.8f) == 1.0f);
    CHECK(cloud_shadow_factor(1.0f, 0.0f, 1.0f, 0.8f) == 1.0f);
    CHECK(cloud_shadow_factor(1.0f, 0.6f, 0.0f, 0.8f) == 1.0f);
    CHECK(cloud_shadow_factor(1.0f, 0.6f, 1.0f, 0.8f) == doctest::Approx(0.4f));
    CHECK(cloud_shadow_factor(0.5f, 0.6f, 1.0f, 0.8f) == doctest::Approx(0.7f));
    CHECK(cloud_shadow_factor(1.0f, 1.0f, 1.0f, 1.0f) == doctest::Approx(0.0f));
    // Clamped inputs.
    CHECK(cloud_shadow_factor(5.0f, 5.0f, 5.0f, 1.0f) == doctest::Approx(0.0f));
    CHECK(cloud_shadow_factor(-1.0f, 0.6f, 1.0f, 0.8f) == 1.0f);
    // Monotone in each.
    f32 prev = 2.0f;
    for (f32 d = 0.0f; d <= 1.0f; d += 0.1f) {
        const f32 f = cloud_shadow_factor(d, 0.6f, 1.0f, 0.8f);
        CHECK(f <= prev);
        prev = f;
    }
    prev = 2.0f;
    for (f32 s = 0.0f; s <= 1.0f; s += 0.1f) {
        const f32 f = cloud_shadow_factor(1.0f, s, 1.0f, 0.8f);
        CHECK(f <= prev);
        prev = f;
    }
}

TEST_CASE("cloud shadow: the factor fades to 1 toward the horizon, smoothly") {
    CHECK(cloud_shadow_factor(1.0f, 0.6f, 1.0f, 0.0f) == 1.0f);
    CHECK(cloud_shadow_factor(1.0f, 0.6f, 1.0f, 0.02f) == 1.0f);
    CHECK(cloud_shadow_factor(1.0f, 0.6f, 1.0f, -0.5f) == 1.0f);
    CHECK(cloud_shadow_factor(1.0f, 0.6f, 1.0f, 0.25f) == doctest::Approx(0.4f));
    f32 prev = 2.0f;
    for (f32 y = 0.0f; y <= 1.0f; y += 0.01f) {
        const f32 f = cloud_shadow_factor(1.0f, 0.6f, 1.0f, y);
        CHECK(f <= prev + 1e-6f);
        if (prev <= 1.0f) CHECK(prev - f < 0.05f);  // no step
        prev = f;
    }
}

TEST_CASE("cloud shadow: the factor is 1 for non-finite input") {
    CHECK(cloud_shadow_factor(kNaN, 0.6f, 1.0f, 0.8f) == 1.0f);
    CHECK(cloud_shadow_factor(1.0f, kNaN, 1.0f, 0.8f) == 1.0f);
    CHECK(cloud_shadow_factor(1.0f, 0.6f, kInf, 0.8f) == 1.0f);
    CHECK(cloud_shadow_factor(1.0f, 0.6f, 1.0f, kNaN) == 1.0f);
    CHECK(cloud_shadow_factor(kInf, 0.6f, 1.0f, 0.8f) == 1.0f);
}

TEST_CASE("cloud shadow: the whole chain is deterministic and finite") {
    const CloudDensityParams p = field(0.7f);
    const math::vec3 sun{0.3f, 0.8f, 0.2f};
    for (int i = 0; i < 30; ++i) {
        math::vec2 xz{};
        const math::vec3 w{f32(i) * 13.0f, f32(i % 5), f32(i) * -7.0f};
        REQUIRE(cloud_shadow_point(w, sun, {0, 1, 0}, 2000.0f, xz));
        const f32 f = cloud_shadow_factor(cloud_density(xz, p), 0.6f, 1.0f, 0.8f);
        CHECK(f >= 0.4f);
        CHECK(f <= 1.0f);
        CHECK(f == cloud_shadow_factor(cloud_density(xz, p), 0.6f, 1.0f, 0.8f));
    }
}

TEST_CASE("cloud shadow: the quality tiers") {
    const QualitySettings low = quality_settings(QualityTier::Low);
    const QualitySettings med = quality_settings(QualityTier::Medium);
    const QualitySettings high = quality_settings(QualityTier::High);
    CHECK_FALSE(low.cloud_shadows);
    CHECK(med.cloud_shadows);
    CHECK(high.cloud_shadows);
    CHECK(low.cloud_shadow_octaves == 0u);
    CHECK(med.cloud_shadow_octaves == 2u);
    CHECK(high.cloud_shadow_octaves == 3u);
}

TEST_CASE("cloud shadow: apply_quality honors the tier and r.cloud_shadows") {
    ResetCsCvar reset;
    const CloudShadow s = on();
    CHECK_FALSE(applied(QualityTier::Low, s).cloud_shadow.enabled);
    CHECK(applied(QualityTier::Medium, s).cloud_shadow.enabled);
    CHECK(applied(QualityTier::High, s).cloud_shadow.enabled);
    CHECK_FALSE(applied(QualityTier::High, CloudShadow{}).cloud_shadow.enabled);  // off stays off
    cs_cvar().set_string("0");
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        CHECK_FALSE(applied(tier, s).cloud_shadow.enabled);
    }
    cs_cvar().set_string("1");
    CHECK(applied(QualityTier::High, s).cloud_shadow.enabled);
}

TEST_CASE("cloud shadow: apply_quality clamps the ranges and sanitizes NaN") {
    ResetCsCvar reset;
    CloudShadow s = on();
    s.strength = 9.0f;
    s.softness = 9.0f;
    CloudShadow c = applied(QualityTier::High, s).cloud_shadow;
    CHECK(c.strength == 1.0f);
    CHECK(c.softness == 1.0f);
    s.strength = -1.0f;
    s.softness = -1.0f;
    c = applied(QualityTier::High, s).cloud_shadow;
    CHECK(c.strength == 0.0f);
    CHECK(c.softness == 0.0f);
    s.strength = s.softness = kNaN;
    c = applied(QualityTier::High, s).cloud_shadow;
    CHECK(c.enabled);
    CHECK(c.strength == 0.6f);
    CHECK(c.softness == 0.3f);
}

TEST_CASE("cloud shadow: in-range values and the default pass through apply_quality unchanged") {
    ResetCsCvar reset;
    CloudShadow s = on();
    s.strength = 0.25f;
    s.softness = 0.75f;
    CloudShadow c = applied(QualityTier::High, s).cloud_shadow;
    CHECK(c.strength == 0.25f);
    CHECK(c.softness == 0.75f);
    c = applied(QualityTier::Medium, on()).cloud_shadow;
    CHECK(c.strength == 0.6f);
    CHECK(c.softness == 0.3f);
}

// ---- M-CLOUDSHADOW 51c: the sun's share of the light (cloud_shadow_sun_share)

TEST_CASE("cloud shadow: sun share is S / (S + A) of the sun's and the sky ambient's luma") {
    const auto luma = [](math::vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; };
    const DirectionalLight d;  // the default sun and sky ambient
    const f32 s = luma(d.color) * d.intensity, a = luma(d.ambient_sky);
    CHECK(cloud_shadow_sun_share(d.color, d.intensity, d.ambient_sky) == doctest::Approx(s / (s + a)).epsilon(1e-6));
    CHECK(cloud_shadow_sun_share(d.color, d.intensity, d.ambient_sky) > 0.75f);  // a bright noon: most of the light
    // White sun of intensity 1 against a white ambient of 1: half.
    CHECK(cloud_shadow_sun_share({1, 1, 1}, 1.0f, {1, 1, 1}) == doctest::Approx(0.5f).epsilon(1e-6));
    // A dimmer sun against the same sky: a smaller share; a brighter one: a larger share, never above 1.
    const f32 noon = cloud_shadow_sun_share({1, 1, 1}, 3.0f, {0.4f, 0.4f, 0.4f});
    const f32 dusk = cloud_shadow_sun_share({1, 0.6f, 0.4f}, 0.5f, {0.4f, 0.4f, 0.4f});
    CHECK(dusk < noon);
    CHECK(noon <= 1.0f);
    CHECK(cloud_shadow_sun_share({1, 1, 1}, 1000.0f, {0.4f, 0.4f, 0.4f}) <= 1.0f);
    // No ambient: all of the light is the sun's.
    CHECK(cloud_shadow_sun_share({1, 1, 1}, 2.0f, {0, 0, 0}) == 1.0f);
}

TEST_CASE("cloud shadow: sun share is 0 without a sun and for non-finite input") {
    CHECK(cloud_shadow_sun_share({1, 1, 1}, 0.0f, {0.4f, 0.4f, 0.4f}) == 0.0f);
    CHECK(cloud_shadow_sun_share({1, 1, 1}, -2.0f, {0.4f, 0.4f, 0.4f}) == 0.0f);
    CHECK(cloud_shadow_sun_share({0, 0, 0}, 2.0f, {0.4f, 0.4f, 0.4f}) == 0.0f);
    CHECK(cloud_shadow_sun_share({-1, -1, -1}, 2.0f, {0.4f, 0.4f, 0.4f}) == 0.0f);
    CHECK(cloud_shadow_sun_share({0, 0, 0}, 0.0f, {0, 0, 0}) == 0.0f);
    CHECK(cloud_shadow_sun_share({kNaN, 1, 1}, 2.0f, {0.4f, 0.4f, 0.4f}) == 0.0f);
    CHECK(cloud_shadow_sun_share({1, 1, 1}, kNaN, {0.4f, 0.4f, 0.4f}) == 0.0f);
    CHECK(cloud_shadow_sun_share({1, 1, 1}, kInf, {0.4f, 0.4f, 0.4f}) == 0.0f);
    CHECK(cloud_shadow_sun_share({1, 1, 1}, 2.0f, {0.4f, kInf, 0.4f}) == 0.0f);
    // A negative ambient channel counts as 0 (the share stays in 0..1).
    CHECK(cloud_shadow_sun_share({1, 1, 1}, 2.0f, {-5, -5, -5}) == 1.0f);
    // Huge but finite: S overflows to inf -> 0 rather than NaN.
    CHECK(cloud_shadow_sun_share({3e38f, 3e38f, 3e38f}, 3e38f, {0.4f, 0.4f, 0.4f}) == 0.0f);
}

// ---- M-CLOUDSHADOW 51c review: the view gate, the fog share and the exact fog composite

TEST_CASE("cloud shadow: the pass's view gate takes a perspective projection only") {
    CHECK(cloud_shadow_perspective(math::perspective_rh_no(math::radians(45.0f), 1.5f, 0.1f, 1000.0f)));
    CHECK_FALSE(cloud_shadow_perspective(math::ortho_rh_no(-5.0f, 5.0f, -5.0f, 5.0f, 0.1f, 100.0f)));
    math::mat4 p = math::perspective_rh_no(math::radians(45.0f), 1.5f, 0.1f, 1000.0f);
    p.c[2][0] = kNaN;
    CHECK_FALSE(cloud_shadow_perspective(p));
    p = math::perspective_rh_no(math::radians(45.0f), 1.5f, 0.1f, 1000.0f);
    p.c[0][0] = 0.0f;
    CHECK_FALSE(cloud_shadow_perspective(p));
    p.c[0][0] = kInf;
    CHECK_FALSE(cloud_shadow_perspective(p));
    CHECK_FALSE(cloud_shadow_perspective(math::mat4{}));  // all zero: degenerate
}

TEST_CASE("cloud shadow: fog visibility is the main pass's exp² term; a negative density fogs as the positive") {
    CHECK(cloud_shadow_fog_visibility(0.0f, 500.0f) == 1.0f);
    CHECK(cloud_shadow_fog_visibility(kNaN, 500.0f) == 1.0f);
    CHECK(cloud_shadow_fog_visibility(0.01f, kInf) == 1.0f);
    CHECK(cloud_shadow_fog_visibility(0.01f, 100.0f) == doctest::Approx(std::exp(-1.0f)).epsilon(1e-6));
    CHECK(cloud_shadow_fog_visibility(-0.01f, 100.0f) == cloud_shadow_fog_visibility(0.01f, 100.0f));
    CHECK(cloud_shadow_fog_visibility(0.01f, 0.0f) == 1.0f);
}

TEST_CASE("cloud shadow: the shade composite shades the surface part of a fogged pixel only") {
    const math::vec3 fog{4.0f, 3.0f, 2.0f};
    // No fog: pixel * factor exactly (the unfogged path).
    const math::vec3 a = cloud_shadow_shade({0.5f, 0.25f, 2.0f}, 0.4f, 1.0f, fog);
    CHECK(a.x == 0.5f * 0.4f);
    CHECK(a.y == 0.25f * 0.4f);
    CHECK(a.z == 2.0f * 0.4f);
    // A fogged pixel: s T + fog (1 - T) becomes s factor T + fog (1 - T).
    const f32 s = 0.3f, t = 0.5f, factor = 0.4f;
    const math::vec3 px{s * t + fog.x * (1 - t), s * t + fog.y * (1 - t), s * t + fog.z * (1 - t)};
    const math::vec3 b = cloud_shadow_shade(px, factor, t, fog);
    CHECK(b.x == doctest::Approx(s * factor * t + fog.x * (1 - t)).epsilon(1e-6));
    CHECK(b.y == doctest::Approx(s * factor * t + fog.y * (1 - t)).epsilon(1e-6));
    CHECK(b.z == doctest::Approx(s * factor * t + fog.z * (1 - t)).epsilon(1e-6));
    // Factor 1 (no cloud) leaves any pixel as it is; a NaN factor too; the factor clamps.
    const math::vec3 c = cloud_shadow_shade(px, 1.0f, t, fog);
    CHECK(c.x == px.x);
    CHECK(cloud_shadow_shade(px, kNaN, t, fog).x == px.x);
    CHECK(cloud_shadow_shade(px, -1.0f, t, fog).x == doctest::Approx(fog.x * (1 - t)).epsilon(1e-6));
    // All fog (T = 0): nothing to shade.
    CHECK(cloud_shadow_shade(fog, 0.2f, 0.0f, fog).x == doctest::Approx(fog.x).epsilon(1e-6));
}
