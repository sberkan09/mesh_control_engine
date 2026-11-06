// HeatHaze3D (M-HEATHAZE 43d): the component defaults, the zeroed pad and the pure fire_follow multiplier.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>

#include "mce/kit/heat_haze.h"

using namespace mce;
using namespace mce::kit;

namespace {
bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
constexpr f32 kInf = std::numeric_limits<f32>::infinity();
}  // namespace

TEST_CASE("heat haze: component defaults and zeroed pad") {
    const HeatHaze3D c;
    CHECK(c.enabled == 1u);
    CHECK(same_bits(c.radius, 1.0f));
    CHECK(same_bits(c.strength, 0.5f));
    CHECK(same_bits(c.rise, 0.6f));
    CHECK(same_bits(c.scale, 1.0f));
    CHECK(same_bits(c.softness, 0.5f));
    CHECK(same_bits(c.offset_y, 0.0f));
    CHECK(same_bits(c.fire_follow, 0.0f));
    CHECK((c.pad_[0] | c.pad_[1] | c.pad_[2]) == 0u);
}

TEST_CASE("heat haze: fire scale is exactly 1 without follow or with unusable light numbers") {
    CHECK(same_bits(heat_haze_fire_scale(0.0f, 100.0f, 1.0f), 1.0f));
    CHECK(same_bits(heat_haze_fire_scale(-1.0f, 100.0f, 1.0f), 1.0f));
    CHECK(same_bits(heat_haze_fire_scale(kNaN, 100.0f, 1.0f), 1.0f));
    CHECK(same_bits(heat_haze_fire_scale(1.0f, kNaN, 7.0f), 1.0f));
    CHECK(same_bits(heat_haze_fire_scale(1.0f, 3.0f, kInf), 1.0f));
    CHECK(same_bits(heat_haze_fire_scale(1.0f, kInf, 7.0f), 1.0f));
}

TEST_CASE("heat haze: fire scale follows intensity per metre of range around the reference") {
    // The default PointLight3D (3 over 7 m) is the reference: unchanged at any follow.
    CHECK(std::abs(heat_haze_fire_scale(1.0f, 3.0f, 7.0f) - 1.0f) < 1e-6f);
    CHECK(std::abs(heat_haze_fire_scale(0.5f, 3.0f, 7.0f) - 1.0f) < 1e-6f);
    // Twice the reference brightness: 1 + follow; clamped there however bright.
    CHECK(std::abs(heat_haze_fire_scale(1.0f, 6.0f, 7.0f) - 2.0f) < 1e-5f);
    CHECK(std::abs(heat_haze_fire_scale(0.5f, 6.0f, 7.0f) - 1.5f) < 1e-5f);
    CHECK(same_bits(heat_haze_fire_scale(1.0f, 100.0f, 7.0f), 2.0f));
    // Dark light: down to 1 - follow.
    CHECK(same_bits(heat_haze_fire_scale(1.0f, 0.0f, 7.0f), 0.0f));
    CHECK(same_bits(heat_haze_fire_scale(0.25f, 0.0f, 7.0f), 0.75f));
    CHECK(heat_haze_fire_scale(1.0f, -5.0f, 7.0f) == 0.0f);
    // Follow above 1 counts as 1.
    CHECK(same_bits(heat_haze_fire_scale(5.0f, 100.0f, 7.0f), 2.0f));
    // A tiny range is floored at 0.1 m (no division blow-up).
    CHECK(same_bits(heat_haze_fire_scale(1.0f, 1.0f, 0.0f), 2.0f));
}

TEST_CASE("heat haze: fire scale is monotone in the intensity and pure") {
    f32 prev = -1.0f;
    for (int i = 0; i <= 40; ++i) {
        const f32 s = heat_haze_fire_scale(0.8f, 0.2f * f32(i), 7.0f);
        CHECK(s >= prev);
        CHECK(s >= 0.2f - 1e-6f);
        CHECK(s <= 1.8f + 1e-6f);
        CHECK(same_bits(s, heat_haze_fire_scale(0.8f, 0.2f * f32(i), 7.0f)));
        prev = s;
    }
}
