// Season (M-SEASONS 19a): the phase wraps at 1, a day plus an hour advance it by 1/year_days per day, `start_phase`
// offsets it and the f64 sum does not drift over 100000 days; the foliage tint is white at the centre of summer, equals
// each authored tint at its centre and is continuous across every quarter boundary and the year wrap; the density is 1
// outside winter and `winter_density` at its centre; the snow bias is exactly 0 in summer and maximal at the centre of
// winter; `strength` 0, `enabled` 0 and a missing component give the identity exactly; NaN and zero ranges fall back;
// the helpers agree over a WorldClock run in one go and in two halves; the component round-trips save/load; the
// WorldClock, Weather and SnowCover columns are the same with and without a Season.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>
#include <vector>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/season.h"
#include "mce/kit/snow_cover.h"
#include "mce/kit/weather.h"
#include "mce/kit/world_clock.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
bool same_vec(math::vec3 a, math::vec3 b) { return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z); }
bool near_vec(math::vec3 a, math::vec3 b, f32 eps) {
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps && std::fabs(a.z - b.z) <= eps;
}

constexpr math::vec3 kWhite{1.0f, 1.0f, 1.0f};

}  // namespace

TEST_CASE("season: defaults") {
    const Season s;
    CHECK(s.enabled == 1);
    CHECK(s.year_days == 12.0f);
    CHECK(s.start_phase == 0.1f);
    CHECK(s.winter_density == 0.4f);
    CHECK(s.snow_cover_bias == 0.5f);
    CHECK(s.cold_bias == 0.25f);
    CHECK(s.strength == 1.0f);
    CHECK(same_vec(s.spring_tint, math::vec3{0.75f, 1.05f, 0.7f}));
    CHECK(same_vec(s.autumn_tint, math::vec3{1.35f, 0.7f, 0.35f}));
    CHECK(same_vec(s.winter_tint, math::vec3{0.7f, 0.72f, 0.75f}));
    CHECK(s.pad_[0] == 0);
    CHECK(s.pad_[1] == 0);
    CHECK(s.pad_[2] == 0);
}

TEST_CASE("season: the phase wraps at 1 and advances by 1/year_days per day") {
    Season s;
    s.year_days = 10.0f;
    s.start_phase = 0.0f;
    CHECK(season_phase(s, 0, 0.0f) == 0.0f);
    CHECK(season_phase(s, 5, 0.0f) == doctest::Approx(0.5f));
    CHECK(season_phase(s, 10, 0.0f) == 0.0f);
    CHECK(season_phase(s, 13, 0.0f) == doctest::Approx(0.3f).epsilon(1e-5));
    CHECK(season_phase(s, 1, 12.0f) - season_phase(s, 1, 0.0f) == doctest::Approx(0.05f).epsilon(1e-5));
    CHECK(season_phase(s, 0, 24.0f) == doctest::Approx(0.1f).epsilon(1e-5));
    for (u32 d = 0; d < 100; ++d) {
        const f32 p = season_phase(s, d, 7.5f);
        CHECK(p >= 0.0f);
        CHECK(p < 1.0f);
    }
    // one day later is 1/year_days later (mod 1)
    for (u32 d = 0; d < 30; ++d) {
        f32 diff = season_phase(s, d + 1, 3.0f) - season_phase(s, d, 3.0f);
        if (diff < 0.0f) diff += 1.0f;
        CHECK(diff == doctest::Approx(0.1f).epsilon(1e-4));
    }
}

TEST_CASE("season: start_phase offsets the phase") {
    Season s;
    s.year_days = 8.0f;
    s.start_phase = 0.75f;
    CHECK(season_phase(s, 0, 0.0f) == 0.75f);
    CHECK(season_phase(s, 2, 0.0f) == 0.0f);
    CHECK(season_phase(s, 3, 0.0f) == doctest::Approx(0.125f));
    s.start_phase = 1.0f;
    CHECK(season_phase(s, 0, 0.0f) == 0.0f);
}

TEST_CASE("season: the f64 sum does not drift over 100000 days") {
    Season s;
    s.year_days = 7.0f;
    s.start_phase = 0.0f;
    // 100000 mod 7 = 5
    CHECK(season_phase(s, 100000, 0.0f) == doctest::Approx(5.0f / 7.0f).epsilon(1e-6));
    // 14 years later is the same phase as 0 years later
    s.year_days = 12.0f;
    s.start_phase = 0.1f;
    for (u32 d = 0; d < 12; ++d) {
        const f32 a = season_phase(s, d, 6.0f);
        const f32 b = season_phase(s, d + 12u * 8000u, 6.0f);
        CHECK(std::fabs(a - b) < 1e-6f);
    }
    // long runs stay smooth: consecutive hours differ by 1/(24 * year_days)
    const f32 p0 = season_phase(s, 99999, 23.0f);
    const f32 p1 = season_phase(s, 100000, 0.0f);
    f32 diff = p1 - p0;
    if (diff < 0.0f) diff += 1.0f;
    CHECK(diff == doctest::Approx(1.0f / (24.0f * 12.0f)).epsilon(1e-3));
}

TEST_CASE("season: hour, year_days and start_phase out of range fall back or clamp") {
    Season s;
    s.start_phase = 0.0f;
    const f32 base = season_phase(s, 3, 0.0f);
    CHECK(same_bits(season_phase(s, 3, kNaN), base));
    CHECK(same_bits(season_phase(s, 3, -2.0f), base));
    CHECK(same_bits(season_phase(s, 3, 30.0f), base));
    Season zero = s;
    zero.year_days = 0.0f;
    CHECK(same_bits(season_phase(zero, 3, 5.0f), season_phase(s, 3, 5.0f)));
    zero.year_days = kNaN;
    CHECK(same_bits(season_phase(zero, 3, 5.0f), season_phase(s, 3, 5.0f)));
    zero.year_days = -4.0f;
    CHECK(same_bits(season_phase(zero, 3, 5.0f), season_phase(s, 3, 5.0f)));
    Season big = s;
    big.year_days = 1.0e9f;
    Season cap = s;
    cap.year_days = 3650.0f;
    CHECK(same_bits(season_phase(big, 100, 0.0f), season_phase(cap, 100, 0.0f)));
    Season nan_start = s;
    nan_start.start_phase = kNaN;
    Season def_start = s;
    def_start.start_phase = 0.1f;
    CHECK(same_bits(season_phase(nan_start, 3, 0.0f), season_phase(def_start, 3, 0.0f)));
}

TEST_CASE("season: the tint is white at the centre of summer and each authored tint at its centre") {
    const Season s;
    CHECK(same_vec(season_foliage_tint(s, kSeasonSummerCentre), kWhite));
    CHECK(same_vec(season_foliage_tint(s, kSeasonSpringCentre), s.spring_tint));
    CHECK(same_vec(season_foliage_tint(s, kSeasonAutumnCentre), s.autumn_tint));
    CHECK(same_vec(season_foliage_tint(s, kSeasonWinterCentre), s.winter_tint));
    Season custom;
    custom.spring_tint = math::vec3{0.5f, 0.9f, 0.4f};
    custom.autumn_tint = math::vec3{2.0f, 0.5f, 0.25f};
    custom.winter_tint = math::vec3{0.6f, 0.6f, 0.9f};
    CHECK(same_vec(season_foliage_tint(custom, kSeasonSpringCentre), custom.spring_tint));
    CHECK(same_vec(season_foliage_tint(custom, kSeasonAutumnCentre), custom.autumn_tint));
    CHECK(same_vec(season_foliage_tint(custom, kSeasonWinterCentre), custom.winter_tint));
    // halfway between summer and autumn is the average of the two
    const math::vec3 mid = season_foliage_tint(s, 0.5f);
    CHECK(near_vec(mid, math::vec3{(1.0f + s.autumn_tint.x) * 0.5f, (1.0f + s.autumn_tint.y) * 0.5f,
                                   (1.0f + s.autumn_tint.z) * 0.5f}, 1e-5f));
    // strength scales the distance from white
    Season half = s;
    half.strength = 0.5f;
    const math::vec3 h = season_foliage_tint(half, kSeasonAutumnCentre);
    CHECK(near_vec(h, math::vec3{(1.0f + s.autumn_tint.x) * 0.5f, (1.0f + s.autumn_tint.y) * 0.5f,
                                 (1.0f + s.autumn_tint.z) * 0.5f}, 1e-5f));
}

TEST_CASE("season: the tint is continuous across every quarter boundary and the year wrap") {
    const Season s;
    constexpr f32 eps = 1.0e-4f;
    const f32 boundaries[] = {0.25f, 0.5f, 0.75f, 0.0f};
    for (f32 b : boundaries) {
        const f32 below = b == 0.0f ? 1.0f - eps : b - eps;
        const f32 above = b + eps;
        const math::vec3 lo = season_foliage_tint(s, below);
        const math::vec3 hi = season_foliage_tint(s, above);
        CHECK(near_vec(lo, hi, 2.0e-3f));
        const math::vec3 at = season_foliage_tint(s, b);
        CHECK(near_vec(at, lo, 2.0e-3f));
        CHECK(near_vec(at, hi, 2.0e-3f));
    }
    // a fine sweep over the whole year (and the wrap) never jumps
    math::vec3 prev = season_foliage_tint(s, 0.0f);
    for (u32 i = 1; i <= 4000; ++i) {
        const math::vec3 cur = season_foliage_tint(s, static_cast<f32>(i) / 4000.0f);
        CHECK(near_vec(prev, cur, 0.01f));
        prev = cur;
    }
    CHECK(near_vec(prev, season_foliage_tint(s, 0.0f), 1e-6f));
    // phase 1 and phase 0 are the same point of the year; negative phases wrap
    CHECK(same_vec(season_foliage_tint(s, 1.0f), season_foliage_tint(s, 0.0f)));
    CHECK(near_vec(season_foliage_tint(s, -0.125f), season_foliage_tint(s, 0.875f), 1e-6f));
}

TEST_CASE("season: the density is 1 outside winter and winter_density at its centre") {
    const Season s;
    CHECK(season_foliage_density(s, kSeasonWinterCentre) == s.winter_density);
    for (f32 p : {0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.6875f}) CHECK(season_foliage_density(s, p) == 1.0f);
    CHECK(season_foliage_density(s, 0.8f) < 1.0f);
    CHECK(season_foliage_density(s, 0.8f) > s.winter_density);
    CHECK(season_foliage_density(s, 0.95f) < 1.0f);
    CHECK(season_foliage_density(s, 0.0f) < 1.0f);
    // monotone down into the centre of winter and up out of it
    f32 prev = 1.0f;
    for (u32 i = 0; i <= 100; ++i) {
        const f32 d = season_foliage_density(s, 0.6875f + (0.875f - 0.6875f) * static_cast<f32>(i) / 100.0f);
        CHECK(d <= prev + 1e-6f);
        prev = d;
    }
    for (u32 i = 0; i <= 100; ++i) {
        f32 p = 0.875f + 0.25f * static_cast<f32>(i) / 100.0f;
        if (p >= 1.0f) p -= 1.0f;
        const f32 d = season_foliage_density(s, p);
        CHECK(d >= prev - 1e-6f);
        prev = d;
    }
    CHECK(prev == doctest::Approx(1.0f));
    Season half = s;
    half.strength = 0.5f;
    CHECK(season_foliage_density(half, kSeasonWinterCentre) == doctest::Approx(0.7f));
    Season bare = s;
    bare.winter_density = 0.0f;
    CHECK(season_foliage_density(bare, kSeasonWinterCentre) == 0.0f);
}

TEST_CASE("season: the snow bias is 0 in summer and maximal at the centre of winter") {
    const Season s;
    for (f32 p : {0.0f, 0.1f, 0.125f, 0.25f, 0.375f, 0.5f}) {
        const SeasonSnow b = season_snow_bias(s, p);
        CHECK(b.cover_floor == 0.0f);
        CHECK(b.lee_shift == 0.0f);
    }
    const SeasonSnow w = season_snow_bias(s, kSeasonWinterCentre);
    CHECK(w.cover_floor == s.snow_cover_bias);
    CHECK(w.lee_shift == s.cold_bias);
    f32 prev = 0.0f;
    for (u32 i = 0; i <= 100; ++i) {
        const SeasonSnow b = season_snow_bias(s, 0.5f + 0.375f * static_cast<f32>(i) / 100.0f);
        CHECK(b.cover_floor >= prev - 1e-6f);
        CHECK(b.cover_floor <= w.cover_floor);
        prev = b.cover_floor;
    }
    CHECK(season_snow_bias(s, 0.625f).cover_floor > 0.0f);
    CHECK(season_snow_bias(s, 0.95f).cover_floor > 0.0f);
    CHECK(season_snow_bias(s, 0.95f).cover_floor < w.cover_floor);
    Season half = s;
    half.strength = 0.5f;
    CHECK(season_snow_bias(half, kSeasonWinterCentre).cover_floor == doctest::Approx(0.25f));
    CHECK(season_snow_bias(half, kSeasonWinterCentre).lee_shift == doctest::Approx(0.125f));
}

TEST_CASE("season: strength 0, enabled 0 and a missing component give the identity exactly") {
    Season zero;
    zero.strength = 0.0f;
    Season off;
    off.enabled = 0;
    const Season* none = nullptr;
    for (u32 i = 0; i < 64; ++i) {
        const f32 p = static_cast<f32>(i) / 64.0f;
        for (const Season* s : {static_cast<const Season*>(&zero), static_cast<const Season*>(&off), none}) {
            CHECK(same_vec(season_foliage_tint(s, p), kWhite));
            CHECK(same_bits(season_foliage_density(s, p), 1.0f));
            const SeasonSnow b = season_snow_bias(s, p);
            CHECK(same_bits(b.cover_floor, 0.0f));
            CHECK(same_bits(b.lee_shift, 0.0f));
        }
    }
    // summer is the identity too, with the default strength
    const Season s;
    CHECK(same_vec(season_foliage_tint(s, kSeasonSummerCentre), kWhite));
    CHECK(same_bits(season_foliage_density(s, kSeasonSummerCentre), 1.0f));
    CHECK(same_bits(season_snow_bias(s, kSeasonSummerCentre).cover_floor, 0.0f));
}

TEST_CASE("season: NaN and out-of-range values fall back to the defaults or clamp") {
    const Season ref;
    Season n;
    n.spring_tint = math::vec3{kNaN, kNaN, kNaN};
    n.autumn_tint = math::vec3{kNaN, kNaN, kNaN};
    n.winter_tint = math::vec3{kNaN, kNaN, kNaN};
    n.winter_density = kNaN;
    n.snow_cover_bias = kNaN;
    n.cold_bias = kNaN;
    n.strength = kNaN;
    for (u32 i = 0; i < 32; ++i) {
        const f32 p = static_cast<f32>(i) / 32.0f;
        CHECK(same_vec(season_foliage_tint(n, p), season_foliage_tint(ref, p)));
        CHECK(same_bits(season_foliage_density(n, p), season_foliage_density(ref, p)));
        CHECK(same_bits(season_snow_bias(n, p).cover_floor, season_snow_bias(ref, p).cover_floor));
        CHECK(same_bits(season_snow_bias(n, p).lee_shift, season_snow_bias(ref, p).lee_shift));
    }
    // a NaN phase is the identity tint
    CHECK(same_vec(season_foliage_tint(ref, kNaN), kWhite));
    CHECK(same_bits(season_foliage_density(ref, kNaN), 1.0f));
    CHECK(season_snow_bias(ref, kNaN).cover_floor == 0.0f);
    // out of range values clamp
    Season big = ref;
    big.strength = 7.0f;
    big.winter_density = -3.0f;
    big.snow_cover_bias = 9.0f;
    big.cold_bias = 9.0f;
    big.autumn_tint = math::vec3{50.0f, -1.0f, 1.0f};
    CHECK(season_foliage_density(big, kSeasonWinterCentre) == 0.0f);
    CHECK(season_snow_bias(big, kSeasonWinterCentre).cover_floor == 1.0f);
    CHECK(season_snow_bias(big, kSeasonWinterCentre).lee_shift == 1.0f);
    CHECK(same_vec(season_foliage_tint(big, kSeasonAutumnCentre), math::vec3{4.0f, 0.0f, 1.0f}));
}

namespace {

struct Rig {
    World w;
    u64 t = 0;
    Entity wxe, sne, ce, se;
    explicit Rig(bool with_season, f32 day_length = 4.0f) {
        w.register_components<SaveKey>();
        install_weather(w, nullptr);
        if (Physics3D* p = w.try_resource<Physics3D>()) p->auto_step = 0;
        Weather c;
        c.target = WeatherKind::Snow;
        c.transition_seconds = 0.5f;
        wxe = w.spawn_with(SaveKey{1}, c);
        sne = w.spawn_with(SaveKey{2}, SnowCover{});
        WorldClock k;
        k.hour = 6.0f;
        k.day_length_seconds = day_length;
        ce = w.spawn_with(SaveKey{3}, k);
        if (with_season) {
            Season s;
            s.enabled = 0;
            se = w.spawn_with(SaveKey{4}, s);
        }
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    // The helpers fed from the live clock, packed into bits for an exact comparison.
    std::vector<u32> sample(const Season& s) const {
        const WorldClock* k = w.try_get<WorldClock>(ce);
        const f32 p = season_phase(s, k->day, k->hour);
        const math::vec3 tint = season_foliage_tint(s, p);
        const SeasonSnow b = season_snow_bias(s, p);
        return {k->day,
                std::bit_cast<u32>(p),
                std::bit_cast<u32>(tint.x),
                std::bit_cast<u32>(tint.y),
                std::bit_cast<u32>(tint.z),
                std::bit_cast<u32>(season_foliage_density(s, p)),
                std::bit_cast<u32>(b.cover_floor),
                std::bit_cast<u32>(b.lee_shift)};
    }
};

}  // namespace

TEST_CASE("season: the component round-trips save and load") {
    Array<byte> bytes;
    u64 hash_a = 0;
    {
        Rig a(true);
        Season& s = *a.w.try_get_mut<Season>(a.se);
        s.enabled = 1;
        s.year_days = 30.0f;
        s.start_phase = 0.6f;
        s.spring_tint = math::vec3{0.5f, 0.9f, 0.4f};
        s.autumn_tint = math::vec3{2.0f, 0.5f, 0.25f};
        s.winter_tint = math::vec3{0.6f, 0.6f, 0.9f};
        s.winter_density = 0.2f;
        s.snow_cover_bias = 0.8f;
        s.cold_bias = 0.35f;
        s.strength = 0.9f;
        a.run(30);
        save::write_save(a.w, {}, bytes);
        hash_a = a.w.state_hash();
    }
    Rig b(true);
    b.run(5);
    REQUIRE(save::read_save(b.w, bytes.span()));
    const Season& s = *b.w.try_get<Season>(b.se);
    CHECK(s.enabled == 1);
    CHECK(s.year_days == 30.0f);
    CHECK(s.start_phase == 0.6f);
    CHECK(same_vec(s.spring_tint, math::vec3{0.5f, 0.9f, 0.4f}));
    CHECK(same_vec(s.autumn_tint, math::vec3{2.0f, 0.5f, 0.25f}));
    CHECK(same_vec(s.winter_tint, math::vec3{0.6f, 0.6f, 0.9f}));
    CHECK(s.winter_density == 0.2f);
    CHECK(s.snow_cover_bias == 0.8f);
    CHECK(s.cold_bias == 0.35f);
    CHECK(s.strength == 0.9f);
    CHECK(s.pad_[0] == 0);
    CHECK(s.pad_[1] == 0);
    CHECK(s.pad_[2] == 0);
    CHECK(b.w.state_hash() == hash_a);
}

TEST_CASE("season: the WorldClock, Weather and SnowCover data are the same with and without a Season") {
    Rig a(false), b(true);
    for (u32 i = 0; i < 300; ++i) {
        a.run(1);
        b.run(1);
        const Weather& wa = *a.w.try_get<Weather>(a.wxe);
        const Weather& wb = *b.w.try_get<Weather>(b.wxe);
        CHECK(wa.current == wb.current);
        CHECK(same_bits(wa.intensity, wb.intensity));
        CHECK(same_bits(wa.wind, wb.wind));
        CHECK(same_bits(wa.wetness, wb.wetness));
        CHECK(wa.counter == wb.counter);
        CHECK(wa.timer == wb.timer);
        CHECK(wa.fade == wb.fade);
        CHECK(wa.wet_q == wb.wet_q);
        const WorldClock& ka = *a.w.try_get<WorldClock>(a.ce);
        const WorldClock& kb = *b.w.try_get<WorldClock>(b.ce);
        CHECK(same_bits(ka.hour, kb.hour));
        CHECK(ka.day == kb.day);
        CHECK(ka.phase == kb.phase);
        CHECK(ka.phase_den == kb.phase_den);
        const SnowCover& na = *a.w.try_get<SnowCover>(a.sne);
        const SnowCover& nb = *b.w.try_get<SnowCover>(b.sne);
        CHECK(na.snow_q == nb.snow_q);
        CHECK(same_bits(na.cover, nb.cover));
    }
    // the season code never writes the component: it is still as authored
    const Season& s = *b.w.try_get<Season>(b.se);
    CHECK(s.year_days == 12.0f);
    CHECK(s.enabled == 0);
    Rig c(false), d(false);
    c.run(300);
    d.run(300);
    CHECK(c.w.state_hash() == d.w.state_hash());
}

TEST_CASE("season: the helpers over a WorldClock run are the same in one go and in two halves") {
    constexpr u32 kN = 1200;
    constexpr u32 kEvery = 20;
    Season authored;
    authored.year_days = 3.0f;
    authored.start_phase = 0.7f;
    std::vector<std::vector<u32>> whole;
    {
        Rig a(true);
        for (u32 i = 0; i < kN; ++i) {
            a.run(1);
            if ((i + 1) % kEvery == 0) whole.push_back(a.sample(authored));
        }
    }
    CHECK(whole.size() == kN / kEvery);
    bool moves = false;
    for (usize i = 1; i < whole.size(); ++i) moves = moves || whole[i][1] != whole[i - 1][1];
    CHECK(moves);
    // the clock crosses at least one day in the run
    CHECK(whole.back()[0] > whole.front()[0]);
    std::vector<std::vector<u32>> split;
    Array<byte> bytes;
    {
        Rig a(true);
        for (u32 i = 0; i < kN / 2; ++i) {
            a.run(1);
            if ((i + 1) % kEvery == 0) split.push_back(a.sample(authored));
        }
        save::write_save(a.w, {}, bytes);
    }
    {
        Rig b(true);
        b.run(3);
        REQUIRE(save::read_save(b.w, bytes.span()));
        b.t = kN / 2;
        for (u32 i = kN / 2; i < kN; ++i) {
            b.run(1);
            if ((i + 1) % kEvery == 0) split.push_back(b.sample(authored));
        }
    }
    REQUIRE(split.size() == whole.size());
    for (usize i = 0; i < whole.size(); ++i) CHECK(whole[i] == split[i]);
}
