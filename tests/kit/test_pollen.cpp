// Pollen (M-POLLEN 24a): the amount is 0 outside spring (both sides of the year wrap), 1 on the plateau (also across the
// wrap point with a window that straddles it) and continuous at the four edges; `manual` is a floor and works with no
// Season; the day gate closes at night when day_only and rain closes it; enabled 0, strength 0 and a missing component give
// 0 exactly; the look is inactive zeros at amount 0 and drifts along the wind; NaN and inverted ranges fall back; the
// helpers are deterministic.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>

#include "mce/kit/pollen.h"
#include "mce/kit/season.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
const math::vec2 kNoWind{0.0f, 0.0f};

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
bool same_vec(math::vec3 a, math::vec3 b) { return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z); }

bool is_zero_look(const PollenLook& l) {
    return !l.active && same_bits(l.rate, 0.0f) && same_bits(l.speed_min, 0.0f) && same_bits(l.speed_max, 0.0f) &&
           same_vec(l.direction, math::vec3{0.0f, 0.0f, 0.0f}) && same_bits(l.spread, 0.0f) &&
           same_vec(l.gravity, math::vec3{0.0f, 0.0f, 0.0f}) && same_bits(l.drag, 0.0f) && same_bits(l.size, 0.0f) &&
           same_bits(l.lifetime_min, 0.0f) && same_bits(l.lifetime_max, 0.0f) && same_bits(l.color_start.x, 0.0f) &&
           same_bits(l.color_start.w, 0.0f) && same_bits(l.color_end.w, 0.0f) && same_bits(l.box, 0.0f) &&
           same_bits(l.height, 0.0f);
}

const Season kSeason;
// Full day, dry.
f32 day(const Pollen& p, const Season* s, f32 phase) { return pollen_amount(p, s, phase, 0.0f, 0.0f); }

}  // namespace

TEST_CASE("pollen: defaults") {
    const Pollen p;
    CHECK(p.enabled == 1);
    CHECK(p.spring_start == 0.95f);
    CHECK(p.spring_full == 0.02f);
    CHECK(p.spring_end_full == 0.22f);
    CHECK(p.spring_end == 0.32f);
    CHECK(p.manual == 0.0f);
    CHECK(p.day_only == 1);
    CHECK(p.min_daylight == 0.5f);
    CHECK(p.rate == 14.0f);
    CHECK(p.box_xz_m == 18.0f);
    CHECK(p.height_m == 5.0f);
    CHECK(p.size_m == 0.06f);
    CHECK(p.lifetime_s == 12.0f);
    CHECK(p.fall_speed == 0.08f);
    CHECK(p.wind_follow == 0.6f);
    CHECK(same_vec(p.color, math::vec3{1.0f, 0.95f, 0.7f}));
    CHECK(p.max_particles == 512);
    CHECK(p.seed == 1);
    CHECK(p.strength == 1.0f);
    CHECK(p.pad_[0] == 0);
    CHECK(p.pad_[1] == 0);
}

TEST_CASE("pollen: the window is 0 outside spring on both sides of the year wrap") {
    const Pollen p;
    // after the window: 0.32 .. 0.9, and just before it starts: 0.9 .. 0.95
    for (f32 ph = 0.32f; ph < 0.95f; ph += 0.01f) CHECK(day(p, &kSeason, ph) == 0.0f);
    CHECK(day(p, &kSeason, 0.5f) == 0.0f);
    CHECK(day(p, &kSeason, 0.875f) == 0.0f);
    CHECK(day(p, &kSeason, 0.9f) == 0.0f);
    // the phase wraps
    CHECK(day(p, &kSeason, 1.5f) == 0.0f);
    CHECK(day(p, &kSeason, -0.5f) == 0.0f);
}

TEST_CASE("pollen: the window is 1 on the plateau and rises across the year start") {
    const Pollen p;
    CHECK(day(p, &kSeason, kSeasonSpringCentre) == 1.0f);
    for (f32 ph = 0.02f; ph <= 0.22f; ph += 0.01f) CHECK(day(p, &kSeason, ph) == doctest::Approx(1.0f));
    CHECK(day(p, &kSeason, kSeasonSpringCentre + 1.0f) == 1.0f);
    CHECK(day(p, &kSeason, kNaN) != 1.0f);  // NaN counts as the summer centre: outside spring
    // the rising edge straddles the wrap point: 0.97 < 0.99 < 0.0 < 0.01 are strictly increasing, all in (0, 1)
    const f32 a = day(p, &kSeason, 0.97f);
    const f32 b = day(p, &kSeason, 0.99f);
    const f32 c = day(p, &kSeason, 0.0f);
    const f32 d = day(p, &kSeason, 0.01f);
    CHECK(a > 0.0f);
    CHECK(a < b);
    CHECK(b < c);
    CHECK(c < d);
    CHECK(d < 1.0f);
    const f32 down = day(p, &kSeason, 0.27f);
    CHECK(down > 0.0f);
    CHECK(down < 1.0f);
}

TEST_CASE("pollen: a window that straddles the wrap point is 1 across it") {
    Pollen p;
    p.spring_start = 0.8f;
    p.spring_full = 0.9f;
    p.spring_end_full = 0.3f;
    p.spring_end = 0.4f;
    for (f32 ph : {0.9f, 0.95f, 0.99f, 0.0f, 0.05f, 0.125f, 0.3f}) CHECK(day(p, &kSeason, ph) == doctest::Approx(1.0f));
    CHECK(day(p, &kSeason, 0.7f) == 0.0f);
    CHECK(day(p, &kSeason, 0.5f) == 0.0f);
    CHECK(day(p, &kSeason, 0.85f) > 0.0f);
    CHECK(day(p, &kSeason, 0.85f) < 1.0f);
}

TEST_CASE("pollen: the amount is continuous at the four edges and symmetric about the centre") {
    const Pollen p;
    for (f32 edge : {0.95f, 0.02f, 0.22f, 0.32f}) {
        CHECK(std::fabs(day(p, &kSeason, edge - 1e-4f) - day(p, &kSeason, edge + 1e-4f)) < 1e-2f);
    }
    CHECK(day(p, &kSeason, 0.95f) == 0.0f);
    CHECK(day(p, &kSeason, 0.32f) == 0.0f);
    CHECK(day(p, &kSeason, 0.02f) == 1.0f);
    CHECK(day(p, &kSeason, 0.22f) == 1.0f);
    // the default edges are not mirrored (-0.175/-0.105 against +0.095/+0.195) but each side is monotonic
    f32 prev = -1.0f;
    for (f32 ph = 0.94f; ph < 1.0f; ph += 0.005f) {
        const f32 v = day(p, &kSeason, ph);
        CHECK(v >= prev);
        prev = v;
    }
    prev = 2.0f;
    for (f32 ph = 0.22f; ph < 0.33f; ph += 0.005f) {
        const f32 v = day(p, &kSeason, ph);
        CHECK(v <= prev);
        prev = v;
    }
}

TEST_CASE("pollen: manual is a floor and no Season means manual") {
    Pollen p;
    p.manual = 0.4f;
    CHECK(day(p, nullptr, 0.6f) == 0.4f);
    CHECK(day(p, nullptr, kSeasonSpringCentre) == 0.4f);
    CHECK(day(p, &kSeason, 0.6f) == 0.4f);
    CHECK(day(p, &kSeason, kSeasonSpringCentre) == 1.0f);
    Season off;
    off.enabled = 0;
    CHECK(day(p, &off, kSeasonSpringCentre) == 0.4f);
    CHECK(day(Pollen{}, nullptr, kSeasonSpringCentre) == 0.0f);
    Season zero;
    zero.strength = 0.0f;
    CHECK(day(Pollen{}, &zero, kSeasonSpringCentre) == 0.0f);
}

TEST_CASE("pollen: the day gate closes at night and rain closes it") {
    const Pollen p;
    const f32 c = kSeasonSpringCentre;
    CHECK(pollen_amount(p, &kSeason, c, 0.0f, 0.0f) == 1.0f);
    CHECK(pollen_amount(p, &kSeason, c, 0.5f, 0.0f) == 0.0f);
    CHECK(pollen_amount(p, &kSeason, c, 1.0f, 0.0f) == 0.0f);
    const f32 mid = pollen_amount(p, &kSeason, c, 0.25f, 0.0f);
    CHECK(mid > 0.0f);
    CHECK(mid < 1.0f);
    CHECK(pollen_amount(p, &kSeason, c, 0.0f, 1.0f) == 0.0f);
    CHECK(pollen_amount(p, &kSeason, c, 0.0f, 0.5f) == doctest::Approx(0.5f));
    // day_only 0 shows by night too; the manual floor is gated as well
    Pollen night;
    night.day_only = 0;
    CHECK(pollen_amount(night, &kSeason, c, 1.0f, 0.0f) == 1.0f);
    Pollen man;
    man.manual = 0.5f;
    CHECK(pollen_amount(man, nullptr, c, 1.0f, 0.0f) == 0.0f);
    CHECK(pollen_amount(man, nullptr, c, 0.0f, 0.0f) == 0.5f);
}

TEST_CASE("pollen: enabled 0, strength 0 and a missing component give exactly 0") {
    Pollen off;
    off.enabled = 0;
    off.manual = 1.0f;
    CHECK(day(off, &kSeason, kSeasonSpringCentre) == 0.0f);
    CHECK(is_zero_look(pollen_look(off, 1.0f, 3.0f, math::vec2{1.0f, 0.0f})));
    Pollen zero;
    zero.strength = 0.0f;
    zero.manual = 1.0f;
    CHECK(day(zero, &kSeason, kSeasonSpringCentre) == 0.0f);
    CHECK(is_zero_look(pollen_look(zero, 1.0f, 3.0f, math::vec2{1.0f, 0.0f})));
    const Pollen* none = nullptr;
    CHECK(pollen_amount(none, &kSeason, kSeasonSpringCentre, 0.0f, 0.0f) == 0.0f);
    CHECK(is_zero_look(pollen_look(none, 1.0f, 3.0f, math::vec2{1.0f, 0.0f})));
    Pollen half;
    half.strength = 0.5f;
    CHECK(day(half, &kSeason, kSeasonSpringCentre) == 0.5f);
}

TEST_CASE("pollen: the look is inactive at amount 0 and follows the authoring otherwise") {
    const Pollen p;
    CHECK(is_zero_look(pollen_look(p, 0.0f, 0.0f, kNoWind)));
    CHECK(is_zero_look(pollen_look(p, kNaN, 0.0f, kNoWind)));
    CHECK(is_zero_look(pollen_look(p, -1.0f, 0.0f, kNoWind)));
    const PollenLook l = pollen_look(p, 0.5f, 0.0f, kNoWind);
    CHECK(l.active);
    CHECK(l.rate == doctest::Approx(7.0f));
    CHECK(l.color_start.w == doctest::Approx(0.4f));
    CHECK(l.color_end.w == 0.0f);
    CHECK(l.color_start.x == 1.0f);
    CHECK(l.color_start.y == 0.95f);
    CHECK(l.color_start.z == 0.7f);
    CHECK(l.size == 0.06f);
    CHECK(l.lifetime_max == 12.0f);
    CHECK(l.lifetime_min < l.lifetime_max);
    CHECK(l.speed_min < l.speed_max);
    CHECK(l.speed_max == doctest::Approx(0.08f));
    CHECK(l.box == 18.0f);
    CHECK(l.height == 5.0f);
    CHECK(l.gravity.y < 0.0f);
    // terminal speed gravity / drag is the authored fall speed
    CHECK(-l.gravity.y / l.drag == doctest::Approx(0.08f));
    CHECK(same_vec(l.direction, math::vec3{0.0f, -1.0f, 0.0f}));
    CHECK(pollen_look(p, 5.0f, 0.0f, kNoWind).rate == 14.0f);
}

TEST_CASE("pollen: the wind tilts the drift along its direction") {
    const Pollen p;
    const PollenLook calm = pollen_look(p, 1.0f, 0.0f, math::vec2{1.0f, 0.0f});
    const PollenLook east = pollen_look(p, 1.0f, 3.0f, math::vec2{1.0f, 0.0f});
    const PollenLook south = pollen_look(p, 1.0f, 3.0f, math::vec2{0.0f, 1.0f});
    CHECK(calm.direction.x == 0.0f);
    CHECK(east.direction.x > 0.0f);
    CHECK(east.direction.z == 0.0f);
    CHECK(east.direction.y < 0.0f);
    CHECK(south.direction.z > 0.0f);
    CHECK(south.direction.x == 0.0f);
    CHECK(math::length(east.direction) == doctest::Approx(1.0f));
    CHECK(east.speed_max > calm.speed_max);
    // wind_follow 0 ignores the wind; NaN wind and direction count as 0
    Pollen deaf;
    deaf.wind_follow = 0.0f;
    CHECK(pollen_look(deaf, 1.0f, 3.0f, math::vec2{1.0f, 0.0f}).direction.x == 0.0f);
    CHECK(pollen_look(p, 1.0f, kNaN, math::vec2{1.0f, 0.0f}).direction.x == 0.0f);
    CHECK(pollen_look(p, 1.0f, 3.0f, math::vec2{kNaN, kNaN}).direction.x == 0.0f);
    // a storm is capped
    CHECK(pollen_look(p, 1.0f, 600.0f, math::vec2{1.0f, 0.0f}).speed_max == pollen_look(p, 1.0f, 6.0f, math::vec2{1.0f, 0.0f}).speed_max);
}

TEST_CASE("pollen: NaN, inverted and zero ranges fall back") {
    const Pollen def;
    const f32 want = day(def, &kSeason, 0.27f);
    const f32 want_up = day(def, &kSeason, 0.99f);
    Pollen n;
    n.spring_start = kNaN;
    n.spring_full = kNaN;
    n.spring_end_full = kNaN;
    n.spring_end = kNaN;
    n.manual = kNaN;
    n.strength = kNaN;
    n.min_daylight = kNaN;
    CHECK(day(n, &kSeason, 0.27f) == want);
    CHECK(day(n, &kSeason, 0.99f) == want_up);
    Pollen z;
    z.spring_start = 0.25f;
    z.spring_full = 0.25f;
    z.spring_end_full = 0.5f;
    z.spring_end = 0.5f;
    CHECK(day(z, &kSeason, 0.27f) == want);
    CHECK(day(z, &kSeason, 0.99f) == want_up);
    Pollen big;
    big.manual = 9.0f;
    big.strength = 9.0f;
    CHECK(day(big, nullptr, 0.6f) == 1.0f);
    // NaN gate inputs count as 0: full day, no rain
    CHECK(pollen_amount(def, &kSeason, kSeasonSpringCentre, kNaN, kNaN) == 1.0f);
    // out-of-range look inputs are clamped
    Pollen wild;
    wild.rate = 9999.0f;
    wild.size_m = 100.0f;
    wild.lifetime_s = 1000.0f;
    wild.box_xz_m = 1000.0f;
    wild.height_m = 0.0f;
    wild.fall_speed = kNaN;
    wild.color = math::vec3{kNaN, 5.0f, -1.0f};
    const PollenLook w = pollen_look(wild, 2.0f, 0.0f, kNoWind);
    CHECK(w.rate == 200.0f);
    CHECK(w.size == 0.5f);
    CHECK(w.lifetime_max == 30.0f);
    CHECK(w.box == 100.0f);
    CHECK(w.height == 1.0f);
    CHECK(w.speed_max == doctest::Approx(0.08f));
    CHECK(w.color_start.x == 1.0f);
    CHECK(w.color_start.y == 1.0f);
    CHECK(w.color_start.z == 0.0f);
    // fall_speed 0 stays finite
    Pollen still;
    still.fall_speed = 0.0f;
    const PollenLook s = pollen_look(still, 1.0f, 0.0f, kNoWind);
    CHECK(std::isfinite(s.drag));
    CHECK(s.drag > 0.0f);
}

TEST_CASE("pollen: the helpers are deterministic") {
    const Pollen p;
    for (int i = 0; i < 200; ++i) {
        const f32 ph = static_cast<f32>(i) * 0.005f;
        const f32 nf = static_cast<f32>(i % 11) * 0.1f;
        const f32 a = pollen_amount(p, &kSeason, ph, nf, 0.1f);
        const f32 b = pollen_amount(p, &kSeason, ph, nf, 0.1f);
        CHECK(same_bits(a, b));
        CHECK(a >= 0.0f);
        CHECK(a <= 1.0f);
        const PollenLook l1 = pollen_look(p, a, 2.0f, math::vec2{0.6f, 0.8f});
        const PollenLook l2 = pollen_look(p, a, 2.0f, math::vec2{0.6f, 0.8f});
        CHECK(same_bits(l1.rate, l2.rate));
        CHECK(same_bits(l1.color_start.w, l2.color_start.w));
        CHECK(same_vec(l1.direction, l2.direction));
    }
}
