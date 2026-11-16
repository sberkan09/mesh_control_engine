// M-NIGHTSKY 14c: the NightSky carrier and how the quality tiers and `r.nightsky` map onto it.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/convar.h"
#include "mce/render/mesh.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

namespace {

ConVar& night_cvar() {
    ConVar* cv = ConVar::find(StringId("r.nightsky"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetNightCvar {
    ~ResetNightCvar() { night_cvar().reset(); }
};

NightSky starry() {
    NightSky n;
    n.moon_intensity = 2.0f;
    n.star_density = 0.4f;
    n.star_twinkle = 0.6f;
    return n;
}

}  // namespace

TEST_CASE("night sky: the default carrier is off, and so is a fresh Scene3D") {
    const NightSky d;
    CHECK(d.moon_dir.x == 0.0f);
    CHECK(d.moon_dir.y == 1.0f);
    CHECK(d.moon_dir.z == 0.0f);
    CHECK(d.moon_cos_size == 0.9995f);
    CHECK(d.moon_intensity == 0.0f);
    CHECK(d.moon_phase == 0.5f);
    CHECK(d.star_density == 0.0f);
    CHECK(d.star_brightness == 1.0f);
    CHECK(d.star_time == 0.0f);
    CHECK(d.night_fade == 1.0f);
    CHECK(d.enabled);
    CHECK(d.pad_[0] == 0);
    CHECK(d.pad_[1] == 0);
    CHECK(d.pad_[2] == 0);
    MeshStore store;
    Scene3D scene(store, mem::heap());
    CHECK(scene.night_sky().moon_intensity == 0.0f);
    CHECK(scene.night_sky().star_density == 0.0f);
}

TEST_CASE("night sky: Scene3D set/get round trip") {
    MeshStore store;
    Scene3D scene(store, mem::heap());
    NightSky n;
    n.moon_dir = math::vec3{0.0f, 0.0f, -1.0f};
    n.moon_cos_size = 0.99f;
    n.moon_intensity = 3.0f;
    n.moon_color = math::vec3{0.5f, 0.6f, 0.7f};
    n.moon_phase = 0.25f;
    n.star_density = 0.3f;
    n.star_brightness = 2.0f;
    n.star_twinkle = 0.2f;
    n.star_time = 12.5f;
    n.night_fade = 0.75f;
    n.enabled = false;
    scene.set_night_sky(n);
    const NightSky& g = scene.night_sky();
    CHECK(g.moon_dir.z == -1.0f);
    CHECK(g.moon_cos_size == 0.99f);
    CHECK(g.moon_intensity == 3.0f);
    CHECK(g.moon_color.y == 0.6f);
    CHECK(g.moon_phase == 0.25f);
    CHECK(g.star_density == 0.3f);
    CHECK(g.star_brightness == 2.0f);
    CHECK(g.star_twinkle == 0.2f);
    CHECK(g.star_time == 12.5f);
    CHECK(g.night_fade == 0.75f);
    CHECK_FALSE(g.enabled);
}

TEST_CASE("night sky: Low keeps the disk without twinkle and drops the stars; Medium and High keep everything") {
    ResetNightCvar reset;
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        const bool low = tier == QualityTier::Low;
        const QualitySettings q = quality_settings(tier);
        CHECK(q.night_sky == !low);
        const NightSky n = apply_quality(q, starry());
        CHECK(n.moon_intensity == 2.0f);
        CHECK(n.star_density == (low ? 0.0f : 0.4f));
        CHECK(n.star_twinkle == (low ? 0.0f : 0.6f));
        CHECK(n.enabled);
    }
}

TEST_CASE("night sky: r.nightsky 0 zeroes the moon and the stars on every tier") {
    ResetNightCvar reset;
    night_cvar().set_string("0");
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        const NightSky n = apply_quality(quality_settings(tier), starry());
        CHECK(n.moon_intensity == 0.0f);
        CHECK(n.star_density == 0.0f);
    }
}

TEST_CASE("night sky: a clear input stays off; enabled passes through") {
    ResetNightCvar reset;
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        const NightSky n = apply_quality(quality_settings(tier), NightSky{});
        CHECK(n.moon_intensity == 0.0f);
        CHECK(n.star_density == 0.0f);
    }
    NightSky n = starry();
    n.enabled = false;
    CHECK_FALSE(apply_quality(quality_settings(QualityTier::High), n).enabled);
}

TEST_CASE("night sky: apply_quality clamps every field and sanitizes NaN") {
    ResetNightCvar reset;
    const QualitySettings q = quality_settings(QualityTier::High);
    NightSky n;
    n.moon_cos_size = 0.0f;
    n.moon_intensity = 100.0f;
    n.moon_color = math::vec3{-1.0f, 2.0f, 0.5f};
    n.moon_phase = 3.0f;
    n.star_density = 1.5f;
    n.star_brightness = 99.0f;
    n.star_twinkle = -1.0f;
    n.night_fade = 2.0f;
    NightSky out = apply_quality(q, n);
    CHECK(out.moon_cos_size == 0.9f);
    CHECK(out.moon_intensity == 8.0f);
    CHECK(out.moon_color.x == 0.0f);
    CHECK(out.moon_color.y == 1.0f);
    CHECK(out.moon_color.z == 0.5f);
    CHECK(out.moon_phase == 1.0f);
    CHECK(out.star_density == 1.0f);
    CHECK(out.star_brightness == 4.0f);
    CHECK(out.star_twinkle == 0.0f);
    CHECK(out.night_fade == 1.0f);

    n.moon_cos_size = 5.0f;
    CHECK(apply_quality(q, n).moon_cos_size == 1.0f);

    const f32 nan = std::nanf("");
    n = NightSky{};
    n.moon_dir = math::vec3{nan, nan, nan};
    n.moon_cos_size = nan;
    n.moon_intensity = nan;
    n.moon_color = math::vec3{nan, nan, nan};
    n.moon_phase = nan;
    n.star_density = nan;
    n.star_brightness = nan;
    n.star_twinkle = nan;
    n.star_time = nan;
    n.night_fade = nan;
    out = apply_quality(q, n);
    CHECK(out.moon_dir.x == 0.0f);
    CHECK(out.moon_dir.y == 1.0f);
    CHECK(out.moon_dir.z == 0.0f);
    CHECK(out.moon_cos_size == 0.9995f);
    CHECK(out.moon_intensity == 0.0f);
    CHECK(out.moon_color.x == 0.0f);
    CHECK(out.moon_color.y == 0.0f);
    CHECK(out.moon_color.z == 0.0f);
    CHECK(out.moon_phase == 0.0f);
    CHECK(out.star_density == 0.0f);
    CHECK(out.star_brightness == 1.0f);
    CHECK(out.star_twinkle == 0.0f);
    CHECK(out.star_time == 0.0f);
    CHECK(out.night_fade == 0.0f);

    n = starry();
    n.star_time = 42.0f;
    out = apply_quality(q, n);
    CHECK(out.moon_intensity == 2.0f);  // in range: unchanged
    CHECK(out.star_density == 0.4f);
    CHECK(out.star_time == 42.0f);
}

TEST_CASE("night sky: moon_dir is normalized, a zero or infinite vector falls back to +y") {
    ResetNightCvar reset;
    const QualitySettings q = quality_settings(QualityTier::High);
    NightSky n = starry();
    n.moon_dir = math::vec3{0.0f, 0.0f, 5.0f};
    NightSky out = apply_quality(q, n);
    CHECK(out.moon_dir.x == doctest::Approx(0.0f));
    CHECK(out.moon_dir.y == doctest::Approx(0.0f));
    CHECK(out.moon_dir.z == doctest::Approx(1.0f));
    n.moon_dir = math::vec3{3.0f, 4.0f, 12.0f};
    out = apply_quality(q, n);
    const f32 len = std::sqrt(out.moon_dir.x * out.moon_dir.x + out.moon_dir.y * out.moon_dir.y +
                              out.moon_dir.z * out.moon_dir.z);
    CHECK(len == doctest::Approx(1.0f));
    CHECK(out.moon_dir.x == doctest::Approx(3.0f / 13.0f));
    n.moon_dir = math::vec3{0.0f, 0.0f, 0.0f};
    out = apply_quality(q, n);
    CHECK(out.moon_dir.y == 1.0f);
    n.moon_dir = math::vec3{INFINITY, 0.0f, 0.0f};
    out = apply_quality(q, n);
    CHECK(out.moon_dir.y == 1.0f);
}
