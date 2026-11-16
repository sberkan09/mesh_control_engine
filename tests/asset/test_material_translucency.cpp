// M-TRANSLUCENT 41a: the translucency and wrap factors quantized into MaterialChunk::flags.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "mce/asset/formats.h"
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

TEST_CASE("The reserved bits sit above the used ones and do not overlap") {
    static_assert((MaterialFlags::TranslucencyMask & MaterialFlags::WrapMask) == 0);
    static_assert(MaterialFlags::TranslucencyMask == 0x0000FF00u && MaterialFlags::WrapMask == 0x00FF0000u);
    constexpr u32 used = MaterialFlags::AlphaBlend | MaterialFlags::AlphaMask | MaterialFlags::DoubleSided | MaterialFlags::Unlit |
                         MaterialFlags::OcclusionInMR | MaterialFlags::OcclusionOnly;
    static_assert((used & (MaterialFlags::TranslucencyMask | MaterialFlags::WrapMask)) == 0);
    CHECK(MaterialFlags::TranslucencyShift == 8u);
    CHECK(MaterialFlags::WrapShift == 16u);
}

TEST_CASE("quantize_unit and dequantize_unit round trip over 0..255") {
    for (u32 q = 0; q <= 255; ++q) {
        const f32 v = dequantize_unit(q);
        CHECK(quantize_unit(v) == q);
    }
    CHECK(dequantize_unit(0) == 0.0f);
    CHECK(dequantize_unit(255) == 1.0f);
    CHECK(quantize_unit(0.0f) == 0u);
    CHECK(quantize_unit(1.0f) == 255u);
    CHECK(quantize_unit(0.5f) == 128u);
}

TEST_CASE("quantize_unit clamps and sends NaN to 0") {
    CHECK(quantize_unit(-1.0f) == 0u);
    CHECK(quantize_unit(-std::numeric_limits<f32>::infinity()) == 0u);
    CHECK(quantize_unit(2.0f) == 255u);
    CHECK(quantize_unit(1e30f) == 255u);
    CHECK(quantize_unit(std::numeric_limits<f32>::infinity()) == 255u);
    CHECK(quantize_unit(std::numeric_limits<f32>::quiet_NaN()) == 0u);
}

TEST_CASE("set_material_translucency writes both fields and reads them back") {
    MaterialChunk m = make();
    set_material_translucency(m, 0.5f, 1.0f);
    CHECK(m.flags == ((128u << MaterialFlags::TranslucencyShift) | (255u << MaterialFlags::WrapShift)));
    CHECK(material_translucency(m) == doctest::Approx(128.0f / 255.0f));
    CHECK(material_wrap(m) == 1.0f);
    CHECK(translucency_bits(m.flags) == m.flags);
    set_material_translucency(m, 0.0f, 0.25f);
    CHECK(material_translucency(m) == 0.0f);
    CHECK(material_wrap(m) == doctest::Approx(64.0f / 255.0f).epsilon(0.01));
    // Out of range and NaN go through the clamp.
    set_material_translucency(m, 7.0f, std::numeric_limits<f32>::quiet_NaN());
    CHECK(material_translucency(m) == 1.0f);
    CHECK(material_wrap(m) == 0.0f);
}

TEST_CASE("set_material_translucency leaves every other flags bit") {
    const u32 other = ~(MaterialFlags::TranslucencyMask | MaterialFlags::WrapMask);
    MaterialChunk m = make(other);
    set_material_translucency(m, 0.7f, 0.3f);
    CHECK((m.flags & other) == other);
    CHECK(material_translucency(m) > 0.0f);
    set_material_translucency(m, 0.0f, 0.0f);
    CHECK(m.flags == other);
    CHECK(translucency_bits(m.flags) == 0u);

    MaterialChunk d = make(MaterialFlags::DoubleSided | MaterialFlags::AlphaMask | (1u << 6) | (1u << 24) | (1u << 31));
    const u32 before = d.flags;
    set_material_translucency(d, 1.0f, 1.0f);
    CHECK((d.flags & other) == before);
}

TEST_CASE("A material cooked before the seam reads 0, 0") {
    const MaterialChunk m = make(MaterialFlags::AlphaBlend | MaterialFlags::DoubleSided | MaterialFlags::OcclusionInMR);
    CHECK(material_translucency(m) == 0.0f);
    CHECK(material_wrap(m) == 0.0f);
    CHECK(translucency_bits(m.flags) == 0u);
    // Through the real reader: a zeroed payload of the chunk's size.
    byte payload[sizeof(MaterialChunk)] = {};
    auto r = MaterialChunk::read(Span<const byte>(payload, sizeof(payload)));
    REQUIRE(r.has_value());
    CHECK(material_translucency(*r.value()) == 0.0f);
    CHECK(material_wrap(*r.value()) == 0.0f);
}
