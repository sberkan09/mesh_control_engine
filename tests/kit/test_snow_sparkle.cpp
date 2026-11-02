// SnowSparkle (M-SNOWSPARKLE 31a): the amount is the smoothed cover gate times the low-sun gate (or the manual floor), times
// clear and dry weather; enabled 0, strength 0 and a missing component give 0 exactly; the look is inactive zeros at amount
// 0 and tiny additive specks otherwise; NaN and inverted ranges fall back; the helpers are deterministic.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>

#include "mce/kit/snow_sparkle.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
bool same_vec(math::vec3 a, math::vec3 b) { return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z); }

bool is_zero_look(const SnowSparkleLook& l) {
    return !l.active && !l.additive && same_bits(l.rate, 0.0f) && same_bits(l.speed_min, 0.0f) &&
           same_bits(l.speed_max, 0.0f) && same_vec(l.direction, math::vec3{0.0f, 0.0f, 0.0f}) && same_bits(l.spread, 0.0f) &&
           same_vec(l.gravity, math::vec3{0.0f, 0.0f, 0.0f}) && same_bits(l.drag, 0.0f) && same_bits(l.size, 0.0f) &&
           same_bits(l.lifetime_min, 0.0f) && same_bits(l.lifetime_max, 0.0f) && same_bits(l.color_start.x, 0.0f) &&
           same_bits(l.color_start.w, 0.0f) && same_bits(l.color_end.w, 0.0f) && same_bits(l.box, 0.0f) &&
           same_bits(l.height, 0.0f);
}

}  // namespace

TEST_CASE("sparkle: defaults") {
    const SnowSparkle s;
    CHECK(s.enabled == 1);
    CHECK(s.cover_low == 0.2f);
    CHECK(s.cover_full == 0.8f);
    CHECK(s.sun_low == 0.05f);
    CHECK(s.sun_high == 0.6f);
    CHECK(s.manual == 0.0f);
    CHECK(s.rate == 24.0f);
    CHECK(s.box_xz_m == 14.0f);
    CHECK(s.height_m == 1.2f);
    CHECK(s.size_m == 0.03f);
    CHECK(s.lifetime_s == 0.6f);
    CHECK(same_vec(s.color, math::vec3{1.0f, 1.0f, 1.0f}));
    CHECK(s.max_particles == 96);
    CHECK(s.seed == 1);
    CHECK(s.strength == 1.0f);
    CHECK(s.pad_[0] == 0);
    CHECK(s.pad_[1] == 0);
    CHECK(s.pad_[2] == 0);
}

TEST_CASE("sparkle: the cover gate is a smoothstep between cover_low and cover_full") {
    const SnowSparkle s;
    CHECK(snow_sparkle_amount(s, 0.0f, 0.05f, 0.0f, 0.0f) == 0.0f);
    CHECK(snow_sparkle_amount(s, 0.2f, 0.05f, 0.0f, 0.0f) == 0.0f);
    CHECK(snow_sparkle_amount(s, 0.5f, 0.05f, 0.0f, 0.0f) == doctest::Approx(0.5f));
    CHECK(snow_sparkle_amount(s, 0.8f, 0.05f, 0.0f, 0.0f) == 1.0f);
    CHECK(snow_sparkle_amount(s, 1.0f, 0.05f, 0.0f, 0.0f) == 1.0f);
    CHECK(snow_sparkle_amount(s, 9.0f, 0.05f, 0.0f, 0.0f) == 1.0f);
    CHECK(snow_sparkle_amount(s, kNaN, 0.05f, 0.0f, 0.0f) == 0.0f);
    f32 prev = 0.0f;
    for (int i = 0; i <= 100; ++i) {
        const f32 a = snow_sparkle_amount(s, 0.01f * static_cast<f32>(i), 0.05f, 0.0f, 0.0f);
        CHECK(a >= prev);
        CHECK(a <= 1.0f);
        prev = a;
    }
}

TEST_CASE("sparkle: the sun gate is strongest in low sun and closed at night and in high sun") {
    const SnowSparkle s;
    CHECK(snow_sparkle_amount(s, 1.0f, 0.0f, 0.0f, 0.0f) == 0.0f);    // horizon
    CHECK(snow_sparkle_amount(s, 1.0f, -0.5f, 0.0f, 0.0f) == 0.0f);   // below the horizon
    CHECK(snow_sparkle_amount(s, 1.0f, 0.025f, 0.0f, 0.0f) == doctest::Approx(0.5f));
    CHECK(snow_sparkle_amount(s, 1.0f, 0.05f, 0.0f, 0.0f) == 1.0f);
    CHECK(snow_sparkle_amount(s, 1.0f, 0.15f, 0.0f, 0.0f) == 1.0f);   // inside the plateau (to 0.1875)
    const f32 mid = snow_sparkle_amount(s, 1.0f, 0.3f, 0.0f, 0.0f);
    CHECK(mid > 0.0f);
    CHECK(mid < 1.0f);
    CHECK(snow_sparkle_amount(s, 1.0f, 0.6f, 0.0f, 0.0f) == 0.0f);
    CHECK(snow_sparkle_amount(s, 1.0f, 1.0f, 0.0f, 0.0f) == 0.0f);    // overhead sun
    CHECK(snow_sparkle_amount(s, 1.0f, kNaN, 0.0f, 0.0f) == 0.0f);
    CHECK(snow_sparkle_amount(s, 1.0f, 0.1f, 0.0f, 0.0f) >= snow_sparkle_amount(s, 1.0f, 0.4f, 0.0f, 0.0f));
}

TEST_CASE("sparkle: manual is a floor that ignores the cover and sun gates but not cloud and rain") {
    SnowSparkle s;
    s.manual = 0.4f;
    CHECK(snow_sparkle_amount(s, 0.0f, 0.0f, 0.0f, 0.0f) == 0.4f);
    CHECK(snow_sparkle_amount(s, 0.0f, 1.0f, 0.0f, 0.0f) == 0.4f);
    CHECK(snow_sparkle_amount(s, 1.0f, 0.05f, 0.0f, 0.0f) == 1.0f);
    CHECK(snow_sparkle_amount(s, 0.0f, 0.0f, 0.5f, 0.0f) == doctest::Approx(0.2f));
    CHECK(snow_sparkle_amount(s, 0.0f, 0.0f, 0.0f, 1.0f) == 0.0f);
    s.strength = 0.5f;
    CHECK(snow_sparkle_amount(s, 0.0f, 0.0f, 0.0f, 0.0f) == doctest::Approx(0.2f));
}

TEST_CASE("sparkle: cloud and rain close the amount") {
    const SnowSparkle s;
    CHECK(snow_sparkle_amount(s, 1.0f, 0.05f, 1.0f, 0.0f) == 0.0f);
    CHECK(snow_sparkle_amount(s, 1.0f, 0.05f, 0.5f, 0.0f) == doctest::Approx(0.5f));
    CHECK(snow_sparkle_amount(s, 1.0f, 0.05f, 0.0f, 1.0f) == 0.0f);
    CHECK(snow_sparkle_amount(s, 1.0f, 0.05f, 0.0f, 0.5f) == doctest::Approx(0.5f));
    CHECK(snow_sparkle_amount(s, 1.0f, 0.05f, 0.5f, 0.5f) == doctest::Approx(0.25f));
    CHECK(snow_sparkle_amount(s, 1.0f, 0.05f, kNaN, kNaN) == 1.0f);
}

TEST_CASE("sparkle: enabled 0, strength 0 and a missing component give exactly 0") {
    SnowSparkle off;
    off.enabled = 0;
    off.manual = 1.0f;
    CHECK(same_bits(snow_sparkle_amount(off, 1.0f, 0.05f, 0.0f, 0.0f), 0.0f));
    SnowSparkle zero;
    zero.strength = 0.0f;
    zero.manual = 1.0f;
    CHECK(same_bits(snow_sparkle_amount(zero, 1.0f, 0.05f, 0.0f, 0.0f), 0.0f));
    CHECK(same_bits(snow_sparkle_amount(static_cast<const SnowSparkle*>(nullptr), 1.0f, 0.05f, 0.0f, 0.0f), 0.0f));
    CHECK(is_zero_look(snow_sparkle_look(off, 1.0f)));
    CHECK(is_zero_look(snow_sparkle_look(zero, 1.0f)));
    CHECK(is_zero_look(snow_sparkle_look(static_cast<const SnowSparkle*>(nullptr), 1.0f)));
}

TEST_CASE("sparkle: the look is inactive at amount 0 and follows the authoring otherwise") {
    const SnowSparkle s;
    CHECK(is_zero_look(snow_sparkle_look(s, 0.0f)));
    CHECK(is_zero_look(snow_sparkle_look(s, kNaN)));
    CHECK(is_zero_look(snow_sparkle_look(s, -1.0f)));
    const SnowSparkleLook l = snow_sparkle_look(s, 0.5f);
    CHECK(l.active);
    CHECK(l.additive);
    CHECK(l.rate == doctest::Approx(12.0f));
    CHECK(l.color_start.w == doctest::Approx(0.5f));
    CHECK(l.color_end.w == 0.0f);
    CHECK(l.color_start.x == 1.0f);
    CHECK(l.color_start.y == 1.0f);
    CHECK(l.color_start.z == 1.0f);
    CHECK(same_bits(l.color_end.x, l.color_start.x));
    CHECK(l.size == 0.03f);
    CHECK(l.lifetime_max == 0.6f);
    CHECK(l.lifetime_min < l.lifetime_max);
    CHECK(l.speed_max <= 0.1f);
    CHECK(l.speed_min <= l.speed_max);
    CHECK(l.direction.y == 1.0f);
    CHECK(l.gravity.y == 0.0f);
    CHECK(l.drag > 0.0f);
    CHECK(l.box == 14.0f);
    CHECK(l.height == 1.2f);
    CHECK(snow_sparkle_look(s, 5.0f).rate == 24.0f);
}

TEST_CASE("sparkle: NaN, inverted and out-of-range authoring fall back or clamp; the helpers are deterministic") {
    const SnowSparkle def;
    const f32 want = snow_sparkle_amount(def, 0.5f, 0.3f, 0.1f, 0.1f);
    SnowSparkle inv;
    inv.cover_low = 0.8f;
    inv.cover_full = 0.2f;
    inv.sun_low = 0.6f;
    inv.sun_high = 0.05f;
    CHECK(snow_sparkle_amount(inv, 0.5f, 0.3f, 0.1f, 0.1f) == want);
    SnowSparkle empty;
    empty.cover_low = 0.5f;
    empty.cover_full = 0.5f;
    empty.sun_low = 0.3f;
    empty.sun_high = 0.3f;
    CHECK(snow_sparkle_amount(empty, 0.5f, 0.3f, 0.1f, 0.1f) == want);
    SnowSparkle nan;
    nan.cover_low = kNaN;
    nan.cover_full = kNaN;
    nan.sun_low = kNaN;
    nan.sun_high = kNaN;
    nan.manual = kNaN;
    nan.strength = kNaN;
    CHECK(snow_sparkle_amount(nan, 0.5f, 0.3f, 0.1f, 0.1f) == want);
    SnowSparkle bad;
    bad.rate = kNaN;
    bad.size_m = kNaN;
    bad.lifetime_s = kNaN;
    bad.box_xz_m = kNaN;
    bad.height_m = kNaN;
    bad.color = math::vec3{kNaN, kNaN, kNaN};
    const SnowSparkleLook a = snow_sparkle_look(bad, 1.0f);
    const SnowSparkleLook d = snow_sparkle_look(def, 1.0f);
    CHECK(a.rate == d.rate);
    CHECK(a.size == d.size);
    CHECK(a.lifetime_max == d.lifetime_max);
    CHECK(a.box == d.box);
    CHECK(a.height == d.height);
    CHECK(same_bits(a.color_start.x, d.color_start.x));
    SnowSparkle wild;
    wild.rate = 1e9f;
    wild.box_xz_m = 1e9f;
    wild.height_m = 1e9f;
    wild.size_m = 0.0f;
    wild.lifetime_s = 0.0f;
    const SnowSparkleLook c = snow_sparkle_look(wild, 1.0f);
    CHECK(c.rate == 200.0f);
    CHECK(c.box == 100.0f);
    CHECK(c.height == 10.0f);
    CHECK(c.size == 0.005f);
    CHECK(c.lifetime_max == 0.1f);
    SnowSparkle e;
    e.manual = 0.2f;
    for (int i = 0; i < 50; ++i) {
        const f32 sun = 0.02f * static_cast<f32>(i);
        const f32 a1 = snow_sparkle_amount(e, 0.6f, sun, 0.2f, 0.1f);
        const f32 a2 = snow_sparkle_amount(e, 0.6f, sun, 0.2f, 0.1f);
        CHECK(same_bits(a1, a2));
        CHECK(a1 >= 0.0f);
        CHECK(a1 <= 1.0f);
        CHECK(same_vec(snow_sparkle_look(e, a1).direction, snow_sparkle_look(e, a2).direction));
        CHECK(same_bits(snow_sparkle_look(e, a1).rate, snow_sparkle_look(e, a2).rate));
    }
}
