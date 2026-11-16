// M-LENSRAIN 42b: the pure lens rain helpers (wetness curve, frame, hash) and how the quality tiers and `r.lens_rain`
// map onto PostFx::rain. No GPU.

#include <doctest/doctest.h>

#include <limits>

#include "mce/convar.h"
#include "mce/render/lens_rain.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
constexpr f32 kInf = std::numeric_limits<f32>::infinity();

LensRain on() {
    LensRain r;
    r.enabled = true;
    r.amount = 0.5f;
    return r;
}

ConVar& rain_cvar() {
    ConVar* cv = ConVar::find(StringId("r.lens_rain"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetRainCvar {
    ~ResetRainCvar() { rain_cvar().reset(); }
};

PostFx applied(QualityTier tier, const LensRain& rain) {
    PostFx post;
    post.rain = rain;
    ShadowSettings shadows;
    apply_quality(quality_settings(tier), shadows, post);
    return post;
}

}  // namespace

TEST_CASE("lens rain: the hash goldens") {
    // Computed by the implementation and cross-checked with an independent Python port; the GLSL port must reproduce these
    // constant for constant. (0, 0, 0) hashes to 0: the lowbias32 mix has 0 as a fixed point.
    CHECK(lens_rain_hash(0, 0, 0) == 0x00000000u);
    CHECK(lens_rain_hash(1, 0, 0) == 0x24009c6du);
    CHECK(lens_rain_hash(0, 1, 0) == 0xae6f80f1u);
    CHECK(lens_rain_hash(0, 0, 1) == 0x688990c0u);
    CHECK(lens_rain_hash(1, 1, 1) == 0x7ed82d9bu);
    CHECK(lens_rain_hash(5, 7, 1) == 0x38f584cdu);
    CHECK(lens_rain_hash(123, 456, 789) == 0x070c359cu);
    CHECK(lens_rain_hash(0xffffffffu, 0xffffffffu, 0xffffffffu) == 0x4d764214u);
}

TEST_CASE("lens rain: the hash depends on every input and is stable") {
    const u32 h = lens_rain_hash(3, 4, 5);
    CHECK(h == lens_rain_hash(3, 4, 5));
    CHECK(h != lens_rain_hash(4, 4, 5));
    CHECK(h != lens_rain_hash(3, 5, 5));
    CHECK(h != lens_rain_hash(3, 4, 6));
    CHECK(lens_rain_hash(3, 4, 5) != lens_rain_hash(4, 3, 5));  // not symmetric in the cell
}

TEST_CASE("lens rain: the wetness curve rises toward the intensity") {
    // A dry lens in steady rain: linear at 1/4 per second, so 4 s of rain at intensity 1 is fully wet.
    CHECK(lens_rain_wetness_step(0.0f, true, 1.0f, 1.0f) == doctest::Approx(0.25f));
    f32 w = 0.0f;
    for (int i = 0; i < 40; ++i) w = lens_rain_wetness_step(w, true, 1.0f, 0.1f);
    CHECK(w == doctest::Approx(1.0f));
    // It stops at the intensity.
    w = 0.0f;
    for (int i = 0; i < 100; ++i) w = lens_rain_wetness_step(w, true, 0.4f, 0.1f);
    CHECK(w == 0.4f);
    // Monotone while raining below the intensity.
    f32 prev = 0.0f;
    w = 0.0f;
    for (int i = 0; i < 30; ++i) {
        w = lens_rain_wetness_step(w, true, 1.0f, 0.1f);
        CHECK(w >= prev);
        prev = w;
    }
    // A wetter lens than the rain's intensity dries toward it, never below.
    CHECK(lens_rain_wetness_step(1.0f, true, 0.5f, 1.0f) == doctest::Approx(0.975f));
    CHECK(lens_rain_wetness_step(0.51f, true, 0.5f, 10.0f) == 0.5f);
}

TEST_CASE("lens rain: the wetness curve dries to 0 in about 40 s") {
    CHECK(lens_rain_wetness_step(1.0f, false, 1.0f, 1.0f) == doctest::Approx(0.975f));
    f32 w = 1.0f;
    for (int i = 0; i < 400; ++i) w = lens_rain_wetness_step(w, false, 1.0f, 0.1f);
    CHECK(w == doctest::Approx(0.0f).epsilon(1e-3));
    CHECK(w >= 0.0f);
    CHECK(lens_rain_wetness_step(1.0f, false, 1.0f, 41.0f) == 0.0f);
    CHECK(lens_rain_wetness_step(0.0f, false, 1.0f, 1.0f) == 0.0f);  // dry stays dry
    // The intensity is ignored when it is not raining.
    CHECK(lens_rain_wetness_step(0.5f, false, 0.0f, 1.0f) == lens_rain_wetness_step(0.5f, false, 1.0f, 1.0f));
}

TEST_CASE("lens rain: the wetness curve clamps and treats NaN as 0") {
    CHECK(lens_rain_wetness_step(2.0f, true, 1.0f, 0.0f) == 1.0f);   // dt 0 only clamps
    CHECK(lens_rain_wetness_step(-1.0f, true, 1.0f, -1.0f) == 0.0f); // dt < 0 only clamps
    CHECK(lens_rain_wetness_step(0.3f, true, 1.0f, 0.0f) == 0.3f);
    CHECK(lens_rain_wetness_step(0.3f, false, 1.0f, -5.0f) == 0.3f);
    CHECK(lens_rain_wetness_step(0.3f, true, 1.0f, kNaN) == 0.3f);
    CHECK(lens_rain_wetness_step(kNaN, true, 1.0f, 1.0f) == doctest::Approx(0.25f));  // NaN wetness = 0
    CHECK(lens_rain_wetness_step(kNaN, false, 1.0f, 1.0f) == 0.0f);
    CHECK(lens_rain_wetness_step(0.5f, true, kNaN, 1.0f) == doctest::Approx(0.475f));  // NaN intensity = 0: dries toward 0
    CHECK(lens_rain_wetness_step(0.0f, true, kNaN, 1.0f) == 0.0f);
    CHECK(lens_rain_wetness_step(0.0f, true, 5.0f, 100.0f) == 1.0f);  // intensity clamped to 1
    CHECK(lens_rain_wetness_step(0.0f, true, -5.0f, 100.0f) == 0.0f);
    CHECK(lens_rain_wetness_step(0.5f, true, 1.0f, kInf) == 0.5f);
    for (f32 w : {-3.0f, 0.0f, 0.5f, 1.0f, 3.0f, kNaN}) {
        for (bool raining : {false, true}) {
            const f32 r = lens_rain_wetness_step(w, raining, 0.7f, 0.37f);
            CHECK(r >= 0.0f);
            CHECK(r <= 1.0f);
        }
    }
}

TEST_CASE("lens rain: the frame passes the setting through") {
    LensRain r = on();
    r.amount = 0.75f;
    r.density = 2.0f;
    r.size = 1.5f;
    r.refraction = 0.5f;
    r.blur = 0.2f;
    r.trail = 0.9f;
    r.fall_speed = 1.0f;
    r.seed = 77;
    const LensRainFrame f = lens_rain_frame(r, 12.5f, 1.77f);
    CHECK(f.amount == 0.75f);
    CHECK(f.density == 2.0f);
    CHECK(f.size == 1.5f);
    CHECK(f.refraction == 0.5f);
    CHECK(f.blur == 0.2f);
    CHECK(f.trail == 0.9f);
    CHECK(f.fall_speed == 1.0f);
    CHECK(f.time == 12.5f);
    CHECK(f.aspect == 1.77f);
    CHECK(f.seed == 77u);
}

TEST_CASE("lens rain: the frame amount is exactly 0 when the pass must skip") {
    LensRain r = on();
    r.amount = 1.0f;
    CHECK(lens_rain_frame(r, 0.0f, 1.0f).amount == 1.0f);
    r.enabled = false;
    CHECK(lens_rain_frame(r, 0.0f, 1.0f).amount == 0.0f);
    r = on();
    r.amount = 0.0f;
    CHECK(lens_rain_frame(r, 0.0f, 1.0f).amount == 0.0f);
    r.amount = -1.0f;
    CHECK(lens_rain_frame(r, 0.0f, 1.0f).amount == 0.0f);
    r.amount = kNaN;
    CHECK(lens_rain_frame(r, 0.0f, 1.0f).amount == 0.0f);
    r.amount = 5.0f;
    CHECK(lens_rain_frame(r, 0.0f, 1.0f).amount == 1.0f);
    // The default setting is disabled.
    CHECK(lens_rain_frame(LensRain{}, 1.0f, 1.0f).amount == 0.0f);
}

TEST_CASE("lens rain: the frame clamps its fields and sanitizes time and aspect") {
    LensRain r = on();
    r.density = 100.0f;
    r.size = 0.0f;
    r.refraction = 100.0f;
    r.blur = -1.0f;
    r.trail = 100.0f;
    r.fall_speed = -1.0f;
    LensRainFrame f = lens_rain_frame(r, 1.0f, 1.0f);
    CHECK(f.density == 4.0f);
    CHECK(f.size == 0.25f);
    CHECK(f.refraction == 2.0f);
    CHECK(f.blur == 0.0f);
    CHECK(f.trail == 1.0f);
    CHECK(f.fall_speed == 0.0f);

    r.density = r.size = r.refraction = r.blur = r.trail = r.fall_speed = kNaN;
    f = lens_rain_frame(r, 1.0f, 1.0f);
    const LensRain d;
    CHECK(f.density == d.density);
    CHECK(f.size == d.size);
    CHECK(f.refraction == d.refraction);
    CHECK(f.blur == d.blur);
    CHECK(f.trail == d.trail);
    CHECK(f.fall_speed == d.fall_speed);

    CHECK(lens_rain_frame(on(), kNaN, 1.0f).time == 0.0f);
    CHECK(lens_rain_frame(on(), kInf, 1.0f).time == 0.0f);
    CHECK(lens_rain_frame(on(), 3.0f, kNaN).aspect == 1.0f);
    CHECK(lens_rain_frame(on(), 3.0f, 0.0f).aspect == 1.0f);
    CHECK(lens_rain_frame(on(), 3.0f, -2.0f).aspect == 1.0f);
    CHECK(lens_rain_frame(on(), 3.0f, kInf).aspect == 1.0f);
}

TEST_CASE("lens rain: the frame is deterministic") {
    LensRain r = on();
    r.seed = 9;
    const LensRainFrame a = lens_rain_frame(r, 4.25f, 1.5f);
    const LensRainFrame b = lens_rain_frame(r, 4.25f, 1.5f);
    CHECK(a.amount == b.amount);
    CHECK(a.time == b.time);
    CHECK(a.seed == b.seed);
    CHECK(a.aspect == b.aspect);
}

TEST_CASE("lens rain: the quality tiers") {
    const QualitySettings low = quality_settings(QualityTier::Low);
    const QualitySettings med = quality_settings(QualityTier::Medium);
    const QualitySettings high = quality_settings(QualityTier::High);
    CHECK_FALSE(low.lens_rain);
    CHECK(low.lens_rain_layers == 0u);
    CHECK(med.lens_rain);
    CHECK(med.lens_rain_layers == 1u);
    CHECK(high.lens_rain);
    CHECK(high.lens_rain_layers == 2u);
}

TEST_CASE("lens rain: apply_quality honors the tier and r.lens_rain") {
    ResetRainCvar reset;
    const LensRain r = on();
    CHECK_FALSE(applied(QualityTier::Low, r).rain.enabled);
    CHECK(applied(QualityTier::Medium, r).rain.enabled);
    CHECK(applied(QualityTier::High, r).rain.enabled);
    // A disabled setting stays disabled.
    CHECK_FALSE(applied(QualityTier::High, LensRain{}).rain.enabled);

    rain_cvar().set_string("0");
    for (QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High}) {
        INFO(quality_tier_name(tier));
        CHECK_FALSE(applied(tier, r).rain.enabled);
    }
    rain_cvar().set_string("1");
    CHECK(applied(QualityTier::High, r).rain.enabled);
}

TEST_CASE("lens rain: apply_quality clamps the ranges and sanitizes NaN") {
    ResetRainCvar reset;
    LensRain r = on();
    r.amount = 5.0f;
    r.density = 100.0f;
    r.size = 0.0f;
    r.refraction = 100.0f;
    r.blur = 5.0f;
    r.trail = 5.0f;
    r.fall_speed = 100.0f;
    LensRain c = applied(QualityTier::High, r).rain;
    CHECK(c.enabled);
    CHECK(c.amount == 1.0f);
    CHECK(c.density == 4.0f);
    CHECK(c.size == 0.25f);
    CHECK(c.refraction == 2.0f);
    CHECK(c.blur == 1.0f);
    CHECK(c.trail == 1.0f);
    CHECK(c.fall_speed == 2.0f);

    r.amount = -1.0f;
    r.density = -1.0f;
    r.size = 100.0f;
    r.refraction = -1.0f;
    r.blur = -1.0f;
    r.trail = -1.0f;
    r.fall_speed = -1.0f;
    c = applied(QualityTier::High, r).rain;
    CHECK(c.amount == 0.0f);
    CHECK(c.density == 0.0f);
    CHECK(c.size == 4.0f);
    CHECK(c.refraction == 0.0f);
    CHECK(c.blur == 0.0f);
    CHECK(c.trail == 0.0f);
    CHECK(c.fall_speed == 0.0f);

    // NaN falls back to the default.
    r = on();
    r.amount = r.density = r.size = r.refraction = r.blur = r.trail = r.fall_speed = kNaN;
    c = applied(QualityTier::High, r).rain;
    const LensRain d;
    CHECK(c.amount == d.amount);
    CHECK(c.density == d.density);
    CHECK(c.size == d.size);
    CHECK(c.refraction == d.refraction);
    CHECK(c.blur == d.blur);
    CHECK(c.trail == d.trail);
    CHECK(c.fall_speed == d.fall_speed);
}

TEST_CASE("lens rain: in-range values and the default pass through apply_quality unchanged") {
    ResetRainCvar reset;
    const LensRain d;
    const LensRain c = applied(QualityTier::High, LensRain{}).rain;
    CHECK(c.amount == d.amount);
    CHECK(c.density == d.density);
    CHECK(c.size == d.size);
    CHECK(c.refraction == d.refraction);
    CHECK(c.blur == d.blur);
    CHECK(c.trail == d.trail);
    CHECK(c.fall_speed == d.fall_speed);
    CHECK(c.seed == d.seed);
}
