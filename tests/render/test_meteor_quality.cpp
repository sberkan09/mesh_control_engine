// M-METEOR 18b: the MeteorSky carrier and how the quality tiers and `r.meteor` map onto it.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/convar.h"
#include "mce/render/mesh.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

namespace {

ConVar& meteor_cvar() {
    ConVar* cv = ConVar::find(StringId("r.meteor"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetMeteorCvar {
    ~ResetMeteorCvar() { meteor_cvar().reset(); }
};

MeteorSky lit() {
    MeteorSky m;
    m.level = 1.5f;
    m.rate = 12.0f;
    m.brightness = 2.0f;
    m.length = 1.5f;
    m.speed = 1.25f;
    m.time = 17.5f;
    m.seed = 42;
    return m;
}

}  // namespace

TEST_CASE("meteor: the default carrier is off, and so is a fresh Scene3D") {
    const MeteorSky d;
    CHECK(d.level == 0.0f);
    CHECK(d.rate == 4.0f);
    CHECK(d.brightness == 1.0f);
    CHECK(d.length == 1.0f);
    CHECK(d.speed == 1.0f);
    CHECK(d.time == 0.0f);
    CHECK(d.seed == 0u);
    CHECK(d.enabled);
    CHECK(d.pad_[0] == 0);
    CHECK(d.pad_[1] == 0);
    CHECK(d.pad_[2] == 0);
    MeshStore store;
    Scene3D scene(store, mem::heap());
    CHECK(scene.meteor().level == 0.0f);
}

TEST_CASE("meteor: Scene3D set/get round trip") {
    MeshStore store;
    Scene3D scene(store, mem::heap());
    MeteorSky m = lit();
    m.enabled = false;
    scene.set_meteor(m);
    const MeteorSky& g = scene.meteor();
    CHECK(g.level == 1.5f);
    CHECK(g.rate == 12.0f);
    CHECK(g.brightness == 2.0f);
    CHECK(g.length == 1.5f);
    CHECK(g.speed == 1.25f);
    CHECK(g.time == 17.5f);
    CHECK(g.seed == 42u);
    CHECK_FALSE(g.enabled);
}

TEST_CASE("meteor: Low zeroes the level; Medium and High keep it") {
    ResetMeteorCvar reset;
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        const bool low = tier == QualityTier::Low;
        const QualitySettings q = quality_settings(tier);
        CHECK(q.meteor == !low);
        const MeteorSky m = apply_quality(q, lit());
        CHECK(m.level == (low ? 0.0f : 1.5f));
        CHECK(m.rate == 12.0f);
        CHECK(m.brightness == 2.0f);
        CHECK(m.length == 1.5f);
        CHECK(m.speed == 1.25f);
        CHECK(m.time == 17.5f);
        CHECK(m.seed == 42u);
        CHECK(m.enabled);
    }
}

TEST_CASE("meteor: r.meteor 0 zeroes the level on every tier") {
    ResetMeteorCvar reset;
    meteor_cvar().set_string("0");
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        CHECK(apply_quality(quality_settings(tier), lit()).level == 0.0f);
    }
}

TEST_CASE("meteor: a clear input stays off; enabled passes through") {
    ResetMeteorCvar reset;
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High})
        CHECK(apply_quality(quality_settings(tier), MeteorSky{}).level == 0.0f);
    MeteorSky m = lit();
    m.enabled = false;
    CHECK_FALSE(apply_quality(quality_settings(QualityTier::High), m).enabled);
}

TEST_CASE("meteor: apply_quality clamps every field and sanitizes NaN") {
    ResetMeteorCvar reset;
    const QualitySettings q = quality_settings(QualityTier::High);
    MeteorSky m;
    m.level = 100.0f;
    m.rate = 99.0f;
    m.brightness = 9.0f;
    m.length = 9.0f;
    m.speed = 9.0f;
    m.time = 1000.0f;
    m.seed = 1000000u;
    MeteorSky out = apply_quality(q, m);
    CHECK(out.level == 4.0f);
    CHECK(out.rate == 30.0f);
    CHECK(out.brightness == 4.0f);
    CHECK(out.length == 2.0f);
    CHECK(out.speed == 2.0f);
    CHECK(out.time == 60.0f);
    CHECK(out.seed == 65535u);

    m.level = -1.0f;
    m.rate = -3.0f;
    m.brightness = -1.0f;
    m.length = 0.0f;
    m.speed = 0.0f;
    m.time = -5.0f;
    out = apply_quality(q, m);
    CHECK(out.level == 0.0f);
    CHECK(out.rate == 0.0f);
    CHECK(out.brightness == 0.0f);
    CHECK(out.length == 0.5f);
    CHECK(out.speed == 0.5f);
    CHECK(out.time == 0.0f);

    const f32 nan = std::nanf("");
    m = MeteorSky{};
    m.level = nan;
    m.rate = nan;
    m.brightness = nan;
    m.length = nan;
    m.speed = nan;
    m.time = nan;
    out = apply_quality(q, m);
    CHECK(out.level == 0.0f);
    CHECK(out.rate == 4.0f);
    CHECK(out.brightness == 1.0f);
    CHECK(out.length == 1.0f);
    CHECK(out.speed == 1.0f);
    CHECK(out.time == 0.0f);

    m.time = INFINITY;
    CHECK(apply_quality(q, m).time == 0.0f);

    out = apply_quality(q, lit());
    CHECK(out.level == 1.5f);  // in range: unchanged
    CHECK(out.time == 17.5f);
}
