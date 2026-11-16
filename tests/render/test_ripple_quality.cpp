// M-RAINFX 7a: the Ripples carrier and how the quality tiers and `r.rain_ripples` map onto it.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/convar.h"
#include "mce/render/mesh.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

namespace {

ConVar& ripple_cvar() {
    ConVar* cv = ConVar::find(StringId("r.rain_ripples"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetRippleCvar {
    ~ResetRippleCvar() { ripple_cvar().reset(); }
};

Ripples rainy() {
    Ripples r;
    r.strength = 0.8f;
    return r;
}

}  // namespace

TEST_CASE("ripples: the default carrier is off, and so is a fresh Scene3D") {
    const Ripples d;
    CHECK(d.strength == 0.0f);
    CHECK(d.density == 0.5f);
    CHECK(d.speed == 1.0f);
    CHECK(d.ring_width == 0.15f);
    CHECK(d.enabled);
    MeshStore store;
    Scene3D scene(store, mem::heap());
    CHECK(scene.ripples().strength == 0.0f);
}

TEST_CASE("ripples: Scene3D set/get round trip") {
    MeshStore store;
    Scene3D scene(store, mem::heap());
    Ripples r;
    r.strength = 0.6f;
    r.density = 0.4f;
    r.speed = 2.5f;
    r.ring_width = 0.25f;
    r.enabled = false;
    scene.set_ripples(r);
    const Ripples& g = scene.ripples();
    CHECK(g.strength == 0.6f);
    CHECK(g.density == 0.4f);
    CHECK(g.speed == 2.5f);
    CHECK(g.ring_width == 0.25f);
    CHECK_FALSE(g.enabled);
}

TEST_CASE("ripples: Low drops them, Medium and High keep them") {
    ResetRippleCvar reset;
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        const QualitySettings q = quality_settings(tier);
        CHECK(q.rain_ripples == (tier != QualityTier::Low));
        const Ripples r = apply_quality(q, rainy());
        CHECK(r.strength == (tier != QualityTier::Low ? 0.8f : 0.0f));
        CHECK(r.density == 0.5f);
        CHECK(r.speed == 1.0f);
        CHECK(r.ring_width == 0.15f);
        CHECK(r.enabled);
    }
}

TEST_CASE("ripples: r.rain_ripples 0 zeroes the strength on every tier") {
    ResetRippleCvar reset;
    ripple_cvar().set_string("0");
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        CHECK(apply_quality(quality_settings(tier), rainy()).strength == 0.0f);
    }
}

TEST_CASE("ripples: a tier that disables them zeroes the strength; a dry input stays dry; enabled passes through") {
    ResetRippleCvar reset;
    QualitySettings q = quality_settings(QualityTier::High);
    q.rain_ripples = false;
    CHECK(apply_quality(q, rainy()).strength == 0.0f);
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High})
        CHECK(apply_quality(quality_settings(tier), Ripples{}).strength == 0.0f);
    Ripples r = rainy();
    r.enabled = false;
    CHECK_FALSE(apply_quality(quality_settings(QualityTier::High), r).enabled);
}

TEST_CASE("ripples: apply_quality clamps strength, density and ring_width to 0..1 and speed to 0.1..10") {
    ResetRippleCvar reset;
    const QualitySettings q = quality_settings(QualityTier::High);
    Ripples r;
    r.strength = 1.5f;
    r.density = -0.25f;
    r.ring_width = 2.0f;
    r.speed = 0.0f;
    Ripples out = apply_quality(q, r);
    CHECK(out.strength == 1.0f);
    CHECK(out.density == 0.0f);
    CHECK(out.ring_width == 1.0f);
    CHECK(out.speed == 0.1f);

    r.speed = 1.0e6f;
    CHECK(apply_quality(q, r).speed == 10.0f);
    r.speed = -3.0f;
    CHECK(apply_quality(q, r).speed == 0.1f);

    const f32 nan = std::nanf("");
    r.strength = nan;
    r.density = nan;
    r.ring_width = nan;
    r.speed = nan;
    out = apply_quality(q, r);
    CHECK(out.strength == 0.0f);
    CHECK(out.density == 0.0f);
    CHECK(out.ring_width == 0.0f);
    CHECK(out.speed == 1.0f);  // NaN falls back to the nominal rate

    r = rainy();
    r.speed = 4.0f;
    out = apply_quality(q, r);
    CHECK(out.strength == 0.8f);  // in range: unchanged
    CHECK(out.speed == 4.0f);
}
