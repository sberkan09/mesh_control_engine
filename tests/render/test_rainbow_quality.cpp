// M-RAINBOW 17b: the RainbowSky carrier and how the quality tiers and `r.rainbow` map onto it.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/convar.h"
#include "mce/render/mesh.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

namespace {

ConVar& rainbow_cvar() {
    ConVar* cv = ConVar::find(StringId("r.rainbow"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetRainbowCvar {
    ~ResetRainbowCvar() { rainbow_cvar().reset(); }
};

RainbowSky lit() {
    RainbowSky r;
    r.level = 1.5f;
    r.band_width = 1.5f;
    r.secondary = 0.5f;
    return r;
}

}  // namespace

TEST_CASE("rainbow: the default carrier is off, and so is a fresh Scene3D") {
    const RainbowSky d;
    CHECK(d.level == 0.0f);
    CHECK(d.band_width == 1.0f);
    CHECK(d.secondary == 0.0f);
    CHECK(d.anti_solar.x == 0.0f);
    CHECK(d.anti_solar.y == 0.0f);
    CHECK(d.anti_solar.z == 1.0f);
    CHECK(d.enabled);
    CHECK(d.pad_[0] == 0);
    CHECK(d.pad_[1] == 0);
    CHECK(d.pad_[2] == 0);
    MeshStore store;
    Scene3D scene(store, mem::heap());
    CHECK(scene.rainbow().level == 0.0f);
}

TEST_CASE("rainbow: Scene3D set/get round trip") {
    MeshStore store;
    Scene3D scene(store, mem::heap());
    RainbowSky r;
    r.level = 2.0f;
    r.band_width = 0.75f;
    r.secondary = 0.25f;
    r.anti_solar = math::vec3{1.0f, 0.0f, 0.0f};
    r.enabled = false;
    scene.set_rainbow(r);
    const RainbowSky& g = scene.rainbow();
    CHECK(g.level == 2.0f);
    CHECK(g.band_width == 0.75f);
    CHECK(g.secondary == 0.25f);
    CHECK(g.anti_solar.x == 1.0f);
    CHECK(g.anti_solar.z == 0.0f);
    CHECK_FALSE(g.enabled);
}

TEST_CASE("rainbow: Low zeroes the level; Medium and High keep it") {
    ResetRainbowCvar reset;
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        const bool low = tier == QualityTier::Low;
        const QualitySettings q = quality_settings(tier);
        CHECK(q.rainbow == !low);
        const RainbowSky r = apply_quality(q, lit());
        CHECK(r.level == (low ? 0.0f : 1.5f));
        CHECK(r.band_width == 1.5f);
        CHECK(r.secondary == 0.5f);
        CHECK(r.enabled);
    }
}

TEST_CASE("rainbow: r.rainbow 0 zeroes the level on every tier") {
    ResetRainbowCvar reset;
    rainbow_cvar().set_string("0");
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        CHECK(apply_quality(quality_settings(tier), lit()).level == 0.0f);
    }
}

TEST_CASE("rainbow: a clear input stays off; enabled passes through") {
    ResetRainbowCvar reset;
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High})
        CHECK(apply_quality(quality_settings(tier), RainbowSky{}).level == 0.0f);
    RainbowSky r = lit();
    r.enabled = false;
    CHECK_FALSE(apply_quality(quality_settings(QualityTier::High), r).enabled);
}

TEST_CASE("rainbow: apply_quality clamps every field and sanitizes NaN") {
    ResetRainbowCvar reset;
    const QualitySettings q = quality_settings(QualityTier::High);
    RainbowSky r;
    r.level = 100.0f;
    r.band_width = 9.0f;
    r.secondary = 5.0f;
    RainbowSky out = apply_quality(q, r);
    CHECK(out.level == 4.0f);
    CHECK(out.band_width == 2.0f);
    CHECK(out.secondary == 1.0f);

    r.level = -1.0f;
    r.band_width = 0.0f;
    r.secondary = -2.0f;
    out = apply_quality(q, r);
    CHECK(out.level == 0.0f);
    CHECK(out.band_width == 0.5f);
    CHECK(out.secondary == 0.0f);

    const f32 nan = std::nanf("");
    r = RainbowSky{};
    r.level = nan;
    r.band_width = nan;
    r.secondary = nan;
    r.anti_solar = math::vec3{nan, nan, nan};
    out = apply_quality(q, r);
    CHECK(out.level == 0.0f);
    CHECK(out.band_width == 1.0f);
    CHECK(out.secondary == 0.0f);
    CHECK(out.anti_solar.x == 0.0f);
    CHECK(out.anti_solar.y == 0.0f);
    CHECK(out.anti_solar.z == 1.0f);

    out = apply_quality(q, lit());
    CHECK(out.level == 1.5f);  // in range: unchanged
}

TEST_CASE("rainbow: anti_solar is normalized, a zero or infinite vector falls back to (0, 0, 1)") {
    ResetRainbowCvar reset;
    const QualitySettings q = quality_settings(QualityTier::High);
    RainbowSky r = lit();
    r.anti_solar = math::vec3{0.0f, 5.0f, 0.0f};
    RainbowSky out = apply_quality(q, r);
    CHECK(out.anti_solar.x == doctest::Approx(0.0f));
    CHECK(out.anti_solar.y == doctest::Approx(1.0f));
    CHECK(out.anti_solar.z == doctest::Approx(0.0f));
    r.anti_solar = math::vec3{3.0f, 0.0f, 4.0f};
    out = apply_quality(q, r);
    CHECK(out.anti_solar.x == doctest::Approx(0.6f));
    CHECK(out.anti_solar.z == doctest::Approx(0.8f));
    for (const math::vec3 bad : {math::vec3{0.0f, 0.0f, 0.0f}, math::vec3{INFINITY, 0.0f, 0.0f}}) {
        r.anti_solar = bad;
        out = apply_quality(q, r);
        CHECK(out.anti_solar.x == 0.0f);
        CHECK(out.anti_solar.y == 0.0f);
        CHECK(out.anti_solar.z == 1.0f);
    }
}
