// Dust (M-DUSTWIND 25a): the amount is 0 below wind_start, 1 at wind_full and continuous between; `manual` is a floor;
// rain closes it; enabled 0, strength 0 and a missing component give 0 exactly; the look is inactive zeros at amount 0 and
// streams along the wind; NaN and inverted ranges fall back; the helpers are deterministic.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>

#include "mce/kit/dust.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
const math::vec2 kNoWind{0.0f, 0.0f};

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
bool same_vec(math::vec3 a, math::vec3 b) { return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z); }

bool is_zero_look(const DustLook& l) {
    return !l.active && same_bits(l.rate, 0.0f) && same_bits(l.speed_min, 0.0f) && same_bits(l.speed_max, 0.0f) &&
           same_vec(l.direction, math::vec3{0.0f, 0.0f, 0.0f}) && same_bits(l.spread, 0.0f) &&
           same_vec(l.gravity, math::vec3{0.0f, 0.0f, 0.0f}) && same_bits(l.drag, 0.0f) && same_bits(l.size, 0.0f) &&
           same_bits(l.lifetime_min, 0.0f) && same_bits(l.lifetime_max, 0.0f) && same_bits(l.color_start.x, 0.0f) &&
           same_bits(l.color_start.w, 0.0f) && same_bits(l.color_end.w, 0.0f) && same_bits(l.box, 0.0f) &&
           same_bits(l.height, 0.0f);
}

}  // namespace

TEST_CASE("dust: defaults") {
    const Dust d;
    CHECK(d.enabled == 1);
    CHECK(d.wind_start == 0.3f);
    CHECK(d.wind_full == 0.8f);
    CHECK(d.manual == 0.0f);
    CHECK(d.rate == 40.0f);
    CHECK(d.box_xz_m == 14.0f);
    CHECK(d.height_m == 3.0f);
    CHECK(d.size_m == 0.05f);
    CHECK(d.lifetime_s == 2.5f);
    CHECK(d.speed == 6.0f);
    CHECK(d.wind_follow == 0.8f);
    CHECK(same_vec(d.color, math::vec3{0.78f, 0.66f, 0.46f}));
    CHECK(d.max_particles == 1024);
    CHECK(d.seed == 1);
    CHECK(d.strength == 1.0f);
    CHECK(d.pad_[0] == 0);
    CHECK(d.pad_[1] == 0);
    CHECK(d.pad_[2] == 0);
}

TEST_CASE("dust: 0 below wind_start, 1 at wind_full and rising in between") {
    const Dust d;
    CHECK(dust_amount(d, 0.0f, 0.0f) == 0.0f);
    CHECK(dust_amount(d, 0.3f, 0.0f) == 0.0f);
    CHECK(dust_amount(d, -0.3f, 0.0f) == 0.0f);
    CHECK(dust_amount(d, 0.8f, 0.0f) == 1.0f);
    CHECK(dust_amount(d, 5.0f, 0.0f) == 1.0f);
    CHECK(dust_amount(d, 0.55f, 0.0f) == doctest::Approx(0.5f));
    f32 prev = 0.0f;
    for (int i = 0; i <= 80; ++i) {
        const f32 a = dust_amount(d, 0.0125f * static_cast<f32>(i), 0.0f);
        CHECK(a >= prev);
        CHECK(a <= 1.0f);
        prev = a;
    }
    // continuous at both edges
    CHECK(dust_amount(d, 0.301f, 0.0f) < 0.001f);
    CHECK(dust_amount(d, 0.799f, 0.0f) > 0.999f);
}

TEST_CASE("dust: manual is a floor and rain closes the amount") {
    Dust d;
    d.manual = 0.4f;
    CHECK(dust_amount(d, 0.0f, 0.0f) == 0.4f);
    CHECK(dust_amount(d, 0.55f, 0.0f) == doctest::Approx(0.5f));
    CHECK(dust_amount(d, 0.8f, 0.0f) == 1.0f);
    CHECK(dust_amount(d, 0.8f, 1.0f) == 0.0f);
    CHECK(dust_amount(d, 0.0f, 1.0f) == 0.0f);
    CHECK(dust_amount(d, 0.0f, 0.5f) == doctest::Approx(0.2f));
    CHECK(dust_amount(Dust{}, 0.8f, 0.5f) == doctest::Approx(0.5f));
    d.strength = 0.5f;
    CHECK(dust_amount(d, 0.8f, 0.0f) == doctest::Approx(0.5f));
}

TEST_CASE("dust: enabled 0, strength 0 and a missing component give exactly 0") {
    Dust off;
    off.enabled = 0;
    CHECK(same_bits(dust_amount(off, 1.5f, 0.0f), 0.0f));
    Dust zero;
    zero.strength = 0.0f;
    zero.manual = 1.0f;
    CHECK(same_bits(dust_amount(zero, 1.5f, 0.0f), 0.0f));
    CHECK(same_bits(dust_amount(static_cast<const Dust*>(nullptr), 1.5f, 0.0f), 0.0f));
    CHECK(is_zero_look(dust_look(off, 1.0f, 0.7f, math::vec2{1.0f, 0.0f})));
    CHECK(is_zero_look(dust_look(zero, 1.0f, 0.7f, math::vec2{1.0f, 0.0f})));
    CHECK(is_zero_look(dust_look(static_cast<const Dust*>(nullptr), 1.0f, 0.7f, math::vec2{1.0f, 0.0f})));
}

TEST_CASE("dust: the look is inactive at amount 0 and follows the authoring otherwise") {
    const Dust d;
    CHECK(is_zero_look(dust_look(d, 0.0f, 0.8f, kNoWind)));
    CHECK(is_zero_look(dust_look(d, kNaN, 0.8f, kNoWind)));
    CHECK(is_zero_look(dust_look(d, -1.0f, 0.8f, kNoWind)));
    const DustLook l = dust_look(d, 0.5f, 0.8f, math::vec2{1.0f, 0.0f});
    CHECK(l.active);
    CHECK(l.rate == doctest::Approx(20.0f));
    CHECK(l.color_start.w == doctest::Approx(0.3f));
    CHECK(l.color_end.w == 0.0f);
    CHECK(l.color_start.x == 0.78f);
    CHECK(l.color_start.y == 0.66f);
    CHECK(l.color_start.z == 0.46f);
    CHECK(l.size == 0.05f);
    CHECK(l.lifetime_max == 2.5f);
    CHECK(l.lifetime_min < l.lifetime_max);
    CHECK(l.speed_min < l.speed_max);
    CHECK(l.speed_max == doctest::Approx(6.0f * 0.8f));
    CHECK(l.box == 14.0f);
    CHECK(l.height == 3.0f);
    CHECK(l.gravity.y < 0.0f);
    CHECK(dust_look(d, 5.0f, 0.8f, kNoWind).rate == 40.0f);
}

TEST_CASE("dust: the stream runs along the wind direction and scales with the wind") {
    const Dust d;
    const DustLook east = dust_look(d, 1.0f, 0.8f, math::vec2{1.0f, 0.0f});
    const DustLook south = dust_look(d, 1.0f, 0.8f, math::vec2{0.0f, 1.0f});
    CHECK(east.direction.x > 0.9f);
    CHECK(east.direction.z == 0.0f);
    CHECK(east.direction.y < 0.0f);
    CHECK(south.direction.z > 0.9f);
    CHECK(south.direction.x == 0.0f);
    CHECK(math::length(east.direction) == doctest::Approx(1.0f));
    // a calm is slower than a gale; wind_follow 0 stops the stream; NaN wind and direction count as 0
    CHECK(dust_look(d, 1.0f, 0.0f, math::vec2{1.0f, 0.0f}).speed_max < east.speed_max);
    Dust deaf;
    deaf.wind_follow = 0.0f;
    CHECK(dust_look(deaf, 1.0f, 0.8f, math::vec2{1.0f, 0.0f}).speed_max == 0.0f);
    CHECK(dust_look(d, 1.0f, kNaN, math::vec2{1.0f, 0.0f}).speed_max == dust_look(d, 1.0f, 0.0f, math::vec2{1.0f, 0.0f}).speed_max);
    const DustLook nodir = dust_look(d, 1.0f, 0.8f, math::vec2{kNaN, kNaN});
    CHECK(nodir.direction.x == 0.0f);
    CHECK(math::length(nodir.direction) == doctest::Approx(1.0f));
    // a storm is capped
    CHECK(dust_look(d, 1.0f, 60.0f, math::vec2{1.0f, 0.0f}).speed_max == dust_look(d, 1.0f, 1.0f, math::vec2{1.0f, 0.0f}).speed_max);
}

TEST_CASE("dust: NaN, inverted and zero ranges fall back") {
    const Dust def;
    const f32 want = dust_amount(def, 0.55f, 0.0f);
    Dust inv;
    inv.wind_start = 0.8f;
    inv.wind_full = 0.3f;
    CHECK(dust_amount(inv, 0.55f, 0.0f) == want);
    Dust empty;
    empty.wind_start = 0.5f;
    empty.wind_full = 0.5f;
    CHECK(dust_amount(empty, 0.55f, 0.0f) == want);
    Dust nan;
    nan.wind_start = kNaN;
    nan.wind_full = kNaN;
    nan.manual = kNaN;
    nan.strength = kNaN;
    CHECK(dust_amount(nan, 0.55f, 0.0f) == want);
    CHECK(dust_amount(def, kNaN, 0.0f) == 0.0f);
    CHECK(dust_amount(def, 0.8f, kNaN) == 1.0f);
    Dust bad;
    bad.rate = kNaN;
    bad.size_m = kNaN;
    bad.lifetime_s = kNaN;
    bad.speed = kNaN;
    bad.wind_follow = kNaN;
    bad.box_xz_m = kNaN;
    bad.height_m = kNaN;
    bad.color = math::vec3{kNaN, kNaN, kNaN};
    const DustLook a = dust_look(bad, 1.0f, 0.8f, math::vec2{1.0f, 0.0f});
    const DustLook b = dust_look(def, 1.0f, 0.8f, math::vec2{1.0f, 0.0f});
    CHECK(a.rate == b.rate);
    CHECK(a.size == b.size);
    CHECK(a.lifetime_max == b.lifetime_max);
    CHECK(a.speed_max == b.speed_max);
    CHECK(a.box == b.box);
    CHECK(a.height == b.height);
    CHECK(same_vec(math::vec3{a.color_start.x, a.color_start.y, a.color_start.z}, math::vec3{b.color_start.x, b.color_start.y, b.color_start.z}));
    // out-of-range authoring is clamped
    Dust wild;
    wild.rate = 1e9f;
    wild.box_xz_m = 1e9f;
    wild.size_m = 0.0f;
    const DustLook c = dust_look(wild, 1.0f, 0.8f, kNoWind);
    CHECK(c.rate == 400.0f);
    CHECK(c.box == 100.0f);
    CHECK(c.size == 0.01f);
}

TEST_CASE("dust: the helpers are deterministic") {
    Dust d;
    d.manual = 0.2f;
    for (int i = 0; i < 50; ++i) {
        const f32 w = 0.02f * static_cast<f32>(i);
        const f32 a1 = dust_amount(d, w, 0.1f);
        const f32 a2 = dust_amount(d, w, 0.1f);
        CHECK(same_bits(a1, a2));
        const DustLook l1 = dust_look(d, a1, w, math::vec2{0.6f, 0.8f});
        const DustLook l2 = dust_look(d, a2, w, math::vec2{0.6f, 0.8f});
        CHECK(same_bits(l1.rate, l2.rate));
        CHECK(same_vec(l1.direction, l2.direction));
        CHECK(same_bits(l1.speed_max, l2.speed_max));
    }
}
