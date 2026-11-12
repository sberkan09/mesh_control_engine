// M-HALO 54a: the Halo carrier (ice halo and sun dogs seen through the Cirrus layer) and how the quality tiers and
// `r.halo` map onto it. Data only: nothing draws it yet (the sky shader is 54b). No GPU.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <limits>

#include "mce/convar.h"
#include "mce/render/mesh.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();

static_assert(sizeof(Halo) == 20, "Halo has no implicit padding: four f32, bool + pad_[3]");

ConVar& halo_cvar() {
    ConVar* cv = ConVar::find(StringId("r.halo"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetHaloCvar {
    ~ResetHaloCvar() { halo_cvar().reset(); }
};

Halo lit() {
    Halo h;
    h.strength = 0.8f;
    h.sun_dogs = 0.6f;
    h.moon = 0.25f;
    h.dispersion = 0.5f;
    return h;
}

// A bit-exact comparison on purpose: the pads are explicit and zeroed, so the bytes are the value.
bool same_bytes(const Halo& a, const Halo& b) {
    return std::memcmp(&a, &b, sizeof(Halo)) == 0;  // NOLINT(bugprone-suspicious-memory-comparison): explicit pads
}

}  // namespace

TEST_CASE("halo: the default carrier is off, and so is a fresh Scene3D") {
    const Halo d;
    CHECK(d.strength == 0.0f);
    CHECK(d.sun_dogs == 1.0f);
    CHECK(d.moon == 0.5f);
    CHECK(d.dispersion == 1.0f);
    CHECK(d.enabled);
    for (u8 b : d.pad_) CHECK(b == 0);
    MeshStore store;
    Scene3D scene(store, mem::heap());
    CHECK(same_bytes(scene.halo(), Halo{}));
}

TEST_CASE("halo: the defaults pass apply_quality bit for bit on every tier") {
    ResetHaloCvar reset;
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        CHECK(same_bytes(apply_quality(quality_settings(tier), Halo{}), Halo{}));
    }
    const Halo in = lit();  // in range: only the tier's gate may change it
    CHECK(same_bytes(apply_quality(quality_settings(QualityTier::High), in), in));
}

TEST_CASE("halo: Scene3D set/get round trip") {
    MeshStore store;
    Scene3D scene(store, mem::heap());
    Halo h = lit();
    h.enabled = false;
    scene.set_halo(h);
    CHECK(same_bytes(scene.halo(), h));
    CHECK(scene.cirrus().coverage == 0.0f);  // the cirrus layer is its own carrier
}

TEST_CASE("halo: Low zeroes the strength; Medium and High keep it") {
    ResetHaloCvar reset;
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        const bool low = tier == QualityTier::Low;
        const QualitySettings q = quality_settings(tier);
        CHECK(q.halo == !low);
        const Halo h = apply_quality(q, lit());
        CHECK(h.strength == (low ? 0.0f : 0.8f));
        CHECK(h.sun_dogs == 0.6f);
        CHECK(h.moon == 0.25f);
        CHECK(h.dispersion == 0.5f);
        CHECK(h.enabled);
    }
}

TEST_CASE("halo: r.halo 0 zeroes the strength on every tier; the cirrus layer is not affected") {
    ResetHaloCvar reset;
    halo_cvar().set_string("0");
    Cirrus cirrus;
    cirrus.coverage = 0.5f;
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        CHECK(apply_quality(quality_settings(tier), lit()).strength == 0.0f);
        if (tier != QualityTier::Low) CHECK(apply_quality(quality_settings(tier), cirrus).coverage == 0.5f);
    }
}

TEST_CASE("halo: enabled passes through") {
    ResetHaloCvar reset;
    Halo h = lit();
    h.enabled = false;
    CHECK_FALSE(apply_quality(quality_settings(QualityTier::High), h).enabled);
}

TEST_CASE("halo: apply_quality clamps every field and sanitizes NaN") {
    ResetHaloCvar reset;
    const QualitySettings q = quality_settings(QualityTier::High);
    Halo h;
    h.strength = 3.0f;
    h.sun_dogs = 2.0f;
    h.moon = 9.0f;
    h.dispersion = 5.0f;
    Halo out = apply_quality(q, h);
    CHECK(out.strength == 1.0f);
    CHECK(out.sun_dogs == 1.0f);
    CHECK(out.moon == 1.0f);
    CHECK(out.dispersion == 1.0f);

    h.strength = -1.0f;
    h.sun_dogs = -1.0f;
    h.moon = -1.0f;
    h.dispersion = -1.0f;
    out = apply_quality(q, h);
    CHECK(out.strength == 0.0f);
    CHECK(out.sun_dogs == 0.0f);
    CHECK(out.moon == 0.0f);
    CHECK(out.dispersion == 0.0f);

    h.strength = kNaN;
    h.sun_dogs = kNaN;
    h.moon = kNaN;
    h.dispersion = kNaN;
    out = apply_quality(q, h);
    CHECK(out.strength == 0.0f);
    CHECK(out.sun_dogs == 1.0f);
    CHECK(out.moon == 0.5f);
    CHECK(out.dispersion == 1.0f);

    h.strength = INFINITY;
    CHECK(std::isfinite(apply_quality(q, h).strength));
}
