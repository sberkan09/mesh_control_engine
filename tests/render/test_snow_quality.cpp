// M-SNOWCOVER 6a: the SnowCover carrier and how the quality tiers and `r.snow_cover` map onto it.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/convar.h"
#include "mce/render/mesh.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

namespace {

ConVar& snow_cvar() {
    ConVar* cv = ConVar::find(StringId("r.snow_cover"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetSnowCvar {
    ~ResetSnowCvar() { snow_cvar().reset(); }
};

SnowCover snowy() {
    SnowCover s;
    s.cover = 0.8f;
    return s;
}

}  // namespace

TEST_CASE("snow cover: the default carrier is dry, and so is a fresh Scene3D") {
    const SnowCover d;
    CHECK(d.cover == 0.0f);
    CHECK(d.coverage == 0.8f);
    CHECK(d.noise_scale == 3.0f);
    CHECK(d.wind_bias == 0.5f);
    CHECK(d.noise_enabled);
    MeshStore store;
    Scene3D scene(store, mem::heap());
    CHECK(scene.snow_cover().cover == 0.0f);
}

TEST_CASE("snow cover: Scene3D set/get round trip") {
    MeshStore store;
    Scene3D scene(store, mem::heap());
    SnowCover s;
    s.cover = 0.6f;
    s.coverage = 0.4f;
    s.noise_scale = 7.0f;
    s.wind_bias = 0.25f;
    s.noise_enabled = false;
    scene.set_snow_cover(s);
    const SnowCover& g = scene.snow_cover();
    CHECK(g.cover == 0.6f);
    CHECK(g.coverage == 0.4f);
    CHECK(g.noise_scale == 7.0f);
    CHECK(g.wind_bias == 0.25f);
    CHECK_FALSE(g.noise_enabled);
}

TEST_CASE("snow cover: tiers keep the cover, only Low drops the noise") {
    ResetSnowCvar reset;
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        const QualitySettings q = quality_settings(tier);
        CHECK(q.snow_cover);
        CHECK(q.snow_noise == (tier != QualityTier::Low));
        const SnowCover s = apply_quality(q, snowy());
        CHECK(s.cover == 0.8f);
        CHECK(s.coverage == 0.8f);
        CHECK(s.noise_enabled == (tier != QualityTier::Low));
    }
}

TEST_CASE("snow cover: r.snow_cover 0 zeroes the cover on every tier") {
    ResetSnowCvar reset;
    snow_cvar().set_string("0");
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        CHECK(apply_quality(quality_settings(tier), snowy()).cover == 0.0f);
    }
}

TEST_CASE("snow cover: a tier that disables it zeroes the cover; a dry input stays dry") {
    ResetSnowCvar reset;
    QualitySettings q = quality_settings(QualityTier::High);
    q.snow_cover = false;
    CHECK(apply_quality(q, snowy()).cover == 0.0f);
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High})
        CHECK(apply_quality(quality_settings(tier), SnowCover{}).cover == 0.0f);
}

TEST_CASE("snow cover: apply_quality clamps cover, coverage and wind_bias to 0..1 and keeps a cell size") {
    ResetSnowCvar reset;
    const QualitySettings q = quality_settings(QualityTier::High);
    SnowCover s;
    s.cover = 1.5f;
    s.coverage = -0.25f;
    s.wind_bias = 2.0f;
    s.noise_scale = 0.0f;
    SnowCover out = apply_quality(q, s);
    CHECK(out.cover == 1.0f);
    CHECK(out.coverage == 0.0f);
    CHECK(out.wind_bias == 1.0f);
    CHECK(out.noise_scale == 0.25f);  // never 0: the pass upload divides by it

    s.cover = std::nanf("");
    s.coverage = std::nanf("");
    s.wind_bias = std::nanf("");
    s.noise_scale = std::nanf("");
    out = apply_quality(q, s);
    CHECK(out.cover == 0.0f);
    CHECK(out.coverage == 0.0f);
    CHECK(out.wind_bias == 0.0f);
    CHECK(out.noise_scale == 0.25f);

    s = snowy();
    s.noise_scale = 1.0e6f;
    out = apply_quality(q, s);
    CHECK(out.cover == 0.8f);  // in range: unchanged
    CHECK(out.wind_bias == 0.5f);
    CHECK(out.noise_scale == 256.0f);  // at most the noise's 256 m period
}
