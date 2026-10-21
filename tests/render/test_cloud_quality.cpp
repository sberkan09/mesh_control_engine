// M-CLOUDS 9a: the Clouds carrier and how the quality tiers and `r.clouds` map onto it.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/convar.h"
#include "mce/render/mesh.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

namespace {

ConVar& cloud_cvar() {
    ConVar* cv = ConVar::find(StringId("r.clouds"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetCloudCvar {
    ~ResetCloudCvar() { cloud_cvar().reset(); }
};

Clouds cloudy() {
    Clouds c;
    c.coverage = 0.8f;
    return c;
}

}  // namespace

TEST_CASE("clouds: the default carrier is off, and so is a fresh Scene3D") {
    const Clouds d;
    CHECK(d.coverage == 0.0f);
    CHECK(d.density == 0.7f);
    CHECK(d.scale_m == 900.0f);
    CHECK(d.speed == 1.0f);
    CHECK(d.wind_dir.x == 1.0f);
    CHECK(d.wind_dir.y == 0.0f);
    CHECK(d.height_m == 2000.0f);
    CHECK(d.enabled);
    MeshStore store;
    Scene3D scene(store, mem::heap());
    CHECK(scene.clouds().coverage == 0.0f);
}

TEST_CASE("clouds: Scene3D set/get round trip") {
    MeshStore store;
    Scene3D scene(store, mem::heap());
    Clouds c;
    c.coverage = 0.6f;
    c.density = 0.4f;
    c.scale_m = 1500.0f;
    c.speed = 2.5f;
    c.wind_dir = math::vec2{0.0f, -1.0f};
    c.height_m = 3000.0f;
    c.enabled = false;
    scene.set_clouds(c);
    const Clouds& g = scene.clouds();
    CHECK(g.coverage == 0.6f);
    CHECK(g.density == 0.4f);
    CHECK(g.scale_m == 1500.0f);
    CHECK(g.speed == 2.5f);
    CHECK(g.wind_dir.x == 0.0f);
    CHECK(g.wind_dir.y == -1.0f);
    CHECK(g.height_m == 3000.0f);
    CHECK_FALSE(g.enabled);
}

TEST_CASE("clouds: Low drops them, Medium and High keep them") {
    ResetCloudCvar reset;
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        const QualitySettings q = quality_settings(tier);
        CHECK(q.clouds == (tier != QualityTier::Low));
        const Clouds c = apply_quality(q, cloudy());
        CHECK(c.coverage == (tier != QualityTier::Low ? 0.8f : 0.0f));
        CHECK(c.density == 0.7f);
        CHECK(c.scale_m == 900.0f);
        CHECK(c.speed == 1.0f);
        CHECK(c.height_m == 2000.0f);
        CHECK(c.enabled);
    }
}

TEST_CASE("clouds: r.clouds 0 zeroes the coverage on every tier") {
    ResetCloudCvar reset;
    cloud_cvar().set_string("0");
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        CHECK(apply_quality(quality_settings(tier), cloudy()).coverage == 0.0f);
    }
}

TEST_CASE("clouds: a tier that disables them zeroes the coverage; a clear input stays clear; enabled passes through") {
    ResetCloudCvar reset;
    QualitySettings q = quality_settings(QualityTier::High);
    q.clouds = false;
    CHECK(apply_quality(q, cloudy()).coverage == 0.0f);
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High})
        CHECK(apply_quality(quality_settings(tier), Clouds{}).coverage == 0.0f);
    Clouds c = cloudy();
    c.enabled = false;
    CHECK_FALSE(apply_quality(quality_settings(QualityTier::High), c).enabled);
}

TEST_CASE("clouds: apply_quality clamps every field and sanitizes NaN") {
    ResetCloudCvar reset;
    const QualitySettings q = quality_settings(QualityTier::High);
    Clouds c;
    c.coverage = 1.5f;
    c.density = -0.25f;
    c.scale_m = 1.0f;
    c.speed = -3.0f;
    c.height_m = 1.0e9f;
    Clouds out = apply_quality(q, c);
    CHECK(out.coverage == 1.0f);
    CHECK(out.density == 0.0f);
    CHECK(out.scale_m == 50.0f);
    CHECK(out.speed == 0.0f);
    CHECK(out.height_m == 20000.0f);

    c.scale_m = 1.0e9f;
    c.speed = 1.0e6f;
    c.height_m = 0.0f;
    out = apply_quality(q, c);
    CHECK(out.scale_m == 20000.0f);
    CHECK(out.speed == 10.0f);
    CHECK(out.height_m == 100.0f);

    const f32 nan = std::nanf("");
    c.coverage = nan;
    c.density = nan;
    c.scale_m = nan;
    c.speed = nan;
    c.height_m = nan;
    c.wind_dir = math::vec2{nan, nan};
    out = apply_quality(q, c);
    CHECK(out.coverage == 0.0f);
    CHECK(out.density == 0.0f);
    CHECK(out.scale_m == 900.0f);
    CHECK(out.speed == 1.0f);
    CHECK(out.height_m == 2000.0f);
    CHECK(out.wind_dir.x == 1.0f);
    CHECK(out.wind_dir.y == 0.0f);

    c = cloudy();
    c.speed = 4.0f;
    out = apply_quality(q, c);
    CHECK(out.coverage == 0.8f);  // in range: unchanged
    CHECK(out.speed == 4.0f);
}

TEST_CASE("clouds: wind_dir is normalized, a zero vector falls back to +x") {
    ResetCloudCvar reset;
    const QualitySettings q = quality_settings(QualityTier::High);
    Clouds c = cloudy();
    c.wind_dir = math::vec2{0.0f, 5.0f};
    Clouds out = apply_quality(q, c);
    CHECK(out.wind_dir.x == doctest::Approx(0.0f));
    CHECK(out.wind_dir.y == doctest::Approx(1.0f));
    c.wind_dir = math::vec2{0.0f, 0.0f};
    out = apply_quality(q, c);
    CHECK(out.wind_dir.x == 1.0f);
    CHECK(out.wind_dir.y == 0.0f);
}
