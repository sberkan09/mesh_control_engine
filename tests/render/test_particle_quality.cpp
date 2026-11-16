// M-PARTICLEQ 28a: the particle_scale knob of QualitySettings per quality tier.

#include <doctest/doctest.h>

#include "mce/convar.h"
#include "mce/render/quality.h"

using namespace mce;
using namespace mce::render;

TEST_CASE("particle quality: Low keeps half, Medium and High keep all") {
    CHECK(quality_settings(QualityTier::Low).particle_scale == 0.5f);
    CHECK(quality_settings(QualityTier::Medium).particle_scale == 1.0f);
    CHECK(quality_settings(QualityTier::High).particle_scale == 1.0f);
}

TEST_CASE("particle quality: a default-constructed QualitySettings keeps all") {
    const QualitySettings q;
    CHECK(q.particle_scale == 1.0f);
}

TEST_CASE("particle quality: every tier is within (0, 1]") {
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        const f32 s = quality_settings(tier).particle_scale;
        CHECK(s > 0.0f);
        CHECK(s <= 1.0f);
    }
}

namespace {

ConVar& particles_cvar() {
    ConVar* cv = ConVar::find(StringId("r.particles"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetParticlesCvar {
    ~ResetParticlesCvar() { particles_cvar().reset(); }
};

}  // namespace

// M-PARTICLEUSER 29a: r.particles multiplies the tier's particle_scale.
TEST_CASE("particle quality: r.particles defaults to 1 and leaves the tiers unchanged") {
    ResetParticlesCvar reset;
    CHECK(quality_settings(QualityTier::Low).particle_scale == 0.5f);
    CHECK(quality_settings(QualityTier::High).particle_scale == 1.0f);
}

TEST_CASE("particle quality: r.particles scales the tier, clamps to 0..1, and 0 keeps none") {
    ResetParticlesCvar reset;
    particles_cvar().set_string("0.5");
    CHECK(quality_settings(QualityTier::High).particle_scale == doctest::Approx(0.5f));
    CHECK(quality_settings(QualityTier::Low).particle_scale == doctest::Approx(0.25f));
    particles_cvar().set_string("3");
    CHECK(quality_settings(QualityTier::Low).particle_scale == doctest::Approx(0.5f));
    particles_cvar().set_string("-1");
    CHECK(quality_settings(QualityTier::High).particle_scale == 0.0f);
}
