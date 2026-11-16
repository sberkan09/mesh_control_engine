// Firefly (M-FIREFLIES 23a): the amount is 0 outside summer, 1 on the plateau at night and continuous at the edges; `manual`
// is a floor and works with no Season; the night gate closes by day and rain closes it; enabled 0, strength 0 and a missing
// component give 0 exactly; the look is inactive zeros at amount 0; NaN and inverted ranges fall back; the helpers are
// deterministic; the component round-trips through the reflection defaults.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>

#include "mce/kit/firefly.h"
#include "mce/kit/season.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
bool same_vec(math::vec3 a, math::vec3 b) { return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z); }

bool is_zero_look(const FireflyLook& l) {
    return !l.active && same_bits(l.rate, 0.0f) && same_bits(l.speed_min, 0.0f) && same_bits(l.speed_max, 0.0f) &&
           same_vec(l.direction, math::vec3{0.0f, 0.0f, 0.0f}) && same_bits(l.spread, 0.0f) &&
           same_vec(l.gravity, math::vec3{0.0f, 0.0f, 0.0f}) && same_bits(l.drag, 0.0f) && same_bits(l.size, 0.0f) &&
           same_bits(l.lifetime_min, 0.0f) && same_bits(l.lifetime_max, 0.0f) && same_bits(l.color_start.x, 0.0f) &&
           same_bits(l.color_start.w, 0.0f) && same_bits(l.color_end.w, 0.0f) && same_bits(l.box, 0.0f) &&
           same_bits(l.height, 0.0f);
}

const Season kSeason;
// Full night, clear, dry.
f32 night(const Firefly& f, const Season* s, f32 phase) { return firefly_amount(f, s, phase, 1.0f, 0.0f, 0.0f); }

}  // namespace

TEST_CASE("firefly: defaults") {
    const Firefly f;
    CHECK(f.enabled == 1);
    CHECK(f.summer_start == 0.2f);
    CHECK(f.summer_full == 0.3f);
    CHECK(f.summer_end_full == 0.45f);
    CHECK(f.summer_end == 0.55f);
    CHECK(f.manual == 0.0f);
    CHECK(f.night_only == 1);
    CHECK(f.min_darkness == 0.6f);
    CHECK(f.rate == 18.0f);
    CHECK(f.box_xz_m == 14.0f);
    CHECK(f.height_m == 3.0f);
    CHECK(f.size_m == 0.08f);
    CHECK(f.lifetime_s == 6.0f);
    CHECK(f.drift_speed == 0.25f);
    CHECK(same_vec(f.color, math::vec3{0.8f, 1.0f, 0.35f}));
    CHECK(f.max_particles == 256);
    CHECK(f.seed == 1);
    CHECK(f.strength == 1.0f);
    CHECK(f.pad_[0] == 0);
    CHECK(f.pad_[1] == 0);
}

TEST_CASE("firefly: the window is 0 outside summer, 1 on the plateau and continuous") {
    const Firefly f;
    for (f32 p = 0.0f; p <= 0.2f; p += 0.01f) CHECK(night(f, &kSeason, p) == 0.0f);
    for (f32 p = 0.55f; p < 1.0f; p += 0.01f) CHECK(night(f, &kSeason, p) == 0.0f);
    CHECK(night(f, &kSeason, 0.3f) == 1.0f);
    CHECK(night(f, &kSeason, kSeasonSummerCentre) == 1.0f);
    CHECK(night(f, &kSeason, 0.45f) == 1.0f);
    const f32 up = night(f, &kSeason, 0.25f);
    CHECK(up > 0.0f);
    CHECK(up < 1.0f);
    const f32 down = night(f, &kSeason, 0.5f);
    CHECK(down > 0.0f);
    CHECK(down < 1.0f);
    for (f32 edge : {0.2f, 0.3f, 0.45f, 0.55f}) {
        CHECK(std::fabs(night(f, &kSeason, edge - 1e-4f) - night(f, &kSeason, edge + 1e-4f)) < 1e-2f);
    }
    // symmetric about the summer centre
    for (f32 d = 0.0f; d <= 0.175f; d += 0.005f) {
        CHECK(night(f, &kSeason, kSeasonSummerCentre - d) ==
              doctest::Approx(night(f, &kSeason, kSeasonSummerCentre + d)).epsilon(1e-4));
    }
    // the phase wraps and NaN is the summer centre
    CHECK(night(f, &kSeason, kSeasonSummerCentre + 1.0f) == 1.0f);
    CHECK(night(f, &kSeason, kNaN) == 1.0f);
}

TEST_CASE("firefly: manual is a floor and no Season means manual") {
    Firefly f;
    f.manual = 0.4f;
    CHECK(night(f, nullptr, 0.9f) == 0.4f);
    CHECK(night(f, nullptr, kSeasonSummerCentre) == 0.4f);
    CHECK(night(f, &kSeason, 0.9f) == 0.4f);
    CHECK(night(f, &kSeason, kSeasonSummerCentre) == 1.0f);
    Season off;
    off.enabled = 0;
    CHECK(night(f, &off, kSeasonSummerCentre) == 0.4f);
    CHECK(night(Firefly{}, nullptr, kSeasonSummerCentre) == 0.0f);
    Season zero;
    zero.strength = 0.0f;
    CHECK(night(Firefly{}, &zero, kSeasonSummerCentre) == 0.0f);
}

TEST_CASE("firefly: the night gate closes by day and rain closes it") {
    const Firefly f;
    const f32 c = kSeasonSummerCentre;
    CHECK(firefly_amount(f, &kSeason, c, 0.0f, 0.0f, 0.0f) == 0.0f);
    CHECK(firefly_amount(f, &kSeason, c, 0.6f, 0.0f, 0.0f) == 0.0f);
    CHECK(firefly_amount(f, &kSeason, c, 1.0f, 0.0f, 0.0f) == 1.0f);
    const f32 mid = firefly_amount(f, &kSeason, c, 0.8f, 0.0f, 0.0f);
    CHECK(mid > 0.0f);
    CHECK(mid < 1.0f);
    CHECK(firefly_amount(f, &kSeason, c, 1.0f, 0.0f, 1.0f) == 0.0f);
    CHECK(firefly_amount(f, &kSeason, c, 1.0f, 0.0f, 0.5f) == doctest::Approx(0.5f));
    // cloud cover does not change the amount
    CHECK(firefly_amount(f, &kSeason, c, 1.0f, 1.0f, 0.0f) == 1.0f);
    // night_only 0 shows by day, manual floor is gated too
    Firefly day;
    day.night_only = 0;
    CHECK(firefly_amount(day, &kSeason, c, 0.0f, 0.0f, 0.0f) == 1.0f);
    Firefly man;
    man.manual = 0.5f;
    CHECK(firefly_amount(man, nullptr, c, 0.0f, 0.0f, 0.0f) == 0.0f);
    CHECK(firefly_amount(man, nullptr, c, 1.0f, 0.0f, 0.0f) == 0.5f);
}

TEST_CASE("firefly: enabled 0, strength 0 and a missing component give exactly 0") {
    Firefly off;
    off.enabled = 0;
    off.manual = 1.0f;
    CHECK(night(off, &kSeason, kSeasonSummerCentre) == 0.0f);
    CHECK(is_zero_look(firefly_look(off, 1.0f)));
    Firefly zero;
    zero.strength = 0.0f;
    zero.manual = 1.0f;
    CHECK(night(zero, &kSeason, kSeasonSummerCentre) == 0.0f);
    CHECK(is_zero_look(firefly_look(zero, 1.0f)));
    const Firefly* none = nullptr;
    CHECK(firefly_amount(none, &kSeason, kSeasonSummerCentre, 1.0f, 0.0f, 0.0f) == 0.0f);
    CHECK(is_zero_look(firefly_look(none, 1.0f)));
    Firefly half;
    half.strength = 0.5f;
    CHECK(night(half, &kSeason, kSeasonSummerCentre) == 0.5f);
}

TEST_CASE("firefly: the look is inactive at amount 0 and follows the authoring otherwise") {
    const Firefly f;
    CHECK(is_zero_look(firefly_look(f, 0.0f)));
    CHECK(is_zero_look(firefly_look(f, kNaN)));
    CHECK(is_zero_look(firefly_look(f, -1.0f)));
    const FireflyLook l = firefly_look(f, 0.5f);
    CHECK(l.active);
    CHECK(l.rate == doctest::Approx(9.0f));
    CHECK(l.color_start.w == doctest::Approx(0.45f));
    CHECK(l.color_end.w == 0.0f);
    CHECK(l.color_start.x == 0.8f);
    CHECK(l.color_start.y == 1.0f);
    CHECK(l.color_start.z == 0.35f);
    CHECK(l.size == 0.08f);
    CHECK(l.lifetime_max == 6.0f);
    CHECK(l.lifetime_min < l.lifetime_max);
    CHECK(l.speed_min < l.speed_max);
    CHECK(l.speed_max == 0.25f);
    CHECK(l.box == 14.0f);
    CHECK(l.height == 3.0f);
    CHECK(l.drag > 0.0f);
    CHECK(std::fabs(l.gravity.y) < 0.1f);
    CHECK(math::length(l.direction) == doctest::Approx(1.0f));
    // amounts above 1 clamp
    CHECK(firefly_look(f, 5.0f).rate == 18.0f);
}

TEST_CASE("firefly: NaN, inverted and zero ranges fall back") {
    const Firefly def;
    const f32 want = night(def, &kSeason, 0.25f);
    Firefly n;
    n.summer_start = kNaN;
    n.summer_full = kNaN;
    n.summer_end_full = kNaN;
    n.summer_end = kNaN;
    n.manual = kNaN;
    n.strength = kNaN;
    n.min_darkness = kNaN;
    CHECK(night(n, &kSeason, 0.25f) == want);
    Firefly z;
    z.summer_start = 0.25f;
    z.summer_full = 0.25f;
    z.summer_end_full = 0.5f;
    z.summer_end = 0.5f;
    CHECK(night(z, &kSeason, 0.25f) == want);
    Firefly inv;
    inv.summer_start = 0.3f;
    inv.summer_full = 0.2f;
    inv.summer_end_full = 0.55f;
    inv.summer_end = 0.45f;
    CHECK(night(inv, &kSeason, 0.25f) == want);
    CHECK(night(inv, &kSeason, 0.5f) == night(def, &kSeason, 0.5f));
    Firefly big;
    big.manual = 9.0f;
    big.strength = 9.0f;
    CHECK(night(big, nullptr, 0.1f) == 1.0f);
    // NaN gate inputs count as 0: no night, no rain
    CHECK(firefly_amount(def, &kSeason, kSeasonSummerCentre, kNaN, kNaN, kNaN) == 0.0f);
    CHECK(firefly_amount(def, &kSeason, kSeasonSummerCentre, 1.0f, kNaN, kNaN) == 1.0f);
    // out-of-range look inputs are clamped
    Firefly wild;
    wild.rate = 9999.0f;
    wild.size_m = 100.0f;
    wild.lifetime_s = 1000.0f;
    wild.box_xz_m = 1000.0f;
    wild.height_m = 0.0f;
    wild.drift_speed = kNaN;
    wild.color = math::vec3{kNaN, 5.0f, -1.0f};
    const FireflyLook w = firefly_look(wild, 2.0f);
    CHECK(w.rate == 200.0f);
    CHECK(w.size == 0.5f);
    CHECK(w.lifetime_max == 30.0f);
    CHECK(w.box == 100.0f);
    CHECK(w.height == 1.0f);
    CHECK(w.speed_max == 0.25f);
    CHECK(w.color_start.x == 0.8f);
    CHECK(w.color_start.y == 1.0f);
    CHECK(w.color_start.z == 0.0f);
}

TEST_CASE("firefly: the helpers are deterministic") {
    const Firefly f;
    for (int i = 0; i < 200; ++i) {
        const f32 p = static_cast<f32>(i) * 0.005f;
        const f32 nf = static_cast<f32>(i % 11) * 0.1f;
        const f32 a = firefly_amount(f, &kSeason, p, nf, 0.3f, 0.1f);
        const f32 b = firefly_amount(f, &kSeason, p, nf, 0.3f, 0.1f);
        CHECK(same_bits(a, b));
        CHECK(a >= 0.0f);
        CHECK(a <= 1.0f);
        const FireflyLook l1 = firefly_look(f, a);
        const FireflyLook l2 = firefly_look(f, a);
        CHECK(same_bits(l1.rate, l2.rate));
        CHECK(same_bits(l1.color_start.w, l2.color_start.w));
    }
}
