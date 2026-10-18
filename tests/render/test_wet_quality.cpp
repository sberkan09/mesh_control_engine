// M-WETSURFACE 5a: the WetSurfaces carrier and how the quality tiers and `r.wet_surfaces` map onto it.

#include <doctest/doctest.h>

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
