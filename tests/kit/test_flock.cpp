// Flock (M-BIRDFLOCK 26a): the amount is the larger of two migration trapezoids (spring 0.125, autumn 0.625, wrapping the
// year start) or the manual floor, times the twilight gate and the dry weather; no Season gives the manual floor;
// enabled 0, strength 0 and a missing component give 0 exactly; the look is inactive zeros at amount 0 and a straight
// ground-plane stream along the heading otherwise; NaN and inverted ranges fall back; the helpers are deterministic.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>

#include "mce/kit/flock.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
bool same_vec(math::vec3 a, math::vec3 b) { return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z); }

bool is_zero_look(const FlockLook& l) {
    return !l.active && same_bits(l.rate, 0.0f) && same_bits(l.speed_min, 0.0f) && same_bits(l.speed_max, 0.0f) &&
           same_vec(l.direction, math::vec3{0.0f, 0.0f, 0.0f}) && same_bits(l.spread, 0.0f) &&
           same_vec(l.gravity, math::vec3{0.0f, 0.0f, 0.0f}) && same_bits(l.drag, 0.0f) && same_bits(l.size, 0.0f) &&
           same_bits(l.lifetime_min, 0.0f) && same_bits(l.lifetime_max, 0.0f) && same_bits(l.color_start.x, 0.0f) &&
           same_bits(l.color_start.w, 0.0f) && same_bits(l.color_end.w, 0.0f) && same_bits(l.box, 0.0f) &&
           same_bits(l.height, 0.0f);
}

}  // namespace

TEST_CASE("flock: defaults") {
    const Flock f;
    CHECK(f.enabled == 1);
    CHECK(f.migration_full == 0.06f);
    CHECK(f.migration_end == 0.12f);
    CHECK(f.manual == 0.0f);
    CHECK(f.twilight_width == 0.25f);
    CHECK(f.rate == 3.0f);
    CHECK(f.box_xz_m == 60.0f);
    CHECK(f.height_m == 45.0f);
    CHECK(f.size_m == 0.35f);
    CHECK(f.lifetime_s == 14.0f);
    CHECK(f.speed == 9.0f);
    CHECK(f.heading_deg == 70.0f);
    CHECK(same_vec(f.color, math::vec3{0.08f, 0.07f, 0.07f}));
    CHECK(f.max_particles == 64);
    CHECK(f.seed == 1);
    CHECK(f.strength == 1.0f);
    CHECK(f.pad_[0] == 0);
    CHECK(f.pad_[1] == 0);
    CHECK(f.pad_[2] == 0);
}

TEST_CASE("flock: the migration is a trapezoid around both season centres and wraps the year start") {
    const Flock f;
    const Season s;
    CHECK(flock_amount(f, &s, 0.125f, 0.0f, 0.0f) == 1.0f);
    CHECK(flock_amount(f, &s, 0.625f, 0.0f, 0.0f) == 1.0f);
    CHECK(flock_amount(f, &s, 0.125f + 0.06f, 0.0f, 0.0f) == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(flock_amount(f, &s, 0.125f + 0.09f, 0.0f, 0.0f) == doctest::Approx(0.5f).epsilon(1e-3));
    CHECK(flock_amount(f, &s, 0.125f + 0.13f, 0.0f, 0.0f) == 0.0f);
    CHECK(flock_amount(f, &s, 0.375f, 0.0f, 0.0f) == 0.0f);
    CHECK(flock_amount(f, &s, 0.875f, 0.0f, 0.0f) == 0.0f);
    // symmetric on both sides of a centre
    CHECK(flock_amount(f, &s, 0.625f - 0.09f, 0.0f, 0.0f) == doctest::Approx(flock_amount(f, &s, 0.625f + 0.09f, 0.0f, 0.0f)));
    // a window that reaches over the year start: centre 0.125 with half-width 0.2 covers phase 0.95
    Flock wide;
    wide.migration_full = 0.2f;
    wide.migration_end = 0.3f;
    CHECK(flock_amount(wide, &s, 0.95f, 0.0f, 0.0f) == 1.0f);
    CHECK(flock_amount(wide, &s, 0.0f, 0.0f, 0.0f) == 1.0f);
    CHECK(flock_amount(wide, &s, 1.95f, 0.0f, 0.0f) == 1.0f);
    CHECK(flock_amount(wide, &s, -0.05f, 0.0f, 0.0f) == 1.0f);
    f32 prev = -1.0f;
    for (int i = 0; i <= 200; ++i) {
        const f32 a = flock_amount(f, &s, 0.005f * static_cast<f32>(i), 0.0f, 0.0f);
        CHECK(a >= 0.0f);
        CHECK(a <= 1.0f);
        if (prev >= 0.0f) CHECK(std::fabs(a - prev) < 0.2f);
        prev = a;
    }
}

TEST_CASE("flock: the twilight gate opens at the horizon and closes in the day and the night") {
    const Flock f;
    const Season s;
    CHECK(flock_amount(f, &s, 0.125f, 0.0f, 0.0f) == 1.0f);
    CHECK(flock_amount(f, &s, 0.125f, 0.25f, 0.0f) == 0.0f);
    CHECK(flock_amount(f, &s, 0.125f, -0.25f, 0.0f) == 0.0f);
    CHECK(flock_amount(f, &s, 0.125f, 0.9f, 0.0f) == 0.0f);
    CHECK(flock_amount(f, &s, 0.125f, -1.0f, 0.0f) == 0.0f);
    CHECK(flock_amount(f, &s, 0.125f, 0.125f, 0.0f) == doctest::Approx(0.5f));
    CHECK(flock_amount(f, &s, 0.125f, -0.125f, 0.0f) == doctest::Approx(0.5f));
    CHECK(flock_amount(f, &s, 0.125f, kNaN, 0.0f) == 0.0f);
}

TEST_CASE("flock: manual is a floor, no Season gives the floor only, rain closes the amount") {
    Flock f;
    f.manual = 0.4f;
    const Season s;
    CHECK(flock_amount(f, &s, 0.375f, 0.0f, 0.0f) == 0.4f);
    CHECK(flock_amount(f, nullptr, 0.125f, 0.0f, 0.0f) == 0.4f);
    Season off;
    off.enabled = 0;
    CHECK(flock_amount(f, &off, 0.125f, 0.0f, 0.0f) == 0.4f);
    CHECK(flock_amount(f, &s, 0.125f, 0.0f, 0.0f) == 1.0f);
    CHECK(flock_amount(f, &s, 0.125f, 0.0f, 1.0f) == 0.0f);
    CHECK(flock_amount(f, &s, 0.375f, 0.0f, 0.5f) == doctest::Approx(0.2f));
    CHECK(flock_amount(Flock{}, nullptr, 0.125f, 0.0f, 0.0f) == 0.0f);
    f.strength = 0.5f;
    CHECK(flock_amount(f, &s, 0.125f, 0.0f, 0.0f) == doctest::Approx(0.5f));
}

TEST_CASE("flock: enabled 0, strength 0 and a missing component give exactly 0") {
    Flock off;
    off.enabled = 0;
    off.manual = 1.0f;
    const Season s;
    CHECK(same_bits(flock_amount(off, &s, 0.125f, 0.0f, 0.0f), 0.0f));
    Flock zero;
    zero.strength = 0.0f;
    zero.manual = 1.0f;
    CHECK(same_bits(flock_amount(zero, &s, 0.125f, 0.0f, 0.0f), 0.0f));
    CHECK(same_bits(flock_amount(static_cast<const Flock*>(nullptr), &s, 0.125f, 0.0f, 0.0f), 0.0f));
    CHECK(is_zero_look(flock_look(off, 1.0f)));
    CHECK(is_zero_look(flock_look(zero, 1.0f)));
    CHECK(is_zero_look(flock_look(static_cast<const Flock*>(nullptr), 1.0f)));
}

TEST_CASE("flock: the look is inactive at amount 0 and follows the authoring otherwise") {
    const Flock f;
    CHECK(is_zero_look(flock_look(f, 0.0f)));
    CHECK(is_zero_look(flock_look(f, kNaN)));
    CHECK(is_zero_look(flock_look(f, -1.0f)));
    const FlockLook l = flock_look(f, 0.5f);
    CHECK(l.active);
    CHECK(l.rate == doctest::Approx(1.5f));
    CHECK(l.color_start.w == doctest::Approx(0.425f));
    CHECK(l.color_end.w == 0.0f);
    CHECK(l.color_start.x == 0.08f);
    CHECK(l.color_start.y == 0.07f);
    CHECK(l.color_start.z == 0.07f);
    CHECK(l.size == 0.35f);
    CHECK(l.lifetime_max == 14.0f);
    CHECK(l.lifetime_min < l.lifetime_max);
    CHECK(l.speed_min < l.speed_max);
    CHECK(l.speed_max == 9.0f);
    CHECK(l.box == 60.0f);
    CHECK(l.height == 45.0f);
    CHECK(l.gravity.x == 0.0f);
    CHECK(l.gravity.y == 0.0f);
    CHECK(l.gravity.z == 0.0f);
    CHECK(l.drag >= 0.0f);
    CHECK(l.drag <= 0.05f);
    CHECK(flock_look(f, 5.0f).rate == 3.0f);
}

TEST_CASE("flock: the heading sets a unit direction on the ground plane") {
    Flock f;
    f.heading_deg = 0.0f;
    FlockLook l = flock_look(f, 1.0f);
    CHECK(l.direction.z == doctest::Approx(1.0f));
    CHECK(l.direction.x == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(l.direction.y == 0.0f);
    f.heading_deg = 90.0f;
    l = flock_look(f, 1.0f);
    CHECK(l.direction.x == doctest::Approx(1.0f));
    CHECK(l.direction.z == doctest::Approx(0.0f).epsilon(1e-6));
    f.heading_deg = 180.0f;
    CHECK(flock_look(f, 1.0f).direction.z == doctest::Approx(-1.0f));
    f.heading_deg = 70.0f;
    l = flock_look(f, 1.0f);
    CHECK(l.direction.x > 0.9f);
    CHECK(l.direction.z > 0.3f);
    CHECK(l.direction.y == 0.0f);
    CHECK(math::length(l.direction) == doctest::Approx(1.0f));
    // NaN falls back to the default heading
    f.heading_deg = kNaN;
    CHECK(same_vec(flock_look(f, 1.0f).direction, l.direction));
}

TEST_CASE("flock: NaN, inverted and out-of-range authoring fall back or clamp; the helpers are deterministic") {
    const Flock def;
    const Season s;
    const f32 want = flock_amount(def, &s, 0.125f + 0.09f, 0.1f, 0.0f);
    Flock inv;
    inv.migration_full = 0.12f;
    inv.migration_end = 0.06f;
    CHECK(flock_amount(inv, &s, 0.125f + 0.09f, 0.1f, 0.0f) == want);
    Flock empty;
    empty.migration_full = 0.1f;
    empty.migration_end = 0.1f;
    CHECK(flock_amount(empty, &s, 0.125f + 0.09f, 0.1f, 0.0f) == want);
    Flock nan;
    nan.migration_full = kNaN;
    nan.migration_end = kNaN;
    nan.twilight_width = kNaN;
    nan.manual = kNaN;
    nan.strength = kNaN;
    CHECK(flock_amount(nan, &s, 0.125f + 0.09f, 0.1f, 0.0f) == want);
    CHECK(flock_amount(def, &s, kNaN, 0.0f, 0.0f) == 0.0f);
    CHECK(flock_amount(def, &s, 0.125f, 0.0f, kNaN) == 1.0f);
    Flock bad;
    bad.rate = kNaN;
    bad.size_m = kNaN;
    bad.lifetime_s = kNaN;
    bad.speed = kNaN;
    bad.box_xz_m = kNaN;
    bad.height_m = kNaN;
    bad.color = math::vec3{kNaN, kNaN, kNaN};
    const FlockLook a = flock_look(bad, 1.0f);
    const FlockLook b = flock_look(def, 1.0f);
    CHECK(a.rate == b.rate);
    CHECK(a.size == b.size);
    CHECK(a.lifetime_max == b.lifetime_max);
    CHECK(a.speed_max == b.speed_max);
    CHECK(a.box == b.box);
    CHECK(a.height == b.height);
    CHECK(same_bits(a.color_start.x, b.color_start.x));
    Flock wild;
    wild.rate = 1e9f;
    wild.box_xz_m = 1e9f;
    wild.size_m = 0.0f;
    const FlockLook c = flock_look(wild, 1.0f);
    CHECK(c.rate == 60.0f);
    CHECK(c.box == 200.0f);
    CHECK(c.size == 0.05f);
    Flock d;
    d.manual = 0.2f;
    for (int i = 0; i < 50; ++i) {
        const f32 ph = 0.02f * static_cast<f32>(i);
        const f32 a1 = flock_amount(d, &s, ph, 0.05f, 0.1f);
        const f32 a2 = flock_amount(d, &s, ph, 0.05f, 0.1f);
        CHECK(same_bits(a1, a2));
        CHECK(same_vec(flock_look(d, a1).direction, flock_look(d, a2).direction));
        CHECK(same_bits(flock_look(d, a1).rate, flock_look(d, a2).rate));
    }
}
