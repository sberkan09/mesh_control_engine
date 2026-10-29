// M-AURORA 16b: the AuroraSky carrier and how the quality tiers and `r.aurora` map onto it.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/convar.h"
#include "mce/render/mesh.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

namespace {

ConVar& aurora_cvar() {
    ConVar* cv = ConVar::find(StringId("r.aurora"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetAuroraCvar {
    ~ResetAuroraCvar() { aurora_cvar().reset(); }
};

AuroraSky lit() {
    AuroraSky a;
    a.level = 1.5f;
    a.curtain_count = 4;
    a.speed = 2.0f;
    return a;
}

}  // namespace

TEST_CASE("aurora: the default carrier is off, and so is a fresh Scene3D") {
    const AuroraSky d;
    CHECK(d.level == 0.0f);
    CHECK(d.base_height == 0.18f);
    CHECK(d.height_span == 0.35f);
    CHECK(d.azimuth.x == 0.0f);
    CHECK(d.azimuth.y == 1.0f);
    CHECK(d.curtain_count == 3);
    CHECK(d.time == 0.0f);
    CHECK(d.speed == 1.0f);
    CHECK(d.enabled);
    CHECK(d.pad_[0] == 0);
    CHECK(d.pad_[1] == 0);
    CHECK(d.pad_[2] == 0);
    MeshStore store;
    Scene3D scene(store, mem::heap());
    CHECK(scene.aurora().level == 0.0f);
}

TEST_CASE("aurora: Scene3D set/get round trip") {
    MeshStore store;
    Scene3D scene(store, mem::heap());
    AuroraSky a;
    a.level = 2.0f;
    a.base_height = 0.1f;
    a.height_span = 0.5f;
    a.azimuth = math::vec2{1.0f, 0.0f};
    a.half_width_cos = 0.5f;
    a.curtain_count = 5;
    a.time = 12.5f;
    a.speed = 0.5f;
    a.low_color = math::vec3{0.1f, 0.2f, 0.3f};
    a.high_color = math::vec3{0.4f, 0.5f, 0.6f};
    a.enabled = false;
    scene.set_aurora(a);
    const AuroraSky& g = scene.aurora();
    CHECK(g.level == 2.0f);
    CHECK(g.base_height == 0.1f);
    CHECK(g.height_span == 0.5f);
    CHECK(g.azimuth.x == 1.0f);
    CHECK(g.half_width_cos == 0.5f);
    CHECK(g.curtain_count == 5);
    CHECK(g.time == 12.5f);
    CHECK(g.speed == 0.5f);
    CHECK(g.low_color.y == 0.2f);
    CHECK(g.high_color.z == 0.6f);
    CHECK_FALSE(g.enabled);
}

TEST_CASE("aurora: Low zeroes the level; Medium and High keep it") {
    ResetAuroraCvar reset;
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        const bool low = tier == QualityTier::Low;
        const QualitySettings q = quality_settings(tier);
        CHECK(q.aurora == !low);
        const AuroraSky a = apply_quality(q, lit());
        CHECK(a.level == (low ? 0.0f : 1.5f));
        CHECK(a.curtain_count == 4);
        CHECK(a.speed == 2.0f);
        CHECK(a.enabled);
    }
}

TEST_CASE("aurora: r.aurora 0 zeroes the level on every tier") {
    ResetAuroraCvar reset;
    aurora_cvar().set_string("0");
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        CHECK(apply_quality(quality_settings(tier), lit()).level == 0.0f);
    }
}

TEST_CASE("aurora: a clear input stays off; enabled passes through") {
    ResetAuroraCvar reset;
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High})
        CHECK(apply_quality(quality_settings(tier), AuroraSky{}).level == 0.0f);
    AuroraSky a = lit();
    a.enabled = false;
    CHECK_FALSE(apply_quality(quality_settings(QualityTier::High), a).enabled);
}

TEST_CASE("aurora: apply_quality clamps every field and sanitizes NaN") {
    ResetAuroraCvar reset;
    const QualitySettings q = quality_settings(QualityTier::High);
    AuroraSky a;
    a.level = 100.0f;
    a.base_height = 0.0f;
    a.height_span = 5.0f;
    a.half_width_cos = -3.0f;
    a.curtain_count = 99;
    a.speed = 99.0f;
    a.low_color = math::vec3{-1.0f, 2.0f, 0.5f};
    a.high_color = math::vec3{3.0f, -2.0f, 0.25f};
    AuroraSky out = apply_quality(q, a);
    CHECK(out.level == 4.0f);
    CHECK(out.base_height == 0.02f);
    CHECK(out.height_span == 0.8f);
    CHECK(out.half_width_cos == -1.0f);
    CHECK(out.curtain_count == 6);
    CHECK(out.speed == 4.0f);
    CHECK(out.low_color.x == 0.0f);
    CHECK(out.low_color.y == 1.0f);
    CHECK(out.low_color.z == 0.5f);
    CHECK(out.high_color.x == 1.0f);
    CHECK(out.high_color.y == 0.0f);
    CHECK(out.high_color.z == 0.25f);

    a.level = -1.0f;
    a.base_height = 9.0f;
    a.height_span = 0.0f;
    a.half_width_cos = 7.0f;
    a.curtain_count = -2;
    a.speed = -1.0f;
    out = apply_quality(q, a);
    CHECK(out.level == 0.0f);
    CHECK(out.base_height == 0.6f);
    CHECK(out.height_span == 0.05f);
    CHECK(out.half_width_cos == 1.0f);
    CHECK(out.curtain_count == 1);
    CHECK(out.speed == 0.0f);

    const f32 nan = std::nanf("");
    a = AuroraSky{};
    a.level = nan;
    a.base_height = nan;
    a.height_span = nan;
    a.azimuth = math::vec2{nan, nan};
    a.half_width_cos = nan;
    a.time = nan;
    a.speed = nan;
    a.low_color = math::vec3{nan, nan, nan};
    a.high_color = math::vec3{nan, nan, nan};
    out = apply_quality(q, a);
    CHECK(out.level == 0.0f);
    CHECK(out.base_height == 0.18f);
    CHECK(out.height_span == 0.35f);
    CHECK(out.azimuth.x == 0.0f);
    CHECK(out.azimuth.y == 1.0f);
    CHECK(out.half_width_cos == 0.8192f);
    CHECK(out.time == 0.0f);
    CHECK(out.speed == 1.0f);
    CHECK(out.low_color.x == 0.0f);
    CHECK(out.low_color.y == 0.0f);
    CHECK(out.low_color.z == 0.0f);
    CHECK(out.high_color.x == 0.0f);
    CHECK(out.high_color.y == 0.0f);
    CHECK(out.high_color.z == 0.0f);

    a = lit();
    a.time = 42.0f;
    out = apply_quality(q, a);
    CHECK(out.level == 1.5f);  // in range: unchanged
    CHECK(out.time == 42.0f);
}

TEST_CASE("aurora: azimuth is normalized, a zero or infinite vector falls back to (0, 1)") {
    ResetAuroraCvar reset;
    const QualitySettings q = quality_settings(QualityTier::High);
    AuroraSky a = lit();
    a.azimuth = math::vec2{5.0f, 0.0f};
    AuroraSky out = apply_quality(q, a);
    CHECK(out.azimuth.x == doctest::Approx(1.0f));
    CHECK(out.azimuth.y == doctest::Approx(0.0f));
    a.azimuth = math::vec2{3.0f, 4.0f};
    out = apply_quality(q, a);
    CHECK(out.azimuth.x == doctest::Approx(0.6f));
    CHECK(out.azimuth.y == doctest::Approx(0.8f));
    a.azimuth = math::vec2{0.0f, 0.0f};
    out = apply_quality(q, a);
    CHECK(out.azimuth.x == 0.0f);
    CHECK(out.azimuth.y == 1.0f);
    a.azimuth = math::vec2{INFINITY, 0.0f};
    out = apply_quality(q, a);
    CHECK(out.azimuth.x == 0.0f);
    CHECK(out.azimuth.y == 1.0f);
}
