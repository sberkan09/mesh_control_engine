// WindGust (M-WINDGUST 12a): the level is a pure function of authoring, seed and tick; same seed same curve, other seed
// another; gap, rise, fall and peak stay in the authored ranges; level 0 in the gap and at the start; inverted, NaN and
// zero ranges; a dt change mid-gust does not jump; effective_wind returns Weather.wind bit-identically without a gust,
// with the bit clear or disabled; save/load mid-gust and N-ticks equality; a long run has no drift; the Weather bytes
// are the same with and without a WindGust; no Weather or no WindGust leaves the world untouched.

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/weather.h"
#include "mce/kit/wind_gust.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr u32 kUnit = 1u << 24;

struct Rig {
    World w;
    u64 t = 0;
    f32 dt = kDt;
    Entity wx, gust;
    explicit Rig(u32 seed = 7, bool with_gust = true, bool with_weather = true) {
        w.register_components<SaveKey>();
        install_weather(w, nullptr);
        if (Physics3D* p = w.try_resource<Physics3D>()) p->auto_step = 0;  // the tests change dt
        if (with_weather) {
            Weather c;
            c.target = WeatherKind::Rain;
            c.transition_seconds = 0.05f;
            wx = w.spawn_with(SaveKey{1}, c);
        }
        if (with_gust) {
            WindGust g;
            g.seed = seed;
            gust = w.spawn_with(SaveKey{2}, g);
        }
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(dt, t++);
    }
    WindGust& g() { return *w.try_get_mut<WindGust>(gust); }
    const Weather& weather() { return *w.try_get<Weather>(wx); }
};

std::vector<u32> curve(u32 seed, u32 n) {
    Rig r(seed);
    std::vector<u32> out;
    for (u32 i = 0; i < n; ++i) {
        r.run(1);
        out.push_back(r.g().gust_q);
    }
    return out;
}

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }

}  // namespace

TEST_CASE("wind_gust: defaults") {
    const WindGust g;
    CHECK(g.strength == 0.6f);
    CHECK(g.min_gap_s == 2.0f);
    CHECK(g.max_gap_s == 8.0f);
    CHECK(g.min_rise_s == 0.4f);
    CHECK(g.max_rise_s == 1.5f);
    CHECK(g.min_fall_s == 1.0f);
    CHECK(g.max_fall_s == 3.0f);
    CHECK(g.min_peak == 0.4f);
    CHECK(g.max_peak == 1.0f);
    CHECK(g.seed == 0);
    CHECK(g.affects == 31u);
    CHECK(g.enabled == 1);
    CHECK(g.seeded == 0);
    CHECK(g.gust_q == 0);
    CHECK(g.segment == 0);
    CHECK(g.tick == 0);
    for (u8 b : g.pad_) CHECK(b == 0);
}

TEST_CASE("wind_gust: the level starts at 0, rises, peaks and falls back to 0 within the segment") {
    Rig r;
    r.run(1);
    CHECK(r.g().seeded == 1);
    CHECK(r.g().gust_q == 0);  // the gap
    const u32 gap = r.g().gap_ticks;
    const u32 rise = r.g().rise_ticks;
    const u32 fall = r.g().fall_ticks;
    const u32 peak = r.g().peak_q;
    REQUIRE(rise >= 1);
    REQUIRE(fall >= 1);
    u32 top = 0;
    for (u32 k = 2; k <= gap + rise + fall; ++k) {
        r.run(1);
        const u32 seg_tick = r.g().tick;
        if (k < gap + rise + fall) {
            CHECK(r.g().segment == 0);
            CHECK(seg_tick == k);
        }
        if (k <= gap) CHECK(r.g().gust_q == 0);
        CHECK(r.g().gust_q <= peak);
        top = std::max(top, r.g().gust_q);
    }
    CHECK(top == peak);  // the peak is reached exactly at the end of the rise
    CHECK(r.g().segment == 1);  // the fall ended: next segment
    CHECK(r.g().tick == 0);
    CHECK(r.g().gust_q == 0);
}

TEST_CASE("wind_gust: same seed same curve, another seed another") {
    const std::vector<u32> a = curve(7, 3000);
    const std::vector<u32> b = curve(7, 3000);
    const std::vector<u32> c = curve(8, 3000);
    CHECK(a == b);
    CHECK(a != c);
    CHECK(*std::max_element(a.begin(), a.end()) > 0);
    CHECK(*std::max_element(a.begin(), a.end()) <= kUnit);
    // seed 0 is a valid seed and gusts too
    const std::vector<u32> z = curve(0, 3000);
    CHECK(*std::max_element(z.begin(), z.end()) > 0);
    CHECK(z != a);
}

TEST_CASE("wind_gust: the level at tick T is the same in one run and in two halves") {
    Rig a(11);
    a.run(1234);
    Rig b(11);
    b.run(500);
    b.run(734);
    CHECK(a.g().gust_q == b.g().gust_q);
    CHECK(a.g().segment == b.g().segment);
    CHECK(a.g().tick == b.g().tick);
    CHECK(a.w.state_hash() == b.w.state_hash());
}

TEST_CASE("wind_gust: gap, rise, fall and peak stay in the authored ranges") {
    Rig r(3);
    r.g().min_gap_s = 1.0f;
    r.g().max_gap_s = 2.0f;
    r.g().min_rise_s = 0.5f;
    r.g().max_rise_s = 1.0f;
    r.g().min_fall_s = 0.25f;
    r.g().max_fall_s = 0.75f;
    r.g().min_peak = 0.3f;
    r.g().max_peak = 0.8f;
    u32 seen = 0;
    u32 last_segment = 0xFFFFFFFFu;
    bool varied = false;
    u32 first_gap = 0;
    for (u32 i = 0; i < 60 * 120; ++i) {
        r.run(1);
        if (r.g().segment == last_segment) continue;
        last_segment = r.g().segment;
        ++seen;
        const WindGust& g = r.g();
        CHECK(g.gap_ticks >= 60);
        CHECK(g.gap_ticks <= 120);
        CHECK(g.rise_ticks >= 30);
        CHECK(g.rise_ticks <= 60);
        CHECK(g.fall_ticks >= 15);
        CHECK(g.fall_ticks <= 45);
        CHECK(g.peak_q >= static_cast<u32>(std::floor(0.3 * kUnit)));
        CHECK(g.peak_q <= static_cast<u32>(std::ceil(0.8 * static_cast<f64>(kUnit))) + 1);
        if (seen == 1) first_gap = g.gap_ticks;
        else if (g.gap_ticks != first_gap) varied = true;
    }
    CHECK(seen > 20);
    CHECK(varied);
}

TEST_CASE("wind_gust: inverted, NaN, out-of-range and zero authoring") {
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    {  // inverted pairs behave as swapped
        Rig a(5), b(5);
        a.g().min_gap_s = 6.0f;
        a.g().max_gap_s = 1.0f;
        b.g().min_gap_s = 1.0f;
        b.g().max_gap_s = 6.0f;
        a.g().min_peak = 0.9f;
        a.g().max_peak = 0.2f;
        b.g().min_peak = 0.2f;
        b.g().max_peak = 0.9f;
        a.run(3000);
        b.run(3000);
        CHECK(a.g().gust_q == b.g().gust_q);
        CHECK(a.g().segment == b.g().segment);
    }
    {  // NaN is the default
        Rig a(5), b(5);
        a.g().min_gap_s = nan;
        a.g().max_rise_s = nan;
        a.g().min_peak = nan;
        a.g().strength = nan;
        a.run(3000);
        b.run(3000);
        CHECK(a.g().gust_q == b.g().gust_q);
        CHECK(a.g().segment == b.g().segment);
        CHECK(std::isnan(a.g().min_gap_s));  // authoring is not rewritten
    }
    {  // out of range clamps to the Range
        Rig a(5), b(5);
        a.g().min_gap_s = -5.0f;
        a.g().max_gap_s = 1e9f;
        b.g().min_gap_s = 0.0f;
        b.g().max_gap_s = 120.0f;
        a.g().min_rise_s = 0.0f;
        b.g().min_rise_s = 0.05f;
        a.run(3000);
        b.run(3000);
        CHECK(a.g().gust_q == b.g().gust_q);
        CHECK(a.g().segment == b.g().segment);
    }
    {  // zero-width ranges are exact; a zero gap gusts back to back
        Rig r(5);
        r.g().min_gap_s = 0.0f;
        r.g().max_gap_s = 0.0f;
        r.g().min_rise_s = 0.5f;
        r.g().max_rise_s = 0.5f;
        r.g().min_fall_s = 0.5f;
        r.g().max_fall_s = 0.5f;
        r.g().min_peak = 0.5f;
        r.g().max_peak = 0.5f;
        r.run(1);
        CHECK(r.g().gap_ticks == 0);
        CHECK(r.g().rise_ticks == 30);
        CHECK(r.g().fall_ticks == 30);
        CHECK(r.g().peak_q == kUnit / 2);
        r.run(59);
        CHECK(r.g().segment == 1);  // 60 ticks per gust
        CHECK(r.g().gust_q == 0);
        r.run(30);
        CHECK(r.g().gust_q == kUnit / 2);  // the peak after 30 ticks of the next rise
    }
    {  // zero peak: a gust is flat
        Rig r(5);
        r.g().min_peak = 0.0f;
        r.g().max_peak = 0.0f;
        r.run(3000);
        CHECK(r.g().gust_q == 0);
        CHECK(r.g().segment > 0);
    }
    {  // dt <= 0 and NaN do not advance
        Rig r(5);
        r.run(10);
        const WindGust before = r.g();
        for (f32 d : {0.0f, -1.0f, nan}) {
            r.dt = d;
            r.run(5);
        }
        r.dt = kDt;
        CHECK(std::memcmp(&r.g(), &before, sizeof(WindGust)) == 0);  // NOLINT(bugprone-suspicious-memory-comparison): saved state with explicit pads
        r.run(1);
        CHECK(r.g().tick == before.tick + 1);
    }
}

TEST_CASE("wind_gust: a dt change or an authoring edit mid-gust does not jump the curve") {
    Rig r(5);
    r.g().min_gap_s = 0.0f;
    r.g().max_gap_s = 0.0f;
    r.g().min_peak = 1.0f;
    r.run(20);
    const WindGust before = r.g();
    REQUIRE(before.rise_ticks > 25);
    REQUIRE(before.gust_q > 0);
    r.dt = 1.0f / 30.0f;
    r.g().min_rise_s = 0.05f;  // an edit
    r.g().max_rise_s = 0.05f;
    r.g().min_peak = 0.0f;
    r.g().max_peak = 0.0f;
    u32 prev = before.gust_q;
    const u32 slack = static_cast<u32>(1.6 * before.peak_q / before.rise_ticks) + 2;
    for (u32 i = 0; i < 5; ++i) {
        r.run(1);
        CHECK(r.g().rise_ticks == before.rise_ticks);
        CHECK(r.g().fall_ticks == before.fall_ticks);
        CHECK(r.g().peak_q == before.peak_q);
        CHECK(r.g().gust_q >= prev);
        CHECK(r.g().gust_q - prev <= slack);
        prev = r.g().gust_q;
    }
}

TEST_CASE("wind_gust: effective_wind returns Weather.wind bit-identically unless a gust applies") {
    Weather w;
    w.wind = 0.3f;
    WindGust g;
    g.gust_q = kUnit / 2;
    g.strength = 1.0f;
    // no gust
    CHECK(same_bits(effective_wind(w, nullptr, kGustSway), w.wind));
    // disabled
    g.enabled = 0;
    CHECK(same_bits(effective_wind(w, &g, kGustSway), w.wind));
    g.enabled = 1;
    // bit clear
    g.affects = kGustRain;
    CHECK(same_bits(effective_wind(w, &g, kGustSway), w.wind));
    CHECK(same_bits(effective_wind(w, &g, 0), w.wind));
    // zero level
    g.affects = kGustAll;
    g.gust_q = 0;
    CHECK(same_bits(effective_wind(w, &g, kGustSway), w.wind));
    // an odd wind bit pattern stays untouched too (NaN, out of range)
    for (f32 v : {std::numeric_limits<f32>::quiet_NaN(), -0.0f, 2.5f, 0.1f}) {
        w.wind = v;
        CHECK(same_bits(effective_wind(w, nullptr, kGustAudio), v));
        CHECK(same_bits(effective_wind(w, &g, kGustAudio), v));
    }
    // a gust: wind * (1 + strength * level), rounded once, clamped to 0..1
    w.wind = 0.3f;
    g.gust_q = kUnit / 2;
    CHECK(effective_wind(w, &g, kGustSway) == static_cast<f32>(static_cast<f64>(0.3f) * 1.5));
    g.gust_q = kUnit;
    g.strength = 3.0f;
    CHECK(effective_wind(w, &g, kGustAudio) == 1.0f);  // clamped
    g.strength = 100.0f;                               // clamps to 3
    CHECK(effective_wind(w, &g, kGustAudio) == 1.0f);
    g.strength = std::numeric_limits<f32>::quiet_NaN();  // the default 0.6
    CHECK(effective_wind(w, &g, kGustAudio) == static_cast<f32>(static_cast<f64>(0.3f) * 1.6));
    w.wind = 1.0f;  // a storm already saturates
    g.strength = 0.6f;
    CHECK(effective_wind(w, &g, kGustAudio) == 1.0f);
    for (u32 bit : {kGustSway, kGustRain, kGustClouds, kGustSnow, kGustAudio}) {
        g.affects = bit;
        w.wind = 0.1f;
        CHECK(effective_wind(w, &g, bit) > 0.1f);
        CHECK(same_bits(effective_wind(w, &g, kGustAll & ~bit), 0.1f));
    }
}

TEST_CASE("wind_gust: save and load in the middle of a gust resumes on the same state") {
    Array<byte> bytes;
    u64 hash_a = 0, hash_end_a = 0;
    {
        Rig a(9);
        for (u32 i = 0; i < 60 * 60 && !(a.g().gust_q > 0 && a.g().tick > a.g().gap_ticks + 3); ++i) a.run(1);
        REQUIRE(a.g().gust_q > 0);
        save::write_save(a.w, {}, bytes);
        hash_a = a.w.state_hash();
        a.run(60 * 30);
        hash_end_a = a.w.state_hash();
    }
    Rig b(9);
    b.run(77);  // another state, replaced by the save
    REQUIRE(save::read_save(b.w, bytes.span()));
    CHECK(b.w.state_hash() == hash_a);
    b.run(60 * 30);
    CHECK(b.w.state_hash() == hash_end_a);
}

TEST_CASE("wind_gust: a long run has no drift") {
    // Only the gust system (plus a Weather it requires): a million ticks stay cheap.
    World w;
    w.register_components<Weather>();
    w.register_components<WindGust>();
    w.add_system(Phase::FixedUpdate_Pre, "kit_wind_direction", +[](Frame&) {});
    install_wind_gust(w);
    w.spawn_with(Weather{});
    WindGust init;
    init.seed = 21;
    const Entity e = w.spawn_with(init);
    u64 completed = 0;  // ticks of the finished segments
    u32 last_segment = 0;
    u32 last_len = 0;
    u32 max_level = 0;
    bool in_bounds = true;
    constexpr u32 kTicks = 1000000;
    for (u32 i = 0; i < kTicks; ++i) {
        w.tick(kDt, i);
        const WindGust& g = *w.try_get<WindGust>(e);
        max_level = std::max(max_level, g.gust_q);
        const u64 total = static_cast<u64>(g.gap_ticks) + g.rise_ticks + g.fall_ticks;
        in_bounds = in_bounds && g.gust_q <= g.peak_q && g.peak_q <= kUnit && g.tick < total;
        if (g.segment != last_segment) {
            completed += last_len;
            last_segment = g.segment;
        }
        last_len = static_cast<u32>(total);
    }
    const WindGust& g = *w.try_get<WindGust>(e);
    CHECK(in_bounds);
    CHECK(max_level > 0);
    // Every tick is accounted for: the finished segments plus the ticks into the current one.
    CHECK(completed + g.tick == kTicks);
    CHECK(g.segment > 1000);
}

TEST_CASE("wind_gust: the Weather bytes are the same with and without a WindGust") {
    Rig a(7, true), b(7, false);
    for (u32 i = 0; i < 900; ++i) {
        a.run(1);
        b.run(1);
        Weather wa = a.weather();
        Weather wb = b.weather();
        // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison): both copies come from the same writer, padding included
        CHECK(std::memcmp(&wa, &wb, sizeof(Weather)) == 0);
    }
    CHECK(a.g().segment > 0);
}

TEST_CASE("wind_gust: no Weather, no WindGust or enabled 0 leaves the world untouched") {
    {  // no WindGust: no gust column, deterministic hash
        Rig a(7, false), b(7, false);
        a.run(300);
        b.run(300);
        CHECK(a.w.state_hash() == b.w.state_hash());
        u32 n = 0;
        a.w.query<const WindGust>().each([&](Entity, const WindGust&) { ++n; });
        CHECK(n == 0);
    }
    {  // no Weather
        Rig r(7, true, false);
        const WindGust before = r.g();
        r.run(300);
        CHECK(std::memcmp(&r.g(), &before, sizeof(WindGust)) == 0);  // NOLINT(bugprone-suspicious-memory-comparison): saved state with explicit pads
    }
    {  // enabled 0
        Rig r(7);
        r.g().enabled = 0;
        const WindGust before = r.g();
        r.run(300);
        CHECK(std::memcmp(&r.g(), &before, sizeof(WindGust)) == 0);  // NOLINT(bugprone-suspicious-memory-comparison): saved state with explicit pads
        r.g().enabled = 1;
        r.run(1);
        CHECK(r.g().seeded == 1);
    }
    {  // a second WindGust is ignored
        Rig r(7);
        WindGust second;
        second.seed = 99;
        const Entity e = r.w.spawn_with(SaveKey{3}, second);
        r.run(500);
        CHECK(r.w.try_get<WindGust>(e)->seeded == 0);
        CHECK(r.w.try_get<WindGust>(e)->tick == 0);
    }
}
