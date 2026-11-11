// M-CIRRUS 53a: the Cirrus carrier (the high streak layer behind the Clouds layer) and how the quality tiers and
// `r.cirrus` map onto it. Data only: nothing draws it yet (the sky shader is 53b). No GPU.

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

static_assert(sizeof(Cirrus) == 36, "Cirrus has no implicit padding: five f32, a vec2, an f32, bool + pad_[3]");

ConVar& cirrus_cvar() {
    ConVar* cv = ConVar::find(StringId("r.cirrus"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetCirrusCvar {
    ~ResetCirrusCvar() { cirrus_cvar().reset(); }
};

Cirrus lit() {
    Cirrus c;
    c.coverage = 0.6f;
    c.density = 0.4f;
    c.stretch = 8.0f;
    c.speed = 3.0f;
    return c;
}

bool same_bytes(const Cirrus& a, const Cirrus& b) { return std::memcmp(&a, &b, sizeof(Cirrus)) == 0; }

}  // namespace

TEST_CASE("cirrus: the default carrier is off, and so is a fresh Scene3D") {
    const Cirrus d;
    CHECK(d.coverage == 0.0f);
    CHECK(d.density == 0.35f);
    CHECK(d.scale_m == 3000.0f);
    CHECK(d.stretch == 6.0f);
    CHECK(d.speed == 2.0f);
    CHECK(d.wind_dir.x == 1.0f);
    CHECK(d.wind_dir.y == 0.0f);
    CHECK(d.height_m == 9000.0f);
    CHECK(d.enabled);
    for (u8 b : d.pad_) CHECK(b == 0);
    MeshStore store;
    Scene3D scene(store, mem::heap());
    CHECK(same_bytes(scene.cirrus(), Cirrus{}));
}

TEST_CASE("cirrus: the defaults pass apply_quality bit for bit on every tier") {
    ResetCirrusCvar reset;
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        CHECK(same_bytes(apply_quality(quality_settings(tier), Cirrus{}), Cirrus{}));
    }
    const Cirrus in = lit();  // in range: only the tier's gate may change it
    CHECK(same_bytes(apply_quality(quality_settings(QualityTier::High), in), in));
}

TEST_CASE("cirrus: Scene3D set/get round trip") {
    MeshStore store;
    Scene3D scene(store, mem::heap());
    Cirrus c = lit();
    c.scale_m = 5000.0f;
    c.wind_dir = math::vec2{0.0f, 1.0f};
    c.height_m = 12000.0f;
    c.enabled = false;
    scene.set_cirrus(c);
    CHECK(same_bytes(scene.cirrus(), c));
    CHECK(scene.clouds().coverage == 0.0f);  // the low layer is its own carrier
}

TEST_CASE("cirrus: Low zeroes the coverage; Medium and High keep it") {
    ResetCirrusCvar reset;
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        const bool low = tier == QualityTier::Low;
        const QualitySettings q = quality_settings(tier);
        CHECK(q.cirrus == !low);
        const Cirrus c = apply_quality(q, lit());
        CHECK(c.coverage == (low ? 0.0f : 0.6f));
        CHECK(c.density == 0.4f);
        CHECK(c.stretch == 8.0f);
        CHECK(c.speed == 3.0f);
        CHECK(c.enabled);
    }
}

TEST_CASE("cirrus: r.cirrus 0 zeroes the coverage on every tier; the low layer is not affected") {
    ResetCirrusCvar reset;
    cirrus_cvar().set_string("0");
    Clouds low;
    low.coverage = 0.5f;
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        CHECK(apply_quality(quality_settings(tier), lit()).coverage == 0.0f);
        if (tier != QualityTier::Low) CHECK(apply_quality(quality_settings(tier), low).coverage == 0.5f);
    }
}

TEST_CASE("cirrus: enabled passes through") {
    ResetCirrusCvar reset;
    Cirrus c = lit();
    c.enabled = false;
    CHECK_FALSE(apply_quality(quality_settings(QualityTier::High), c).enabled);
}

TEST_CASE("cirrus: apply_quality clamps every field and sanitizes NaN") {
    ResetCirrusCvar reset;
    const QualitySettings q = quality_settings(QualityTier::High);
    Cirrus c;
    c.coverage = 3.0f;
    c.density = 2.0f;
    c.scale_m = 1.0e6f;
    c.stretch = 100.0f;
    c.speed = 99.0f;
    c.height_m = 1.0e6f;
    Cirrus out = apply_quality(q, c);
    CHECK(out.coverage == 1.0f);
    CHECK(out.density == 1.0f);
    CHECK(out.scale_m == 40000.0f);
    CHECK(out.stretch == 16.0f);
    CHECK(out.speed == 10.0f);
    CHECK(out.height_m == 30000.0f);

    c.coverage = -1.0f;
    c.density = -1.0f;
    c.scale_m = 1.0f;
    c.stretch = 0.0f;
    c.speed = -5.0f;
    c.height_m = 10.0f;
    out = apply_quality(q, c);
    CHECK(out.coverage == 0.0f);
    CHECK(out.density == 0.0f);
    CHECK(out.scale_m == 200.0f);
    CHECK(out.stretch == 1.0f);
    CHECK(out.speed == 0.0f);
    CHECK(out.height_m == 1000.0f);

    c.coverage = kNaN;
    c.density = kNaN;
    c.scale_m = kNaN;
    c.stretch = kNaN;
    c.speed = kNaN;
    c.wind_dir = math::vec2{kNaN, kNaN};
    c.height_m = kNaN;
    out = apply_quality(q, c);
    CHECK(out.coverage == 0.0f);
    CHECK(out.density == 0.35f);
    CHECK(out.scale_m == 3000.0f);
    CHECK(out.stretch == 6.0f);
    CHECK(out.speed == 2.0f);
    CHECK(out.wind_dir.x == 1.0f);
    CHECK(out.wind_dir.y == 0.0f);
    CHECK(out.height_m == 9000.0f);
}

TEST_CASE("cirrus: wind_dir is normalized, a zero or infinite vector falls back to +x") {
    ResetCirrusCvar reset;
    const QualitySettings q = quality_settings(QualityTier::High);
    Cirrus c = lit();
    c.wind_dir = math::vec2{3.0f, 4.0f};
    Cirrus out = apply_quality(q, c);
    CHECK(out.wind_dir.x == doctest::Approx(0.6f));
    CHECK(out.wind_dir.y == doctest::Approx(0.8f));
    c.wind_dir = math::vec2{0.0f, 0.0f};
    out = apply_quality(q, c);
    CHECK(out.wind_dir.x == 1.0f);
    CHECK(out.wind_dir.y == 0.0f);
    c.wind_dir = math::vec2{INFINITY, 0.0f};
    out = apply_quality(q, c);
    CHECK(out.wind_dir.x == 1.0f);
    CHECK(out.wind_dir.y == 0.0f);
}
