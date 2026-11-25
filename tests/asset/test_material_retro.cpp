// M-HEADLIGHT 1: the retro-reflection factor quantized into MaterialChunk::flags bits 24..31.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "mce/asset/formats.h"
#include "mce/asset/material_retro.h"
#include "mce/asset/material_translucency.h"

using namespace mce;
using namespace mce::asset;

namespace {
MaterialChunk make(u32 flags = 0) {
    MaterialChunk m{};
    m.flags = flags;
    return m;
}
}  // namespace

TEST_CASE("retro: every 8-bit level round trips and the mask is bits 24..31 only") {
    CHECK(MaterialFlags::RetroMask == 0xFF000000u);
    CHECK((MaterialFlags::RetroMask & (MaterialFlags::TranslucencyMask | MaterialFlags::WrapMask)) == 0u);
    for (u32 q = 0; q <= 255; ++q) {
        MaterialChunk m = make();
        set_material_retro(m, dequantize_unit(q));
        CHECK(((m.flags & MaterialFlags::RetroMask) >> MaterialFlags::RetroShift) == q);
        CHECK(material_retro(m) == doctest::Approx(static_cast<f32>(q) / 255.0f));
    }
}

TEST_CASE("retro: values clamp to 0..1, NaN reads 0") {
    MaterialChunk m = make();
    set_material_retro(m, 1.7f);
    CHECK(material_retro(m) == 1.0f);
    set_material_retro(m, -0.4f);
    CHECK(material_retro(m) == 0.0f);
    set_material_retro(m, std::numeric_limits<f32>::quiet_NaN());
    CHECK(material_retro(m) == 0.0f);
    set_material_retro(m, 0.5f);
    CHECK(material_retro(m) == doctest::Approx(0.5f).epsilon(0.005));
}

TEST_CASE("retro: writing leaves every other bit, and translucency and wrap never touch it") {
    const u32 other = ~MaterialFlags::RetroMask;
    MaterialChunk m = make(other);
    set_material_retro(m, 0.6f);
    CHECK((m.flags & other) == other);
    CHECK(material_retro(m) > 0.0f);
    set_material_retro(m, 0.0f);
    CHECK(m.flags == other);

    MaterialChunk t = make();
    set_material_retro(t, 0.25f);
    const f32 r = material_retro(t);
    set_material_translucency(t, 1.0f, 1.0f);
    CHECK(material_retro(t) == r);
    set_material_translucency(t, 0.0f, 0.0f);
    CHECK(material_retro(t) == r);
}

TEST_CASE("retro: a material cooked before reads 0") {
    const MaterialChunk m = make(MaterialFlags::AlphaBlend | MaterialFlags::DoubleSided | MaterialFlags::OcclusionInMR |
                                 MaterialFlags::TranslucencyMask | MaterialFlags::WrapMask);
    CHECK(material_retro(m) == 0.0f);
    byte payload[sizeof(MaterialChunk)] = {};
    auto r = MaterialChunk::read(Span<const byte>(payload, sizeof(payload)));
    REQUIRE(r.has_value());
    CHECK(material_retro(*r.value()) == 0.0f);
}
