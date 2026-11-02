// Bat (M-BAT 30a): the amount is the warm-season trapezoid (centre 0.375) or the manual floor, times the dusk gate, the
// calm-wind gate and the dry weather; no Season gives the manual floor; enabled 0, strength 0 and a missing component
// give 0 exactly; the look is inactive zeros at amount 0 and fast wide-spread specks otherwise; NaN and inverted ranges fall
// back; the helpers are deterministic.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>

#include "mce/kit/bat.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
bool same_vec(math::vec3 a, math::vec3 b) { return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z); }

bool is_zero_look(const BatLook& l) {
    return !l.active && same_bits(l.rate, 0.0f) && same_bits(l.speed_min, 0.0f) && same_bits(l.speed_max, 0.0f) &&
           same_vec(l.direction, math::vec3{0.0f, 0.0f, 0.0f}) && same_bits(l.spread, 0.0f) &&
           same_vec(l.gravity, math::vec3{0.0f, 0.0f, 0.0f}) && same_bits(l.drag, 0.0f) && same_bits(l.size, 0.0f) &&
           same_bits(l.lifetime_min, 0.0f) && same_bits(l.lifetime_max, 0.0f) && same_bits(l.color_start.x, 0.0f) &&
           same_bits(l.color_start.w, 0.0f) && same_bits(l.color_end.w, 0.0f) && same_bits(l.box, 0.0f) &&
           same_bits(l.height, 0.0f);
}

}  // namespace

TEST_CASE("bat: defaults") {
    const Bat b;
    CHECK(b.enabled == 1);
    CHECK(b.window_full == 0.16f);
    CHECK(b.window_end == 0.26f);
    CHECK(b.manual == 0.0f);
    CHECK(b.dusk_low == 0.25f);
    CHECK(b.dusk_high == 0.85f);
    CHECK(b.wind_calm == 0.3f);
    CHECK(b.wind_still == 0.75f);
    CHECK(b.rate == 4.0f);
    CHECK(b.box_xz_m == 24.0f);
    CHECK(b.height_m == 6.0f);
    CHECK(b.size_m == 0.1f);
    CHECK(b.lifetime_s == 5.0f);
    CHECK(b.speed == 3.0f);
    CHECK(same_vec(b.color, math::vec3{0.08f, 0.07f, 0.1f}));
    CHECK(b.max_particles == 32);
    CHECK(b.seed == 1);
    CHECK(b.strength == 1.0f);
    CHECK(b.pad_[0] == 0);
    CHECK(b.pad_[1] == 0);
    CHECK(b.pad_[2] == 0);
}

TEST_CASE("bat: the window is a trapezoid around the summer centre and zero in the other seasons") {
    const Bat b;
    const Season s;
    CHECK(bat_amount(b, &s, 0.375f, 0.5f, 0.0f, 0.0f) == 1.0f);
    CHECK(bat_amount(b, &s, 0.375f + 0.16f, 0.5f, 0.0f, 0.0f) == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(bat_amount(b, &s, 0.375f + 0.21f, 0.5f, 0.0f, 0.0f) == doctest::Approx(0.5f).epsilon(1e-3));
    CHECK(bat_amount(b, &s, 0.375f + 0.27f, 0.5f, 0.0f, 0.0f) == 0.0f);
    CHECK(bat_amount(b, &s, 0.875f, 0.5f, 0.0f, 0.0f) == 0.0f);
    CHECK(bat_amount(b, &s, 0.375f - 0.21f, 0.5f, 0.0f, 0.0f) ==
          doctest::Approx(bat_amount(b, &s, 0.375f + 0.21f, 0.5f, 0.0f, 0.0f)));
    for (int i = 0; i <= 200; ++i) {
        const f32 a = bat_amount(b, &s, 0.005f * static_cast<f32>(i), 0.5f, 0.0f, 0.0f);
        CHECK(a >= 0.0f);
        CHECK(a <= 1.0f);
    }
}

TEST_CASE("bat: manual is a floor and no Season gives the floor only") {
    Bat b;
    b.manual = 0.4f;
    const Season s;
    CHECK(bat_amount(b, &s, 0.875f, 0.5f, 0.0f, 0.0f) == 0.4f);
    CHECK(bat_amount(b, nullptr, 0.375f, 0.5f, 0.0f, 0.0f) == 0.4f);
    Season off;
    off.enabled = 0;
    CHECK(bat_amount(b, &off, 0.375f, 0.5f, 0.0f, 0.0f) == 0.4f);
    CHECK(bat_amount(b, &s, 0.375f, 0.5f, 0.0f, 0.0f) == 1.0f);
    CHECK(bat_amount(Bat{}, nullptr, 0.375f, 0.5f, 0.0f, 0.0f) == 0.0f);
    b.strength = 0.5f;
    CHECK(bat_amount(b, &s, 0.375f, 0.5f, 0.0f, 0.0f) == doctest::Approx(0.5f));
}

TEST_CASE("bat: the dusk gate is open inside the band and closed by day and in deep night") {
    const Bat b;
    const Season s;
    CHECK(bat_amount(b, &s, 0.375f, 0.0f, 0.0f, 0.0f) == 0.0f);   // full day
    CHECK(bat_amount(b, &s, 0.375f, 0.25f, 0.0f, 0.0f) == 0.0f);  // lower edge
    CHECK(bat_amount(b, &s, 0.375f, 0.30f, 0.0f, 0.0f) == doctest::Approx(0.5f).epsilon(1e-3));
    CHECK(bat_amount(b, &s, 0.375f, 0.35f, 0.0f, 0.0f) == 1.0f);
    CHECK(bat_amount(b, &s, 0.375f, 0.55f, 0.0f, 0.0f) == 1.0f);
    CHECK(bat_amount(b, &s, 0.375f, 0.75f, 0.0f, 0.0f) == 1.0f);
    CHECK(bat_amount(b, &s, 0.375f, 0.80f, 0.0f, 0.0f) == doctest::Approx(0.5f).epsilon(1e-3));
    CHECK(bat_amount(b, &s, 0.375f, 0.85f, 0.0f, 0.0f) == 0.0f);  // upper edge
    CHECK(bat_amount(b, &s, 0.375f, 1.0f, 0.0f, 0.0f) == 0.0f);    // deep night
    CHECK(bat_amount(b, &s, 0.375f, kNaN, 0.0f, 0.0f) == 0.0f);    // NaN is full day
}

TEST_CASE("bat: the calm gate closes in wind") {
    const Bat b;
    const Season s;
    CHECK(bat_amount(b, &s, 0.375f, 0.5f, 0.0f, 0.0f) == 1.0f);
    CHECK(bat_amount(b, &s, 0.375f, 0.5f, 0.3f, 0.0f) == 1.0f);
    CHECK(bat_amount(b, &s, 0.375f, 0.5f, 0.525f, 0.0f) == doctest::Approx(0.5f));
    CHECK(bat_amount(b, &s, 0.375f, 0.5f, 0.75f, 0.0f) == 0.0f);
    CHECK(bat_amount(b, &s, 0.375f, 0.5f, 9.0f, 0.0f) == 0.0f);
    CHECK(bat_amount(b, &s, 0.375f, 0.5f, kNaN, 0.0f) == 1.0f);
}

TEST_CASE("bat: rain closes the amount") {
    const Bat b;
    const Season s;
    CHECK(bat_amount(b, &s, 0.375f, 0.5f, 0.0f, 1.0f) == 0.0f);
    CHECK(bat_amount(b, &s, 0.375f, 0.5f, 0.0f, 0.5f) == doctest::Approx(0.5f));
    CHECK(bat_amount(b, &s, 0.375f, 0.5f, 0.0f, kNaN) == 1.0f);
}

TEST_CASE("bat: enabled 0, strength 0 and a missing component give exactly 0") {
    Bat off;
    off.enabled = 0;
    off.manual = 1.0f;
    const Season s;
    CHECK(same_bits(bat_amount(off, &s, 0.375f, 0.5f, 0.0f, 0.0f), 0.0f));
    Bat zero;
    zero.strength = 0.0f;
    zero.manual = 1.0f;
    CHECK(same_bits(bat_amount(zero, &s, 0.375f, 0.5f, 0.0f, 0.0f), 0.0f));
    CHECK(same_bits(bat_amount(static_cast<const Bat*>(nullptr), &s, 0.375f, 0.5f, 0.0f, 0.0f), 0.0f));
    CHECK(is_zero_look(bat_look(off, 1.0f)));
    CHECK(is_zero_look(bat_look(zero, 1.0f)));
    CHECK(is_zero_look(bat_look(static_cast<const Bat*>(nullptr), 1.0f)));
}

TEST_CASE("bat: the look is inactive at amount 0 and follows the authoring otherwise") {
    const Bat b;
    CHECK(is_zero_look(bat_look(b, 0.0f)));
    CHECK(is_zero_look(bat_look(b, kNaN)));
    CHECK(is_zero_look(bat_look(b, -1.0f)));
    const BatLook l = bat_look(b, 0.5f);
    CHECK(l.active);
    CHECK(l.rate == doctest::Approx(2.0f));
    CHECK(l.color_start.w == doctest::Approx(0.425f));
    CHECK(l.color_end.w == 0.0f);
    CHECK(l.color_start.x == 0.08f);
    CHECK(l.color_start.y == 0.07f);
    CHECK(l.color_start.z == 0.1f);
    CHECK(same_bits(l.color_end.x, l.color_start.x));
    CHECK(l.size == 0.1f);
    CHECK(l.lifetime_max == 5.0f);
    CHECK(l.lifetime_min < l.lifetime_max);
    CHECK(l.speed_min < l.speed_max);
    CHECK(l.speed_max == 3.0f);
    CHECK(l.direction.y == 1.0f);
    CHECK(l.spread >= 0.8f);
    CHECK(std::fabs(l.gravity.y) <= 0.5f);
    CHECK(l.drag > 0.0f);
    CHECK(l.box == 24.0f);
    CHECK(l.height == 6.0f);
    CHECK(bat_look(b, 5.0f).rate == 4.0f);
}

TEST_CASE("bat: NaN, inverted and out-of-range authoring fall back or clamp; the helpers are deterministic") {
    const Bat def;
    const Season s;
    const f32 want = bat_amount(def, &s, 0.375f + 0.21f, 0.30f, 0.525f, 0.0f);
    Bat inv;
    inv.window_full = 0.26f;
    inv.window_end = 0.16f;
    inv.dusk_low = 0.85f;
    inv.dusk_high = 0.25f;
    inv.wind_calm = 0.75f;
    inv.wind_still = 0.3f;
    CHECK(bat_amount(inv, &s, 0.375f + 0.21f, 0.30f, 0.525f, 0.0f) == want);
    Bat empty;
    empty.window_full = 0.2f;
    empty.window_end = 0.2f;
    empty.dusk_low = 0.5f;
    empty.dusk_high = 0.5f;
    empty.wind_calm = 0.5f;
    empty.wind_still = 0.5f;
    CHECK(bat_amount(empty, &s, 0.375f + 0.21f, 0.30f, 0.525f, 0.0f) == want);
    Bat nan;
    nan.window_full = kNaN;
    nan.window_end = kNaN;
    nan.dusk_low = kNaN;
    nan.dusk_high = kNaN;
    nan.wind_calm = kNaN;
    nan.wind_still = kNaN;
    nan.manual = kNaN;
    nan.strength = kNaN;
    CHECK(bat_amount(nan, &s, 0.375f + 0.21f, 0.30f, 0.525f, 0.0f) == want);
    CHECK(bat_amount(def, &s, kNaN, 0.5f, 0.0f, 0.0f) == 1.0f);  // NaN phase is the summer centre
    Bat bad;
    bad.rate = kNaN;
    bad.size_m = kNaN;
    bad.lifetime_s = kNaN;
    bad.speed = kNaN;
    bad.box_xz_m = kNaN;
    bad.height_m = kNaN;
    bad.color = math::vec3{kNaN, kNaN, kNaN};
    const BatLook a = bat_look(bad, 1.0f);
    const BatLook d = bat_look(def, 1.0f);
    CHECK(a.rate == d.rate);
    CHECK(a.size == d.size);
    CHECK(a.lifetime_max == d.lifetime_max);
    CHECK(a.speed_max == d.speed_max);
    CHECK(a.box == d.box);
    CHECK(a.height == d.height);
    CHECK(same_bits(a.color_start.x, d.color_start.x));
    Bat wild;
    wild.rate = 1e9f;
    wild.box_xz_m = 1e9f;
    wild.size_m = 0.0f;
    wild.speed = 1e9f;
    const BatLook c = bat_look(wild, 1.0f);
    CHECK(c.rate == 60.0f);
    CHECK(c.box == 100.0f);
    CHECK(c.size == 0.02f);
    CHECK(c.speed_max == 8.0f);
    Bat e;
    e.manual = 0.2f;
    for (int i = 0; i < 50; ++i) {
        const f32 ph = 0.02f * static_cast<f32>(i);
        const f32 a1 = bat_amount(e, &s, ph, 0.5f, 0.3f, 0.1f);
        const f32 a2 = bat_amount(e, &s, ph, 0.5f, 0.3f, 0.1f);
        CHECK(same_bits(a1, a2));
        CHECK(same_vec(bat_look(e, a1).direction, bat_look(e, a2).direction));
        CHECK(same_bits(bat_look(e, a1).rate, bat_look(e, a2).rate));
    }
}
