// M-WETSURFACE 5a: the WetSurfaces carrier and how the quality tiers and `r.wet_surfaces` map onto it.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/convar.h"
#include "mce/render/mesh.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

namespace {

ConVar& wet_cvar() {
    ConVar* cv = ConVar::find(StringId("r.wet_surfaces"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetWetCvar {
    ~ResetWetCvar() { wet_cvar().reset(); }
};

WetSurfaces rainy() {
    WetSurfaces w;
    w.wetness = 0.8f;
    return w;
}

}  // namespace

TEST_CASE("wet surfaces: the default carrier is dry, and so is a fresh Scene3D") {
    CHECK(WetSurfaces{}.wetness == 0.0f);
    MeshStore store;
    Scene3D scene(store, mem::heap());
    CHECK(scene.wet_surfaces().wetness == 0.0f);
    scene.set_wet_surfaces(rainy());
    CHECK(scene.wet_surfaces().wetness == 0.8f);
}

TEST_CASE("wet surfaces: tiers keep the wetness, Low drops puddles") {
    ResetWetCvar reset;
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        const QualitySettings q = quality_settings(tier);
        const WetSurfaces w = apply_quality(q, rainy());
        CHECK(w.wetness == 0.8f);
        CHECK(w.darken == 0.5f);
        CHECK(w.puddles_enabled == (tier != QualityTier::Low));
    }
}

TEST_CASE("wet surfaces: r.wet_surfaces 0 zeroes the wetness on every tier") {
    ResetWetCvar reset;
    wet_cvar().set_string("0");
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        CHECK(apply_quality(quality_settings(tier), rainy()).wetness == 0.0f);
    }
}

TEST_CASE("wet surfaces: a tier that disables them zeroes the wetness; a dry input stays dry") {
    ResetWetCvar reset;
    QualitySettings q = quality_settings(QualityTier::High);
    q.wet_surfaces = false;
    CHECK(apply_quality(q, rainy()).wetness == 0.0f);
    CHECK(apply_quality(quality_settings(QualityTier::High), WetSurfaces{}).wetness == 0.0f);
}

TEST_CASE("wet surfaces: apply_quality clamps wetness, puddles, darken and roughness to 0..1 and keeps a cell size") {
    ResetWetCvar reset;
    const QualitySettings q = quality_settings(QualityTier::High);
    WetSurfaces w;
    w.wetness = 1.5f;
    w.puddles = -0.25f;
    w.darken = 2.0f;
    w.roughness = -1.0f;
    w.puddle_scale = 0.0f;
    WetSurfaces out = apply_quality(q, w);
    CHECK(out.wetness == 1.0f);
    CHECK(out.puddles == 0.0f);
    CHECK(out.darken == 1.0f);
    CHECK(out.roughness == 0.0f);
    CHECK(out.puddle_scale == 0.25f);  // never 0: the pass upload divides by it

    w.wetness = std::nanf("");
    w.puddles = std::nanf("");
    w.puddle_scale = std::nanf("");
    out = apply_quality(q, w);
    CHECK(out.wetness == 0.0f);
    CHECK(out.puddles == 0.0f);
    CHECK(out.puddle_scale == 0.25f);

    w = rainy();
    w.puddle_scale = 1.0e6f;
    out = apply_quality(q, w);
    CHECK(out.wetness == 0.8f);  // in range: unchanged
    CHECK(out.puddles == 0.5f);
    CHECK(out.puddle_scale == 256.0f);  // at most the noise's 256 m period
}
