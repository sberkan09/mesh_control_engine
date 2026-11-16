// Butterfly (M-BUTTERFLY 27a): the amount is the summer trapezoid (centre 0.375) or the manual floor, times the day gate,
// the calm-wind gate and the dry weather; no Season gives the manual floor; enabled 0, strength 0 and a missing component
// give 0 exactly; the look is inactive zeros at amount 0 and slow wide-spread specks otherwise; NaN and inverted ranges fall
// back; the helpers are deterministic.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>

#include "mce/kit/butterfly.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
bool same_vec(math::vec3 a, math::vec3 b) { return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z); }

bool is_zero_look(const ButterflyLook& l) {
    return !l.active && same_bits(l.rate, 0.0f) && same_bits(l.speed_min, 0.0f) && same_bits(l.speed_max, 0.0f) &&
           same_vec(l.direction, math::vec3{0.0f, 0.0f, 0.0f}) && same_bits(l.spread, 0.0f) &&
           same_vec(l.gravity, math::vec3{0.0f, 0.0f, 0.0f}) && same_bits(l.drag, 0.0f) && same_bits(l.size, 0.0f) &&
           same_bits(l.lifetime_min, 0.0f) && same_bits(l.lifetime_max, 0.0f) && same_bits(l.color_start.x, 0.0f) &&
           same_bits(l.color_start.w, 0.0f) && same_bits(l.color_end.w, 0.0f) && same_bits(l.box, 0.0f) &&
           same_bits(l.height, 0.0f);
}

}  // namespace

TEST_CASE("butterfly: defaults") {
    const Butterfly b;
    CHECK(b.enabled == 1);
    CHECK(b.window_full == 0.07f);
    CHECK(b.window_end == 0.14f);
    CHECK(b.manual == 0.0f);
    CHECK(b.min_daylight == 0.6f);
    CHECK(b.wind_calm == 0.25f);
    CHECK(b.wind_still == 0.7f);
    CHECK(b.rate == 5.0f);
    CHECK(b.box_xz_m == 18.0f);
    CHECK(b.height_m == 3.0f);
    CHECK(b.size_m == 0.12f);
    CHECK(b.lifetime_s == 8.0f);
    CHECK(b.speed == 0.8f);
    CHECK(b.lift == 0.2f);
    CHECK(same_vec(b.color, math::vec3{1.0f, 0.75f, 0.2f}));
    CHECK(b.max_particles == 48);
    CHECK(b.seed == 1);
    CHECK(b.strength == 1.0f);
    CHECK(b.pad_[0] == 0);
    CHECK(b.pad_[1] == 0);
    CHECK(b.pad_[2] == 0);
}

TEST_CASE("butterfly: the window is a trapezoid around the summer centre and zero in the other seasons") {
    const Butterfly b;
    const Season s;
    CHECK(butterfly_amount(b, &s, 0.375f, 0.0f, 0.0f, 0.0f) == 1.0f);
    CHECK(butterfly_amount(b, &s, 0.375f + 0.07f, 0.0f, 0.0f, 0.0f) == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(butterfly_amount(b, &s, 0.375f + 0.105f, 0.0f, 0.0f, 0.0f) == doctest::Approx(0.5f).epsilon(1e-3));
    CHECK(butterfly_amount(b, &s, 0.375f + 0.15f, 0.0f, 0.0f, 0.0f) == 0.0f);
    CHECK(butterfly_amount(b, &s, 0.125f, 0.0f, 0.0f, 0.0f) == 0.0f);
    CHECK(butterfly_amount(b, &s, 0.625f, 0.0f, 0.0f, 0.0f) == 0.0f);
    CHECK(butterfly_amount(b, &s, 0.875f, 0.0f, 0.0f, 0.0f) == 0.0f);
    CHECK(butterfly_amount(b, &s, 0.375f - 0.105f, 0.0f, 0.0f, 0.0f) ==
          doctest::Approx(butterfly_amount(b, &s, 0.375f + 0.105f, 0.0f, 0.0f, 0.0f)));
    for (int i = 0; i <= 200; ++i) {
        const f32 a = butterfly_amount(b, &s, 0.005f * static_cast<f32>(i), 0.0f, 0.0f, 0.0f);
        CHECK(a >= 0.0f);
        CHECK(a <= 1.0f);
    }
}

TEST_CASE("butterfly: manual is a floor and no Season gives the floor only") {
    Butterfly b;
    b.manual = 0.4f;
    const Season s;
    CHECK(butterfly_amount(b, &s, 0.875f, 0.0f, 0.0f, 0.0f) == 0.4f);
    CHECK(butterfly_amount(b, nullptr, 0.375f, 0.0f, 0.0f, 0.0f) == 0.4f);
    Season off;
    off.enabled = 0;
    CHECK(butterfly_amount(b, &off, 0.375f, 0.0f, 0.0f, 0.0f) == 0.4f);
    CHECK(butterfly_amount(b, &s, 0.375f, 0.0f, 0.0f, 0.0f) == 1.0f);
    CHECK(butterfly_amount(Butterfly{}, nullptr, 0.375f, 0.0f, 0.0f, 0.0f) == 0.0f);
    b.strength = 0.5f;
    CHECK(butterfly_amount(b, &s, 0.375f, 0.0f, 0.0f, 0.0f) == doctest::Approx(0.5f));
}

TEST_CASE("butterfly: the day gate closes at night") {
    const Butterfly b;
    const Season s;
    CHECK(butterfly_amount(b, &s, 0.375f, 0.0f, 0.0f, 0.0f) == 1.0f);
    CHECK(butterfly_amount(b, &s, 0.375f, 0.4f, 0.0f, 0.0f) == 0.0f);  // daylight 0.6 = min_daylight: closed
    CHECK(butterfly_amount(b, &s, 0.375f, 0.6f, 0.0f, 0.0f) == 0.0f);
    CHECK(butterfly_amount(b, &s, 0.375f, 1.0f, 0.0f, 0.0f) == 0.0f);
    CHECK(butterfly_amount(b, &s, 0.375f, 0.2f, 0.0f, 0.0f) == doctest::Approx(0.5f).epsilon(0.05));
    CHECK(butterfly_amount(b, &s, 0.375f, kNaN, 0.0f, 0.0f) == 1.0f);
}

TEST_CASE("butterfly: the calm gate closes in wind") {
    const Butterfly b;
    const Season s;
    CHECK(butterfly_amount(b, &s, 0.375f, 0.0f, 0.0f, 0.0f) == 1.0f);
    CHECK(butterfly_amount(b, &s, 0.375f, 0.0f, 0.25f, 0.0f) == 1.0f);
    CHECK(butterfly_amount(b, &s, 0.375f, 0.0f, 0.475f, 0.0f) == doctest::Approx(0.5f));
    CHECK(butterfly_amount(b, &s, 0.375f, 0.0f, 0.7f, 0.0f) == 0.0f);
    CHECK(butterfly_amount(b, &s, 0.375f, 0.0f, 1.0f, 0.0f) == 0.0f);
    CHECK(butterfly_amount(b, &s, 0.375f, 0.0f, 9.0f, 0.0f) == 0.0f);
    CHECK(butterfly_amount(b, &s, 0.375f, 0.0f, kNaN, 0.0f) == 1.0f);
}

TEST_CASE("butterfly: rain closes the amount") {
    const Butterfly b;
    const Season s;
    CHECK(butterfly_amount(b, &s, 0.375f, 0.0f, 0.0f, 1.0f) == 0.0f);
    CHECK(butterfly_amount(b, &s, 0.375f, 0.0f, 0.0f, 0.5f) == doctest::Approx(0.5f));
    CHECK(butterfly_amount(b, &s, 0.375f, 0.0f, 0.0f, kNaN) == 1.0f);
}

TEST_CASE("butterfly: enabled 0, strength 0 and a missing component give exactly 0") {
    Butterfly off;
    off.enabled = 0;
    off.manual = 1.0f;
    const Season s;
    CHECK(same_bits(butterfly_amount(off, &s, 0.375f, 0.0f, 0.0f, 0.0f), 0.0f));
    Butterfly zero;
    zero.strength = 0.0f;
    zero.manual = 1.0f;
    CHECK(same_bits(butterfly_amount(zero, &s, 0.375f, 0.0f, 0.0f, 0.0f), 0.0f));
    CHECK(same_bits(butterfly_amount(static_cast<const Butterfly*>(nullptr), &s, 0.375f, 0.0f, 0.0f, 0.0f), 0.0f));
    CHECK(is_zero_look(butterfly_look(off, 1.0f)));
    CHECK(is_zero_look(butterfly_look(zero, 1.0f)));
    CHECK(is_zero_look(butterfly_look(static_cast<const Butterfly*>(nullptr), 1.0f)));
}

TEST_CASE("butterfly: the look is inactive at amount 0 and follows the authoring otherwise") {
    const Butterfly b;
    CHECK(is_zero_look(butterfly_look(b, 0.0f)));
    CHECK(is_zero_look(butterfly_look(b, kNaN)));
    CHECK(is_zero_look(butterfly_look(b, -1.0f)));
    const ButterflyLook l = butterfly_look(b, 0.5f);
    CHECK(l.active);
    CHECK(l.rate == doctest::Approx(2.5f));
    CHECK(l.color_start.w == doctest::Approx(0.45f));
    CHECK(l.color_end.w == 0.0f);
    CHECK(l.color_start.x == 1.0f);
    CHECK(l.color_start.y == 0.75f);
    CHECK(l.color_start.z == 0.2f);
    CHECK(same_bits(l.color_end.x, l.color_start.x));
    CHECK(l.size == 0.12f);
    CHECK(l.lifetime_max == 8.0f);
    CHECK(l.lifetime_min < l.lifetime_max);
    CHECK(l.speed_min < l.speed_max);
    CHECK(l.speed_max == 0.8f);
    CHECK(l.direction.y == 1.0f);
    CHECK(l.spread >= 0.8f);
    CHECK(l.gravity.x == 0.0f);
    CHECK(l.gravity.y == 0.2f);
    CHECK(l.gravity.z == 0.0f);
    CHECK(l.drag > 0.0f);
    CHECK(l.box == 18.0f);
    CHECK(l.height == 3.0f);
    CHECK(butterfly_look(b, 5.0f).rate == 5.0f);
}

TEST_CASE("butterfly: NaN, inverted and out-of-range authoring fall back or clamp; the helpers are deterministic") {
    const Butterfly def;
    const Season s;
    const f32 want = butterfly_amount(def, &s, 0.375f + 0.105f, 0.2f, 0.475f, 0.0f);
    Butterfly inv;
    inv.window_full = 0.14f;
    inv.window_end = 0.07f;
    inv.wind_calm = 0.7f;
    inv.wind_still = 0.25f;
    CHECK(butterfly_amount(inv, &s, 0.375f + 0.105f, 0.2f, 0.475f, 0.0f) == want);
    Butterfly empty;
    empty.window_full = 0.1f;
    empty.window_end = 0.1f;
    empty.wind_calm = 0.5f;
    empty.wind_still = 0.5f;
    CHECK(butterfly_amount(empty, &s, 0.375f + 0.105f, 0.2f, 0.475f, 0.0f) == want);
    Butterfly nan;
    nan.window_full = kNaN;
    nan.window_end = kNaN;
    nan.min_daylight = kNaN;
    nan.wind_calm = kNaN;
    nan.wind_still = kNaN;
    nan.manual = kNaN;
    nan.strength = kNaN;
    CHECK(butterfly_amount(nan, &s, 0.375f + 0.105f, 0.2f, 0.475f, 0.0f) == want);
    CHECK(butterfly_amount(def, &s, kNaN, 0.0f, 0.0f, 0.0f) == 1.0f);  // NaN phase is the summer centre
    Butterfly bad;
    bad.rate = kNaN;
    bad.size_m = kNaN;
    bad.lifetime_s = kNaN;
    bad.speed = kNaN;
    bad.lift = kNaN;
    bad.box_xz_m = kNaN;
    bad.height_m = kNaN;
    bad.color = math::vec3{kNaN, kNaN, kNaN};
    const ButterflyLook a = butterfly_look(bad, 1.0f);
    const ButterflyLook d = butterfly_look(def, 1.0f);
    CHECK(a.rate == d.rate);
    CHECK(a.size == d.size);
    CHECK(a.lifetime_max == d.lifetime_max);
    CHECK(a.speed_max == d.speed_max);
    CHECK(a.gravity.y == d.gravity.y);
    CHECK(a.box == d.box);
    CHECK(a.height == d.height);
    CHECK(same_bits(a.color_start.x, d.color_start.x));
    Butterfly wild;
    wild.rate = 1e9f;
    wild.box_xz_m = 1e9f;
    wild.size_m = 0.0f;
    const ButterflyLook c = butterfly_look(wild, 1.0f);
    CHECK(c.rate == 60.0f);
    CHECK(c.box == 100.0f);
    CHECK(c.size == 0.02f);
    Butterfly e;
    e.manual = 0.2f;
    for (int i = 0; i < 50; ++i) {
        const f32 ph = 0.02f * static_cast<f32>(i);
        const f32 a1 = butterfly_amount(e, &s, ph, 0.05f, 0.3f, 0.1f);
        const f32 a2 = butterfly_amount(e, &s, ph, 0.05f, 0.3f, 0.1f);
        CHECK(same_bits(a1, a2));
        CHECK(same_vec(butterfly_look(e, a1).direction, butterfly_look(e, a2).direction));
        CHECK(same_bits(butterfly_look(e, a1).rate, butterfly_look(e, a2).rate));
    }
}
