// M-WATER 15b: the WaterSurface carrier and how the quality tiers and `r.water` map onto it.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/convar.h"
#include "mce/render/mesh.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

namespace {

ConVar& water_cvar() {
    ConVar* cv = ConVar::find(StringId("r.water"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetWaterCvar {
    ~ResetWaterCvar() { water_cvar().reset(); }
};

WaterSurface sea() {
    WaterSurface w;
    w.enabled = true;
    w.amplitude = 0.8f;
    w.foam_amount = 0.7f;
    return w;
}

}  // namespace

TEST_CASE("water: the default carrier is disabled, and so is a fresh Scene3D") {
    const WaterSurface d;
    CHECK_FALSE(d.enabled);
    CHECK(d.level_y == 0.0f);
    CHECK(d.half_extent == 500.0f);
    CHECK(d.wave_count == 4);
    CHECK(d.amplitude == 0.4f);
    CHECK(d.wavelength == 24.0f);
    CHECK(d.steepness == 0.6f);
    CHECK(d.wind_dir.x == 1.0f);
    CHECK(d.wind_dir.y == 0.0f);
    CHECK(d.time == 0.0f);
    CHECK(d.speed_scale == 1.0f);
    CHECK(d.depth_scale_m == 8.0f);
    CHECK(d.reflectivity == 1.0f);
    CHECK(d.roughness == 0.08f);
    CHECK(d.foam_amount == 0.5f);
    CHECK(d.foam_width_m == 1.5f);
    CHECK(d.pad_[0] == 0);
    CHECK(d.pad_[1] == 0);
    CHECK(d.pad_[2] == 0);
    MeshStore store;
    Scene3D scene(store, mem::heap());
    CHECK_FALSE(scene.water().enabled);
}

TEST_CASE("water: Scene3D set/get round trip") {
    MeshStore store;
    Scene3D scene(store, mem::heap());
    WaterSurface w;
    w.level_y = 3.0f;
    w.half_extent = 900.0f;
    w.wave_count = 6;
    w.amplitude = 1.5f;
    w.wavelength = 40.0f;
    w.steepness = 0.3f;
    w.wind_dir = math::vec2{0.0f, 1.0f};
    w.time = 12.5f;
    w.speed_scale = 2.0f;
    w.shallow_color = math::vec3{0.1f, 0.2f, 0.3f};
    w.deep_color = math::vec3{0.4f, 0.5f, 0.6f};
    w.depth_scale_m = 20.0f;
    w.reflectivity = 0.5f;
    w.roughness = 0.2f;
    w.foam_amount = 0.9f;
    w.foam_width_m = 3.0f;
    w.enabled = true;
    scene.set_water(w);
    const WaterSurface& g = scene.water();
    CHECK(g.level_y == 3.0f);
    CHECK(g.half_extent == 900.0f);
    CHECK(g.wave_count == 6);
    CHECK(g.amplitude == 1.5f);
    CHECK(g.wavelength == 40.0f);
    CHECK(g.steepness == 0.3f);
    CHECK(g.wind_dir.y == 1.0f);
    CHECK(g.time == 12.5f);
    CHECK(g.speed_scale == 2.0f);
    CHECK(g.shallow_color.z == 0.3f);
    CHECK(g.deep_color.x == 0.4f);
    CHECK(g.depth_scale_m == 20.0f);
    CHECK(g.reflectivity == 0.5f);
    CHECK(g.roughness == 0.2f);
    CHECK(g.foam_amount == 0.9f);
    CHECK(g.foam_width_m == 3.0f);
    CHECK(g.enabled);
}

TEST_CASE("water: Low is flat and foamless; Medium and High keep everything") {
    ResetWaterCvar reset;
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        const bool low = tier == QualityTier::Low;
        const QualitySettings q = quality_settings(tier);
        CHECK(q.water == !low);
        const WaterSurface w = apply_quality(q, sea());
        CHECK(w.enabled);  // Low still draws the plane
        CHECK(w.amplitude == (low ? 0.0f : 0.8f));
        CHECK(w.foam_amount == (low ? 0.0f : 0.7f));
        CHECK(w.wave_count == 4);
    }
}

TEST_CASE("water: r.water 0 disables the plane on every tier; a disabled input stays disabled") {
    ResetWaterCvar reset;
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        CHECK(apply_quality(quality_settings(tier), WaterSurface{}).enabled == false);
    }
    water_cvar().set_string("0");
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        CHECK_FALSE(apply_quality(quality_settings(tier), sea()).enabled);
    }
}

TEST_CASE("water: apply_quality clamps every field and sanitizes NaN") {
    ResetWaterCvar reset;
    const QualitySettings q = quality_settings(QualityTier::High);
    WaterSurface w = sea();
    w.level_y = -5000.0f;
    w.half_extent = 0.0f;
    w.wave_count = 99;
    w.amplitude = 100.0f;
    w.wavelength = 0.0f;
    w.steepness = 2.0f;
    w.speed_scale = 9.0f;
    w.shallow_color = math::vec3{-1.0f, 2.0f, 0.5f};
    w.depth_scale_m = 0.0f;
    w.reflectivity = -1.0f;
    w.roughness = 3.0f;
    w.foam_amount = 2.0f;
    w.foam_width_m = 100.0f;
    WaterSurface out = apply_quality(q, w);
    CHECK(out.level_y == -1000.0f);
    CHECK(out.half_extent == 1.0f);
    CHECK(out.wave_count == 8);
    CHECK(out.amplitude == 10.0f);
    CHECK(out.wavelength == 1.0f);
    CHECK(out.steepness == 1.0f);
    CHECK(out.speed_scale == 4.0f);
    CHECK(out.shallow_color.x == 0.0f);
    CHECK(out.shallow_color.y == 1.0f);
    CHECK(out.shallow_color.z == 0.5f);
    CHECK(out.depth_scale_m == 0.1f);
    CHECK(out.reflectivity == 0.0f);
    CHECK(out.roughness == 1.0f);
    CHECK(out.foam_amount == 1.0f);
    CHECK(out.foam_width_m == 20.0f);
    w.wave_count = -3;
    w.level_y = 5000.0f;
    out = apply_quality(q, w);
    CHECK(out.wave_count == 1);
    CHECK(out.level_y == 1000.0f);

    const f32 nan = std::nanf("");
    w = WaterSurface{};
    w.enabled = true;
    w.level_y = nan;
    w.half_extent = nan;
    w.amplitude = nan;
    w.wavelength = nan;
    w.steepness = nan;
    w.wind_dir = math::vec2{nan, nan};
    w.time = nan;
    w.speed_scale = nan;
    w.shallow_color = math::vec3{nan, nan, nan};
    w.deep_color = math::vec3{nan, nan, nan};
    w.depth_scale_m = nan;
    w.reflectivity = nan;
    w.roughness = nan;
    w.foam_amount = nan;
    w.foam_width_m = nan;
    out = apply_quality(q, w);
    CHECK(out.level_y == 0.0f);
    CHECK(out.half_extent == 500.0f);
    CHECK(out.amplitude == 0.0f);
    CHECK(out.wavelength == 24.0f);
    CHECK(out.steepness == 0.0f);
    CHECK(out.wind_dir.x == 1.0f);
    CHECK(out.wind_dir.y == 0.0f);
    CHECK(out.time == 0.0f);
    CHECK(out.speed_scale == 1.0f);
    CHECK(out.shallow_color.x == 0.0f);
    CHECK(out.deep_color.z == 0.0f);
    CHECK(out.depth_scale_m == 8.0f);
    CHECK(out.reflectivity == 1.0f);
    CHECK(out.roughness == 0.08f);
    CHECK(out.foam_amount == 0.0f);
    CHECK(out.foam_width_m == 1.5f);

    w = sea();
    w.time = 42.0f;
    out = apply_quality(q, w);
    CHECK(out.amplitude == 0.8f);  // in range: unchanged
    CHECK(out.time == 42.0f);
}

TEST_CASE("water: wind_dir is normalized, a zero or infinite vector falls back to +x") {
    ResetWaterCvar reset;
    const QualitySettings q = quality_settings(QualityTier::High);
    WaterSurface w = sea();
    w.wind_dir = math::vec2{0.0f, 5.0f};
    WaterSurface out = apply_quality(q, w);
    CHECK(out.wind_dir.x == doctest::Approx(0.0f));
    CHECK(out.wind_dir.y == doctest::Approx(1.0f));
    w.wind_dir = math::vec2{3.0f, 4.0f};
    out = apply_quality(q, w);
    CHECK(out.wind_dir.x == doctest::Approx(0.6f));
    CHECK(out.wind_dir.y == doctest::Approx(0.8f));
    w.wind_dir = math::vec2{0.0f, 0.0f};
    out = apply_quality(q, w);
    CHECK(out.wind_dir.x == 1.0f);
    w.wind_dir = math::vec2{INFINITY, 0.0f};
    out = apply_quality(q, w);
    CHECK(out.wind_dir.x == 1.0f);
    CHECK(out.wind_dir.y == 0.0f);
}
