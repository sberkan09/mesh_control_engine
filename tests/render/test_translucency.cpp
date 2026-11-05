// M-TRANSLUCENT 41b: the pure translucency and wrap helpers (the reference the 41d and 41e shaders mirror), the packing of
// ObjectU::o_skin.w, the per-frame switches, and the quality tiers and `r.translucency`. No GPU.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "mce/asset/formats.h"
#include "mce/asset/material_translucency.h"
#include "mce/convar.h"
#include "mce/render/quality.h"
#include "mce/render/translucency.h"

using namespace mce;
using namespace mce::render;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();

ConVar& translucency_cvar() {
    ConVar* cv = ConVar::find(StringId("r.translucency"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetCvar {
    ~ResetCvar() { translucency_cvar().reset(); }
};

u32 flags_of(u32 t8, u32 w8) {
    return (t8 << asset::MaterialFlags::TranslucencyShift) | (w8 << asset::MaterialFlags::WrapShift);
}

}  // namespace

TEST_CASE("translucency constants") {
    CHECK(kTranslucencyDistortion == 0.2f);
    CHECK(kTranslucencyPower == 4.0f);
    CHECK(kTranslucencyScatter == 0.5f);
}

TEST_CASE("wrap_diffuse") {
    // wrap 0 is the clamped Lambert, exactly.
    for (f32 x = -1.0f; x <= 1.0f; x += 0.125f) CHECK(wrap_diffuse(x, 0.0f) == (x > 0.0f ? x : 0.0f));
    // zero at x = -wrap, one at x = 1.
    for (f32 w : {0.25f, 0.5f, 1.0f}) {
        CHECK(wrap_diffuse(-w, w) == 0.0f);
        CHECK(wrap_diffuse(-1.0f, w) == 0.0f);
        CHECK(wrap_diffuse(1.0f, w) == doctest::Approx(1.0f / (1.0f + w)).epsilon(1e-6));
    }
    CHECK(wrap_diffuse(0.0f, 0.5f) == doctest::Approx(0.5f / 2.25f));
    // Energy: the integral over the sphere (uniform in cos theta) equals Lambert's 0.5 within 1e-4.
    for (f32 w : {0.0f, 0.3f, 0.5f, 1.0f}) {
        constexpr int kN = 200000;
        double sum = 0.0;
        for (int i = 0; i < kN; ++i) sum += wrap_diffuse(-1.0f + 2.0f * (static_cast<f32>(i) + 0.5f) / kN, w);
        const double integral = sum / kN * 2.0;  // over x in [-1, 1]
        CHECK(std::fabs(integral - 0.5) < 1e-4);  // Lambert: the integral of max(x, 0) is 0.5
    }
    // Out-of-range wrap is clamped, NaN is 0.
    CHECK(wrap_diffuse(0.5f, 7.0f) == wrap_diffuse(0.5f, 1.0f));
    CHECK(wrap_diffuse(0.5f, -3.0f) == 0.5f);
    CHECK(wrap_diffuse(0.5f, kNaN) == 0.5f);
    CHECK(wrap_diffuse(kNaN, 0.5f) == 0.0f);
}

TEST_CASE("wrap_extra") {
    for (f32 x = -1.0f; x <= 1.0f; x += 0.125f) CHECK(wrap_extra(x, 0.0f) == 0.0f);
    CHECK(wrap_extra(1.0f, 0.5f) < 0.0f);   // the lit side loses
    CHECK(wrap_extra(-0.25f, 0.5f) > 0.0f);  // the band past the terminator gains
    CHECK(wrap_extra(-0.5f, 0.5f) == 0.0f);
    CHECK(wrap_extra(-0.9f, 0.5f) == 0.0f);
    // plain Lambert + extra == wrap_diffuse
    for (f32 x = -1.0f; x <= 1.0f; x += 0.0625f)
        CHECK(((x > 0.0f ? x : 0.0f) + wrap_extra(x, 0.4f)) == doctest::Approx(wrap_diffuse(x, 0.4f)).epsilon(1e-6));
    CHECK(wrap_extra(kNaN, 0.5f) == 0.0f);
}

TEST_CASE("translucency_back") {
    // front side, no lobe: 0
    CHECK(translucency_back(0.7f, 0.9f, false) == 0.0f);
    CHECK(translucency_back(0.0f, 0.9f, false) == 0.0f);
    // back side: -n.l
    CHECK(translucency_back(-0.6f, 1.0f, false) == doctest::Approx(0.6f));
    CHECK(translucency_back(-1.0f, -1.0f, false) == 1.0f);
    // the lobe: peak toward the light (v.t = 1), 0 away, monotone in v.t
    CHECK(translucency_back(0.5f, 1.0f, true) == doctest::Approx(kTranslucencyScatter));
    CHECK(translucency_back(-0.5f, 1.0f, true) == doctest::Approx(0.5f + kTranslucencyScatter));
    CHECK(translucency_back(0.5f, -0.5f, true) == 0.0f);
    CHECK(translucency_back(0.5f, 0.5f, true) == doctest::Approx(kTranslucencyScatter * std::pow(0.5f, kTranslucencyPower)));
    CHECK(translucency_back(0.5f, 0.9f, true) > translucency_back(0.5f, 0.6f, true));
    // scatter off drops the lobe
    CHECK(translucency_back(-0.5f, 1.0f, false) == 0.5f);
    CHECK(translucency_back(kNaN, kNaN, true) == 0.0f);
}

TEST_CASE("translucency_object packing") {
    const QualitySettings q = quality_settings(QualityTier::High);
    ResetCvar reset;
    CHECK(translucency_object(0, false, q, true) == 0.0f);
    CHECK(translucency_object(flags_of(1, 0), false, q, true) == 1.0f);
    CHECK(translucency_object(flags_of(0, 1), false, q, true) == 256.0f);
    CHECK(translucency_object(flags_of(255, 255), false, q, true) == 65535.0f);
    // other flag bits do not leak in
    CHECK(translucency_object(flags_of(10, 20) | 0x3Fu | 0xFF000000u, false, q, true) == 10.0f + 256.0f * 20.0f);
    // every pair round-trips exactly through the shader's mod / floor decode
    for (u32 t = 0; t < 256; ++t) {
        for (u32 w = 0; w < 256; ++w) {
            const f32 v = translucency_object(flags_of(t, w), false, q, true);
            REQUIRE(v == static_cast<f32>(t + 256u * w));
            const f32 t_dec = std::fmod(v, 256.0f);
            const f32 w_dec = std::floor(v * (1.0f / 256.0f));
            REQUIRE(t_dec == static_cast<f32>(t));
            REQUIRE(w_dec == static_cast<f32>(w));
        }
    }
    // agrees with the asset accessors
    asset::MaterialChunk m{};
    asset::set_material_translucency(m, 0.5f, 1.0f);
    const f32 v = translucency_object(m.flags, false, q, true);
    CHECK(std::fmod(v, 256.0f) / 255.0f == doctest::Approx(asset::material_translucency(m)));
    CHECK(std::floor(v / 256.0f) / 255.0f == doctest::Approx(asset::material_wrap(m)));
}

TEST_CASE("translucency_object off cases") {
    ResetCvar reset;
    const QualitySettings q = quality_settings(QualityTier::High);
    const u32 f = flags_of(200, 100);
    CHECK(translucency_object(f, false, q, true) > 0.0f);
    CHECK(translucency_object(f, true, q, true) == 0.0f);   // unlit
    CHECK(translucency_object(f, false, q, false) == 0.0f);  // Forward3DDesc::translucency false
    QualitySettings off = q;
    off.translucency = false;
    CHECK(translucency_object(f, false, off, true) == 0.0f);
    translucency_cvar().set_bool(false);
    CHECK(translucency_object(f, false, q, true) == 0.0f);
    translucency_cvar().reset();
    CHECK(translucency_object(f, false, q, true) > 0.0f);
}

TEST_CASE("translucency tiers and cvar") {
    ResetCvar reset;
    const QualitySettings lo = quality_settings(QualityTier::Low);
    const QualitySettings med = quality_settings(QualityTier::Medium);
    const QualitySettings hi = quality_settings(QualityTier::High);
    for (const QualitySettings* q : {&lo, &med, &hi}) CHECK(q->translucency);
    CHECK_FALSE(lo.translucency_ambient);
    CHECK_FALSE(lo.translucency_scatter);
    CHECK(med.translucency_ambient);
    CHECK(med.translucency_scatter);
    CHECK(hi.translucency_ambient);
    CHECK(hi.translucency_scatter);

    TranslucencyFrame f = translucency_frame(lo);
    CHECK_FALSE(f.ambient);
    CHECK_FALSE(f.scatter);
    f = translucency_frame(med);
    CHECK(f.ambient);
    CHECK(f.scatter);
    f = translucency_frame(hi);
    CHECK(f.ambient);
    CHECK(f.scatter);

    // Low still honors the material (the value is not 0), only the extras are dropped
    CHECK(translucency_object(flags_of(255, 0), false, lo, true) == 255.0f);

    QualitySettings off = hi;
    off.translucency = false;
    f = translucency_frame(off);
    CHECK_FALSE(f.ambient);
    CHECK_FALSE(f.scatter);

    CHECK(translucency_cvar().get_bool());
    translucency_cvar().set_bool(false);
    f = translucency_frame(hi);
    CHECK_FALSE(f.ambient);
    CHECK_FALSE(f.scatter);
    CHECK_FALSE(translucency_cvar_on());
}
