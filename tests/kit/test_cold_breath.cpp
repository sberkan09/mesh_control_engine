// ColdBreath (M-COLDBREATH 34a): the amount is the winter trapezoid (centre 0.875, wrapping across phase 0/1), the settled
// snow cover or the manual floor, times the calm-wind gate; no Season gives the cover and manual only; enabled 0, strength 0
// and a missing component give 0 exactly; the look is inactive zeros at amount 0 and soft pale growing puffs otherwise; NaN
// and inverted ranges fall back; the helpers are deterministic.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>

#include "mce/kit/cold_breath.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
constexpr f32 kWinter = 0.875f;

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
bool same_vec(math::vec3 a, math::vec3 b) { return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z); }

bool is_zero_look(const ColdBreathLook& l) {
    return !l.active && same_bits(l.rate, 0.0f) && same_bits(l.speed_min, 0.0f) && same_bits(l.speed_max, 0.0f) &&
           same_bits(l.lift, 0.0f) && same_bits(l.drag, 0.0f) && same_bits(l.size_start, 0.0f) &&
           same_bits(l.size_end, 0.0f) && same_bits(l.lifetime_min, 0.0f) && same_bits(l.lifetime_max, 0.0f) &&
           same_bits(l.color_start.x, 0.0f) && same_bits(l.color_start.y, 0.0f) && same_bits(l.color_start.z, 0.0f) &&
           same_bits(l.color_start.w, 0.0f) && same_bits(l.color_end.x, 0.0f) && same_bits(l.color_end.w, 0.0f) &&
           same_bits(l.mouth_forward, 0.0f) && same_bits(l.mouth_down, 0.0f);
}

}  // namespace

TEST_CASE("cold breath: defaults") {
    const ColdBreath b;
    CHECK(b.enabled == 1);
    CHECK(b.window_full == 0.08f);
    CHECK(b.window_end == 0.16f);
    CHECK(b.manual == 0.0f);
    CHECK(b.cover_low == 0.15f);
    CHECK(b.cover_full == 0.6f);
    CHECK(b.wind_calm == 0.5f);
    CHECK(b.wind_still == 0.95f);
    CHECK(b.rate == 0.8f);
    CHECK(b.mouth_forward_m == 0.35f);
    CHECK(b.mouth_down_m == 0.25f);
    CHECK(b.size_m == 0.12f);
    CHECK(b.grow == 2.5f);
    CHECK(b.lifetime_s == 1.2f);
    CHECK(b.speed == 0.35f);
    CHECK(b.lift == 0.15f);
    CHECK(same_vec(b.color, math::vec3{0.9f, 0.93f, 0.97f}));
    CHECK(b.max_particles == 24);
    CHECK(b.seed == 1);
    CHECK(b.strength == 1.0f);
    CHECK(b.pad_[0] == 0);
    CHECK(b.pad_[1] == 0);
    CHECK(b.pad_[2] == 0);
}

TEST_CASE("cold breath: the window is a trapezoid around the winter centre and zero in the other seasons") {
    const ColdBreath b;
    const Season s;
    CHECK(cold_breath_amount(b, &s, kWinter, 0.0f, 0.0f) == 1.0f);
    CHECK(cold_breath_amount(b, &s, kWinter + 0.08f, 0.0f, 0.0f) == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(cold_breath_amount(b, &s, kWinter + 0.12f, 0.0f, 0.0f) == doctest::Approx(0.5f).epsilon(1e-3));
    CHECK(cold_breath_amount(b, &s, kWinter - 0.12f, 0.0f, 0.0f) ==
          doctest::Approx(cold_breath_amount(b, &s, kWinter + 0.12f, 0.0f, 0.0f)));
    CHECK(cold_breath_amount(b, &s, kWinter - 0.25f, 0.0f, 0.0f) == 0.0f);
    CHECK(cold_breath_amount(b, &s, 0.375f, 0.0f, 0.0f) == 0.0f);  // summer
    CHECK(cold_breath_amount(b, &s, 0.125f, 0.0f, 0.0f) == 0.0f);  // spring
    for (int i = 0; i <= 200; ++i) {
        const f32 a = cold_breath_amount(b, &s, 0.005f * static_cast<f32>(i), 0.0f, 0.0f);
        CHECK(a >= 0.0f);
        CHECK(a <= 1.0f);
    }
}

TEST_CASE("cold breath: the window wraps across phase 0 and 1") {
    const ColdBreath b;
    const Season s;
    // Phase 0 is 0.125 from the centre on the late side: the same as 0.75 on the early side, partway down the slope.
    const f32 at0 = cold_breath_amount(b, &s, 0.0f, 0.0f, 0.0f);
    CHECK(at0 > 0.0f);
    CHECK(at0 < 1.0f);
    CHECK(at0 == doctest::Approx(cold_breath_amount(b, &s, 0.75f, 0.0f, 0.0f)));
    CHECK(same_bits(cold_breath_amount(b, &s, 1.0f, 0.0f, 0.0f), at0));
    CHECK(cold_breath_amount(b, &s, 0.03f, 0.0f, 0.0f) > 0.0f);   // 0.155 from the centre
    CHECK(cold_breath_amount(b, &s, 0.94f, 0.0f, 0.0f) == 1.0f);  // 0.065 from the centre, inside the full range
    CHECK(cold_breath_amount(b, &s, 0.94f, 0.0f, 0.0f) >= cold_breath_amount(b, &s, 1.02f, 0.0f, 0.0f));
    CHECK(cold_breath_amount(b, &s, 0.04f, 0.0f, 0.0f) == 0.0f);  // 0.165 from the centre
}

TEST_CASE("cold breath: the snow cover brings breath out of season, smoothed between cover_low and cover_full") {
    const ColdBreath b;
    const Season s;
    CHECK(cold_breath_amount(b, &s, 0.375f, 0.0f, 0.0f) == 0.0f);
    CHECK(cold_breath_amount(b, &s, 0.375f, 0.15f, 0.0f) == 0.0f);
    CHECK(cold_breath_amount(b, &s, 0.375f, 0.375f, 0.0f) == doctest::Approx(0.5f));
    CHECK(cold_breath_amount(b, &s, 0.375f, 0.6f, 0.0f) == 1.0f);
    CHECK(cold_breath_amount(b, &s, 0.375f, 1.0f, 0.0f) == 1.0f);
    CHECK(cold_breath_amount(b, &s, 0.375f, kNaN, 0.0f) == 0.0f);
    CHECK(cold_breath_amount(b, nullptr, 0.375f, 1.0f, 0.0f) == 1.0f);  // no Season: the cover still counts
    // The cover and the window combine by max, not by sum.
    CHECK(cold_breath_amount(b, &s, kWinter, 1.0f, 0.0f) == 1.0f);
    f32 prev = 0.0f;
    for (int i = 0; i <= 20; ++i) {
        const f32 a = cold_breath_amount(b, &s, 0.375f, 0.05f * static_cast<f32>(i), 0.0f);
        CHECK(a >= prev);
        prev = a;
    }
}

TEST_CASE("cold breath: manual is a floor that ignores the window and cover but not the wind; no Season gives the floor") {
    ColdBreath b;
    b.manual = 0.4f;
    const Season s;
    CHECK(cold_breath_amount(b, &s, 0.375f, 0.0f, 0.0f) == 0.4f);
    CHECK(cold_breath_amount(b, &s, kWinter, 0.0f, 0.0f) == 1.0f);
    CHECK(cold_breath_amount(b, nullptr, kWinter, 0.0f, 0.0f) == 0.4f);
    Season off;
    off.enabled = 0;
    CHECK(cold_breath_amount(b, &off, kWinter, 0.0f, 0.0f) == 0.4f);
    CHECK(cold_breath_amount(b, nullptr, kWinter, 0.0f, 1.0f) == 0.0f);
    const ColdBreath none;
    CHECK(cold_breath_amount(none, nullptr, kWinter, 0.0f, 0.0f) == 0.0f);
}

TEST_CASE("cold breath: the calm gate scatters the vapour in wind") {
    const ColdBreath b;
    const Season s;
    CHECK(cold_breath_amount(b, &s, kWinter, 0.0f, 0.0f) == 1.0f);
    CHECK(cold_breath_amount(b, &s, kWinter, 0.0f, 0.5f) == 1.0f);
    CHECK(cold_breath_amount(b, &s, kWinter, 0.0f, 0.725f) == doctest::Approx(0.5f));
    CHECK(cold_breath_amount(b, &s, kWinter, 0.0f, 0.95f) == 0.0f);
    CHECK(cold_breath_amount(b, &s, kWinter, 0.0f, 1.0f) == 0.0f);
    CHECK(cold_breath_amount(b, &s, kWinter, 0.0f, kNaN) == 1.0f);
}

TEST_CASE("cold breath: enabled 0, strength 0 and a missing component give exactly 0") {
    const Season s;
    ColdBreath off;
    off.enabled = 0;
    off.manual = 1.0f;
    CHECK(same_bits(cold_breath_amount(off, &s, kWinter, 1.0f, 0.0f), 0.0f));
    ColdBreath zero;
    zero.strength = 0.0f;
    zero.manual = 1.0f;
    CHECK(same_bits(cold_breath_amount(zero, &s, kWinter, 1.0f, 0.0f), 0.0f));
    CHECK(same_bits(cold_breath_amount(static_cast<const ColdBreath*>(nullptr), &s, kWinter, 1.0f, 0.0f), 0.0f));
    CHECK(is_zero_look(cold_breath_look(off, 1.0f)));
    CHECK(is_zero_look(cold_breath_look(zero, 1.0f)));
    CHECK(is_zero_look(cold_breath_look(static_cast<const ColdBreath*>(nullptr), 1.0f)));
    ColdBreath half;
    half.strength = 0.5f;
    CHECK(cold_breath_amount(half, &s, kWinter, 0.0f, 0.0f) == doctest::Approx(0.5f));
}

TEST_CASE("cold breath: the look is inactive at amount 0 and follows the authoring otherwise") {
    const ColdBreath b;
    CHECK(is_zero_look(cold_breath_look(b, 0.0f)));
    CHECK(is_zero_look(cold_breath_look(b, kNaN)));
    CHECK(is_zero_look(cold_breath_look(b, -1.0f)));
    const ColdBreathLook l = cold_breath_look(b, 0.5f);
    CHECK(l.active);
    CHECK(l.rate == doctest::Approx(0.4f));
    CHECK(l.color_start.w == doctest::Approx(0.175f));
    CHECK(l.color_end.w == 0.0f);
    CHECK(l.color_start.x == 0.9f);
    CHECK(l.color_start.y == 0.93f);
    CHECK(l.color_start.z == 0.97f);
    CHECK(same_bits(l.color_end.x, l.color_start.x));
    CHECK(l.size_start == 0.12f);
    CHECK(l.size_end == doctest::Approx(0.3f));
    CHECK(l.size_end > l.size_start);
    CHECK(l.lifetime_max == 1.2f);
    CHECK(l.lifetime_min < l.lifetime_max);
    CHECK(l.speed_max == 0.35f);
    CHECK(l.speed_min <= l.speed_max);
    CHECK(l.lift == 0.15f);
    CHECK(l.drag > 0.0f);
    CHECK(l.mouth_forward == 0.35f);
    CHECK(l.mouth_down == 0.25f);
    CHECK(cold_breath_look(b, 5.0f).rate == 0.8f);
    CHECK(cold_breath_look(b, 5.0f).color_start.w == doctest::Approx(0.35f));
}

TEST_CASE("cold breath: NaN, inverted and out-of-range authoring fall back or clamp; the helpers are deterministic") {
    const ColdBreath def;
    const Season s;
    const f32 ph = kWinter + 0.12f;
    const f32 want = cold_breath_amount(def, &s, ph, 0.3f, 0.6f);
    ColdBreath inv;
    inv.window_full = 0.2f;
    inv.window_end = 0.12f;
    inv.cover_low = 0.7f;
    inv.cover_full = 0.2f;
    inv.wind_calm = 0.9f;
    inv.wind_still = 0.3f;
    CHECK(cold_breath_amount(inv, &s, ph, 0.3f, 0.6f) == want);
    ColdBreath empty;
    empty.window_full = 0.3f;
    empty.window_end = 0.3f;
    empty.cover_low = 0.5f;
    empty.cover_full = 0.5f;
    empty.wind_calm = 0.5f;
    empty.wind_still = 0.5f;
    CHECK(cold_breath_amount(empty, &s, ph, 0.3f, 0.6f) == want);
    ColdBreath nan;
    nan.window_full = kNaN;
    nan.window_end = kNaN;
    nan.cover_low = kNaN;
    nan.cover_full = kNaN;
    nan.wind_calm = kNaN;
    nan.wind_still = kNaN;
    nan.manual = kNaN;
    nan.strength = kNaN;
    CHECK(cold_breath_amount(nan, &s, ph, 0.3f, 0.6f) == want);
    ColdBreath bad;
    bad.rate = kNaN;
    bad.size_m = kNaN;
    bad.grow = kNaN;
    bad.lifetime_s = kNaN;
    bad.speed = kNaN;
    bad.lift = kNaN;
    bad.mouth_forward_m = kNaN;
    bad.mouth_down_m = kNaN;
    bad.color = math::vec3{kNaN, kNaN, kNaN};
    const ColdBreathLook a = cold_breath_look(bad, 1.0f);
    const ColdBreathLook d = cold_breath_look(def, 1.0f);
    CHECK(a.rate == d.rate);
    CHECK(a.size_start == d.size_start);
    CHECK(a.size_end == d.size_end);
    CHECK(a.lifetime_max == d.lifetime_max);
    CHECK(a.speed_max == d.speed_max);
    CHECK(a.lift == d.lift);
    CHECK(a.mouth_forward == d.mouth_forward);
    CHECK(a.mouth_down == d.mouth_down);
    CHECK(same_bits(a.color_start.x, d.color_start.x));
    ColdBreath wild;
    wild.rate = 1e9f;
    wild.size_m = 0.0f;
    wild.grow = 0.0f;
    wild.lifetime_s = 0.0f;
    wild.speed = 1e9f;
    wild.lift = 1e9f;
    wild.mouth_forward_m = 1e9f;
    wild.mouth_down_m = 1e9f;
    const ColdBreathLook c = cold_breath_look(wild, 1.0f);
    CHECK(c.rate == 10.0f);
    CHECK(c.size_start == 0.01f);
    CHECK(c.size_end == 0.01f);  // grow clamps up to 1
    CHECK(c.lifetime_max == 0.3f);
    CHECK(c.speed_max == 3.0f);
    CHECK(c.lift == 2.0f);
    CHECK(c.mouth_forward == 2.0f);
    CHECK(c.mouth_down == 2.0f);
    ColdBreath e;
    e.manual = 0.2f;
    for (int i = 0; i < 50; ++i) {
        const f32 cover = 0.02f * static_cast<f32>(i);
        const f32 a1 = cold_breath_amount(e, &s, ph, cover, 0.6f);
        const f32 a2 = cold_breath_amount(e, &s, ph, cover, 0.6f);
        CHECK(same_bits(a1, a2));
        CHECK(a1 >= 0.0f);
        CHECK(a1 <= 1.0f);
        CHECK(same_bits(cold_breath_look(e, a1).rate, cold_breath_look(e, a2).rate));
        CHECK(same_bits(cold_breath_look(e, a1).color_start.w, cold_breath_look(e, a2).color_start.w));
    }
}
