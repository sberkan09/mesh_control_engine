// M-CAUSTICS 52a: the Caustics setting's defaults, the Forward3DStats fields, the quality tiers and how `r.caustics`
// and apply_quality's clamps map onto PostFx::caustics. Nothing draws with them yet (52b). No GPU.

#include <doctest/doctest.h>

#include <limits>

#include "mce/convar.h"
#include "mce/render/forward3d.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();

ConVar& ca_cvar() {
    ConVar* cv = ConVar::find(StringId("r.caustics"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetCaCvar {
    ~ResetCaCvar() { ca_cvar().reset(); }
};

Caustics on() {
    Caustics s;
    s.enabled = true;
    return s;
}

PostFx applied(QualityTier tier, const Caustics& s) {
    PostFx post;
    post.caustics = s;
    ShadowSettings shadows;
    apply_quality(quality_settings(tier), shadows, post);
    return post;
}

}  // namespace

TEST_CASE("Caustics defaults") {
    const Caustics s;
    CHECK_FALSE(s.enabled);
    CHECK(s.strength == 1.0f);
    CHECK(s.scale == 2.0f);
    CHECK(s.speed == 1.0f);
    CHECK(s.absorption == 0.15f);
    CHECK(s.max_depth == 30.0f);
    for (u8 b : s.pad_) CHECK(b == 0);
    static_assert(sizeof(Caustics) == 24, "Caustics has no implicit padding: bool + pad_[3] + five f32");
}

TEST_CASE("PostFx carries the default (off) caustics setting, Forward3DStats a zero one") {
    const PostFx fx{};
    CHECK_FALSE(fx.caustics.enabled);
    CHECK(fx.caustics.strength == 1.0f);
    const Forward3DStats st{};
    CHECK_FALSE(st.caustics);
    CHECK(st.caustics_strength == 0.0f);
}

TEST_CASE("caustics: the quality tiers") {
    const QualitySettings low = quality_settings(QualityTier::Low);
    const QualitySettings med = quality_settings(QualityTier::Medium);
    const QualitySettings high = quality_settings(QualityTier::High);
    CHECK_FALSE(low.caustics);
    CHECK(med.caustics);
    CHECK(high.caustics);
    CHECK(low.caustics_octaves == 0u);
    CHECK(med.caustics_octaves == 2u);
    CHECK(high.caustics_octaves == 3u);
}

TEST_CASE("caustics: apply_quality honors the tier and r.caustics") {
    ResetCaCvar reset;
    const Caustics s = on();
    CHECK_FALSE(applied(QualityTier::Low, s).caustics.enabled);
    CHECK(applied(QualityTier::Medium, s).caustics.enabled);
    CHECK(applied(QualityTier::High, s).caustics.enabled);
    CHECK_FALSE(applied(QualityTier::High, Caustics{}).caustics.enabled);  // off stays off
    ca_cvar().set_string("0");
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        CHECK_FALSE(applied(tier, s).caustics.enabled);
    }
    ca_cvar().set_string("1");
    CHECK(applied(QualityTier::High, s).caustics.enabled);
}

TEST_CASE("caustics: apply_quality clamps the ranges and sanitizes NaN") {
    ResetCaCvar reset;
    Caustics s = on();
    s.strength = s.scale = s.speed = s.absorption = s.max_depth = 1e6f;
    Caustics c = applied(QualityTier::High, s).caustics;
    CHECK(c.strength == 4.0f);
    CHECK(c.scale == 16.0f);
    CHECK(c.speed == 4.0f);
    CHECK(c.absorption == 4.0f);
    CHECK(c.max_depth == 200.0f);
    s.strength = s.scale = s.speed = s.absorption = s.max_depth = -1.0f;
    c = applied(QualityTier::High, s).caustics;
    CHECK(c.strength == 0.0f);
    CHECK(c.scale == 0.25f);
    CHECK(c.speed == 0.0f);
    CHECK(c.absorption == 0.0f);
    CHECK(c.max_depth == 0.5f);
    s.strength = s.scale = s.speed = s.absorption = s.max_depth = kNaN;
    c = applied(QualityTier::High, s).caustics;
    CHECK(c.enabled);
    CHECK(c.strength == 1.0f);
    CHECK(c.scale == 2.0f);
    CHECK(c.speed == 1.0f);
    CHECK(c.absorption == 0.15f);
    CHECK(c.max_depth == 30.0f);
}

TEST_CASE("caustics: in-range values pass through apply_quality unchanged") {
    ResetCaCvar reset;
    Caustics s = on();
    s.strength = 2.5f;
    s.scale = 0.5f;
    s.speed = 0.25f;
    s.absorption = 1.0f;
    s.max_depth = 12.0f;
    const Caustics c = applied(QualityTier::Medium, s).caustics;
    CHECK(c.strength == 2.5f);
    CHECK(c.scale == 0.5f);
    CHECK(c.speed == 0.25f);
    CHECK(c.absorption == 1.0f);
    CHECK(c.max_depth == 12.0f);
}
