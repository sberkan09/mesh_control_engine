// Midge (M-MIDGES 33a): the amount is the warm-season trapezoid (centre 0.375) times the twilight trapezoid in sun elevation
// (or the manual floor), times the calm-wind gate and the dry weather; no Season gives the manual floor; enabled 0, strength 0
// and a missing component give 0 exactly; the look is inactive zeros at amount 0 and tiny dark hovering specks otherwise; NaN
// and inverted ranges fall back; the helpers are deterministic.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>

#include "mce/kit/midge.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
constexpr f32 kSummer = 0.375f;

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
bool same_vec(math::vec3 a, math::vec3 b) { return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z); }

bool is_zero_look(const MidgeLook& l) {
    return !l.active && same_bits(l.rate, 0.0f) && same_bits(l.speed_min, 0.0f) && same_bits(l.speed_max, 0.0f) &&
           same_vec(l.direction, math::vec3{0.0f, 0.0f, 0.0f}) && same_bits(l.spread, 0.0f) &&
           same_vec(l.gravity, math::vec3{0.0f, 0.0f, 0.0f}) && same_bits(l.drag, 0.0f) && same_bits(l.size, 0.0f) &&
           same_bits(l.lifetime_min, 0.0f) && same_bits(l.lifetime_max, 0.0f) && same_bits(l.color_start.x, 0.0f) &&
           same_bits(l.color_start.w, 0.0f) && same_bits(l.color_end.w, 0.0f) && same_bits(l.box, 0.0f) &&
           same_bits(l.height, 0.0f);
}

}  // namespace

TEST_CASE("midge: defaults") {
    const Midge m;
    CHECK(m.enabled == 1);
    CHECK(m.window_full == 0.12f);
    CHECK(m.window_end == 0.2f);
    CHECK(m.manual == 0.0f);
    CHECK(m.twilight_low == -0.12f);
    CHECK(m.twilight_high == 0.25f);
    CHECK(m.wind_calm == 0.2f);
    CHECK(m.wind_still == 0.6f);
    CHECK(m.rate == 30.0f);
    CHECK(m.box_xz_m == 8.0f);
    CHECK(m.height_m == 2.5f);
    CHECK(m.size_m == 0.025f);
    CHECK(m.lifetime_s == 3.0f);
    CHECK(m.speed == 0.35f);
    CHECK(same_vec(m.color, math::vec3{0.08f, 0.07f, 0.06f}));
    CHECK(m.max_particles == 192);
    CHECK(m.seed == 1);
    CHECK(m.strength == 1.0f);
    CHECK(m.pad_[0] == 0);
    CHECK(m.pad_[1] == 0);
    CHECK(m.pad_[2] == 0);
}

TEST_CASE("midge: the window is a trapezoid around the summer centre and zero in the other seasons") {
    const Midge m;
    const Season s;
    CHECK(midge_amount(m, &s, kSummer, 0.0f, 0.0f, 0.0f) == 1.0f);
    CHECK(midge_amount(m, &s, kSummer + 0.12f, 0.0f, 0.0f, 0.0f) == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(midge_amount(m, &s, kSummer + 0.16f, 0.0f, 0.0f, 0.0f) == doctest::Approx(0.5f).epsilon(1e-3));
    CHECK(midge_amount(m, &s, kSummer + 0.25f, 0.0f, 0.0f, 0.0f) == 0.0f);
    CHECK(midge_amount(m, &s, 0.875f, 0.0f, 0.0f, 0.0f) == 0.0f);
    CHECK(midge_amount(m, &s, kSummer - 0.16f, 0.0f, 0.0f, 0.0f) ==
          doctest::Approx(midge_amount(m, &s, kSummer + 0.16f, 0.0f, 0.0f, 0.0f)));
    for (int i = 0; i <= 200; ++i) {
        const f32 a = midge_amount(m, &s, 0.005f * static_cast<f32>(i), 0.0f, 0.0f, 0.0f);
        CHECK(a >= 0.0f);
        CHECK(a <= 1.0f);
    }
}

TEST_CASE("midge: the twilight gate is a trapezoid in sun elevation, full at the horizon") {
    const Midge m;
    const Season s;
    CHECK(midge_amount(m, &s, kSummer, -0.5f, 0.0f, 0.0f) == 0.0f);     // night
    CHECK(midge_amount(m, &s, kSummer, -0.12f, 0.0f, 0.0f) == 0.0f);    // twilight_low
    CHECK(midge_amount(m, &s, kSummer, -0.06f, 0.0f, 0.0f) == doctest::Approx(0.5f));
    CHECK(midge_amount(m, &s, kSummer, 0.0f, 0.0f, 0.0f) == 1.0f);      // horizon
    CHECK(midge_amount(m, &s, kSummer, 0.125f, 0.0f, 0.0f) == doctest::Approx(0.5f));
    CHECK(midge_amount(m, &s, kSummer, 0.25f, 0.0f, 0.0f) == 0.0f);     // twilight_high
    CHECK(midge_amount(m, &s, kSummer, 1.0f, 0.0f, 0.0f) == 0.0f);      // midday
    CHECK(midge_amount(m, &s, kSummer, kNaN, 0.0f, 0.0f) == 0.0f);
    f32 prev = 0.0f;
    for (int i = 0; i <= 12; ++i) {  // monotone rise to the horizon
        const f32 a = midge_amount(m, &s, kSummer, -0.12f + 0.01f * static_cast<f32>(i), 0.0f, 0.0f);
        CHECK(a >= prev);
        prev = a;
    }
    for (int i = 0; i <= 25; ++i) {  // monotone fall from the horizon
        const f32 a = midge_amount(m, &s, kSummer, 0.01f * static_cast<f32>(i), 0.0f, 0.0f);
        CHECK(a <= prev);
        prev = a;
    }
}

TEST_CASE("midge: manual is a floor that ignores the window and twilight but not the rain; no Season gives the floor only") {
    Midge m;
    m.manual = 0.4f;
    const Season s;
    CHECK(midge_amount(m, &s, 0.875f, 1.0f, 0.0f, 0.0f) == 0.4f);
    CHECK(midge_amount(m, &s, kSummer, 0.0f, 0.0f, 0.0f) == 1.0f);
    CHECK(midge_amount(m, nullptr, kSummer, 0.0f, 0.0f, 0.0f) == 0.4f);
    Season off;
    off.enabled = 0;
    CHECK(midge_amount(m, &off, kSummer, 0.0f, 0.0f, 0.0f) == 0.4f);
    CHECK(midge_amount(m, nullptr, kSummer, 0.0f, 0.0f, 1.0f) == 0.0f);
    CHECK(midge_amount(m, nullptr, kSummer, 0.0f, 0.0f, 0.5f) == doctest::Approx(0.2f));
    const Midge none;
    CHECK(midge_amount(none, nullptr, kSummer, 0.0f, 0.0f, 0.0f) == 0.0f);
}

TEST_CASE("midge: the calm gate closes in wind and rain closes the amount") {
    const Midge m;
    const Season s;
    CHECK(midge_amount(m, &s, kSummer, 0.0f, 0.0f, 0.0f) == 1.0f);
    CHECK(midge_amount(m, &s, kSummer, 0.0f, 0.2f, 0.0f) == 1.0f);
    CHECK(midge_amount(m, &s, kSummer, 0.0f, 0.4f, 0.0f) == doctest::Approx(0.5f));
    CHECK(midge_amount(m, &s, kSummer, 0.0f, 0.6f, 0.0f) == 0.0f);
    CHECK(midge_amount(m, &s, kSummer, 0.0f, 1.0f, 0.0f) == 0.0f);
    CHECK(midge_amount(m, &s, kSummer, 0.0f, kNaN, 0.0f) == 1.0f);
    CHECK(midge_amount(m, &s, kSummer, 0.0f, 0.0f, 1.0f) == 0.0f);
    CHECK(midge_amount(m, &s, kSummer, 0.0f, 0.0f, 0.5f) == doctest::Approx(0.5f));
    CHECK(midge_amount(m, &s, kSummer, 0.0f, 0.0f, kNaN) == 1.0f);
}

TEST_CASE("midge: enabled 0, strength 0 and a missing component give exactly 0") {
    const Season s;
    Midge off;
    off.enabled = 0;
    off.manual = 1.0f;
    CHECK(same_bits(midge_amount(off, &s, kSummer, 0.0f, 0.0f, 0.0f), 0.0f));
    Midge zero;
    zero.strength = 0.0f;
    zero.manual = 1.0f;
    CHECK(same_bits(midge_amount(zero, &s, kSummer, 0.0f, 0.0f, 0.0f), 0.0f));
    CHECK(same_bits(midge_amount(static_cast<const Midge*>(nullptr), &s, kSummer, 0.0f, 0.0f, 0.0f), 0.0f));
    CHECK(is_zero_look(midge_look(off, 1.0f)));
    CHECK(is_zero_look(midge_look(zero, 1.0f)));
    CHECK(is_zero_look(midge_look(static_cast<const Midge*>(nullptr), 1.0f)));
    Midge half;
    half.strength = 0.5f;
    CHECK(midge_amount(half, &s, kSummer, 0.0f, 0.0f, 0.0f) == doctest::Approx(0.5f));
}

TEST_CASE("midge: the look is inactive at amount 0 and follows the authoring otherwise") {
    const Midge m;
    CHECK(is_zero_look(midge_look(m, 0.0f)));
    CHECK(is_zero_look(midge_look(m, kNaN)));
    CHECK(is_zero_look(midge_look(m, -1.0f)));
    const MidgeLook l = midge_look(m, 0.5f);
    CHECK(l.active);
    CHECK(l.rate == doctest::Approx(15.0f));
    CHECK(l.color_start.w == doctest::Approx(0.35f));
    CHECK(l.color_end.w == 0.0f);
    CHECK(l.color_start.x == 0.08f);
    CHECK(same_bits(l.color_end.x, l.color_start.x));
    CHECK(l.size == 0.025f);
    CHECK(l.lifetime_max == 3.0f);
    CHECK(l.lifetime_min < l.lifetime_max);
    CHECK(l.speed_max == 0.35f);
    CHECK(l.speed_min <= l.speed_max);
    CHECK(l.spread == 1.0f);
    CHECK(l.direction.y == 1.0f);
    CHECK(l.gravity.y == 0.0f);
    CHECK(l.drag > 0.0f);
    CHECK(l.box == 8.0f);
    CHECK(l.height == 2.5f);
    CHECK(midge_look(m, 5.0f).rate == 30.0f);
}

TEST_CASE("midge: NaN, inverted and out-of-range authoring fall back or clamp; the helpers are deterministic") {
    const Midge def;
    const Season s;
    const f32 ph = kSummer + 0.16f;
    const f32 want = midge_amount(def, &s, ph, 0.1f, 0.3f, 0.1f);
    Midge inv;
    inv.window_full = 0.2f;
    inv.window_end = 0.12f;
    inv.twilight_low = 0.1f;   // not below the horizon
    inv.twilight_high = -0.1f; // not above the horizon
    inv.wind_calm = 0.6f;
    inv.wind_still = 0.2f;
    CHECK(midge_amount(inv, &s, ph, 0.1f, 0.3f, 0.1f) == want);
    Midge empty;
    empty.window_full = 0.3f;
    empty.window_end = 0.3f;
    empty.twilight_low = 0.0f;
    empty.twilight_high = 0.0f;
    empty.wind_calm = 0.5f;
    empty.wind_still = 0.5f;
    CHECK(midge_amount(empty, &s, ph, 0.1f, 0.3f, 0.1f) == want);
    Midge nan;
    nan.window_full = kNaN;
    nan.window_end = kNaN;
    nan.twilight_low = kNaN;
    nan.twilight_high = kNaN;
    nan.wind_calm = kNaN;
    nan.wind_still = kNaN;
    nan.manual = kNaN;
    nan.strength = kNaN;
    CHECK(midge_amount(nan, &s, ph, 0.1f, 0.3f, 0.1f) == want);
    Midge bad;
    bad.rate = kNaN;
    bad.size_m = kNaN;
    bad.lifetime_s = kNaN;
    bad.box_xz_m = kNaN;
    bad.height_m = kNaN;
    bad.speed = kNaN;
    bad.color = math::vec3{kNaN, kNaN, kNaN};
    const MidgeLook a = midge_look(bad, 1.0f);
    const MidgeLook d = midge_look(def, 1.0f);
    CHECK(a.rate == d.rate);
    CHECK(a.size == d.size);
    CHECK(a.lifetime_max == d.lifetime_max);
    CHECK(a.speed_max == d.speed_max);
    CHECK(a.box == d.box);
    CHECK(a.height == d.height);
    CHECK(same_bits(a.color_start.x, d.color_start.x));
    Midge wild;
    wild.rate = 1e9f;
    wild.box_xz_m = 1e9f;
    wild.height_m = 1e9f;
    wild.size_m = 0.0f;
    wild.lifetime_s = 0.0f;
    wild.speed = 1e9f;
    const MidgeLook c = midge_look(wild, 1.0f);
    CHECK(c.rate == 200.0f);
    CHECK(c.box == 100.0f);
    CHECK(c.height == 20.0f);
    CHECK(c.size == 0.005f);
    CHECK(c.lifetime_max == 0.5f);
    CHECK(c.speed_max == 4.0f);
    Midge e;
    e.manual = 0.2f;
    for (int i = 0; i < 50; ++i) {
        const f32 sun = -0.2f + 0.02f * static_cast<f32>(i);
        const f32 a1 = midge_amount(e, &s, ph, sun, 0.3f, 0.1f);
        const f32 a2 = midge_amount(e, &s, ph, sun, 0.3f, 0.1f);
        CHECK(same_bits(a1, a2));
        CHECK(a1 >= 0.0f);
        CHECK(a1 <= 1.0f);
        CHECK(same_vec(midge_look(e, a1).direction, midge_look(e, a2).direction));
        CHECK(same_bits(midge_look(e, a1).rate, midge_look(e, a2).rate));
    }
}
