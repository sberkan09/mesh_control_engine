// WaterFlow (M-WATER2 49e): the current as data and the flowing wave set. No WaterFlow, `enabled` 0 and a speed of 0
// give a wave set bit-identical to water_wave_set; every flowing omega differs from the still one by a whole number of
// cycles per 64 s (kit::water_flow_cycles, bit-identical to render::water_flow_cycles); the height repeats after the
// period with a current; a short wave's crest is carried by U * 10 s within its rounding bound, a long one within its
// larger bound; a current across the wind carries the pattern across; NaN and out-of-range authoring fall back or
// clamp; the component round-trips save/load; the Water and Weather columns are the same with and without a WaterFlow,
// which is itself never written.

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/world.h"
#include "mce/kit/water.h"
#include "mce/kit/water_flow.h"
#include "mce/kit/weather.h"
#include "mce/render/water_optics.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
constexpr f64 kBase = 6.283185307179586476925286766559 / kWaterPeriodSeconds;  // rad/s per cycle per period

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }

bool same_set(const WaterWaveSet& a, const WaterWaveSet& b) {
    if (!same_bits(a.level_y, b.level_y) || a.count != b.count) return false;
    for (u32 i = 0; i < a.count; ++i) {
        const WaterWave &x = a.wave[i], &y = b.wave[i];
        if (!same_bits(x.dir.x, y.dir.x) || !same_bits(x.dir.y, y.dir.y) || !same_bits(x.k, y.k) ||
            !same_bits(x.amplitude, y.amplitude) || !same_bits(x.steepness, y.steepness) ||
            !same_bits(x.omega, y.omega) || !same_bits(x.phase_seed, y.phase_seed))
            return false;
    }
    return true;
}

WaterFlow flow_of(f32 dir_deg, f32 speed) {
    WaterFlow f;
    f.dir_deg = dir_deg;
    f.speed = speed;
    return f;
}

}  // namespace

TEST_CASE("water flow: defaults and the velocity") {
    const WaterFlow d;
    CHECK(d.enabled == 1);
    CHECK(d.dir_deg == 0.0f);
    CHECK(d.speed == 0.0f);
    CHECK(sizeof(WaterFlow) == 12);

    const auto zero = [](math::vec2 v) { return std::bit_cast<u32>(v.x) == 0u && std::bit_cast<u32>(v.y) == 0u; };
    CHECK(zero(water_flow_velocity(nullptr)));
    CHECK(zero(water_flow_velocity(&d)));  // speed 0
    WaterFlow off = flow_of(30.0f, 2.0f);
    off.enabled = 0;
    CHECK(zero(water_flow_velocity(&off)));
    const WaterFlow nan_speed = flow_of(30.0f, kNaN);
    CHECK(zero(water_flow_velocity(&nan_speed)));
    const WaterFlow neg_speed = flow_of(30.0f, -1.0f);
    CHECK(zero(water_flow_velocity(&neg_speed)));

    for (const f32 deg : {0.0f, 30.0f, 90.0f, 135.0f, 270.0f, 359.0f}) {
        const WaterFlow f = flow_of(deg, 1.2f);
        const math::vec2 v = water_flow_velocity(&f);
        const f64 rad = f64(deg) * (3.14159265358979323846 / 180.0);
        CHECK(same_bits(v.x, f32(f64(1.2f) * std::cos(rad))));
        CHECK(same_bits(v.y, f32(f64(1.2f) * std::sin(rad))));
        CHECK(std::hypot(v.x, v.y) == doctest::Approx(1.2f).epsilon(1e-6));
    }
    const WaterFlow plus_x = flow_of(0.0f, 2.0f);
    CHECK(water_flow_velocity(&plus_x).x == 2.0f);
    CHECK(water_flow_velocity(&plus_x).y == 0.0f);

    // out of range clamps, NaN direction counts as 0
    const WaterFlow fast = flow_of(0.0f, 9.0f);
    CHECK(water_flow_velocity(&fast).x == 5.0f);
    const WaterFlow nan_dir = flow_of(kNaN, 1.0f);
    CHECK(water_flow_velocity(&nan_dir).x == 1.0f);
    CHECK(water_flow_velocity(&nan_dir).y == 0.0f);
    const WaterFlow low_dir = flow_of(-40.0f, 1.0f), high_dir = flow_of(400.0f, 1.0f), at_360 = flow_of(360.0f, 1.0f);
    CHECK(water_flow_velocity(&low_dir).x == 1.0f);  // clamps to 0 degrees
    CHECK(water_flow_velocity(&low_dir).y == 0.0f);
    CHECK(same_bits(water_flow_velocity(&high_dir).x, water_flow_velocity(&at_360).x));
    CHECK(same_bits(water_flow_velocity(&high_dir).y, water_flow_velocity(&at_360).y));
}

TEST_CASE("water flow: no current, disabled or still gives the still wave set bit for bit") {
    Water w;
    w.wave_count = 8;
    w.amplitude = 0.7f;
    w.wavelength = 13.0f;
    for (const f32 wind : {0.0f, 37.0f, 211.0f}) {
        const WaterWaveSet still = water_wave_set(w, wind);
        CHECK(same_set(water_wave_set_flow(w, wind, math::vec2{0.0f, 0.0f}), still));
        CHECK(same_set(water_wave_set_flow(w, wind, math::vec2{-0.0f, 0.0f}), still));
        CHECK(same_set(water_wave_set_flow(w, wind, water_flow_velocity(nullptr)), still));
        WaterFlow off = flow_of(45.0f, 3.0f);
        off.enabled = 0;
        CHECK(same_set(water_wave_set_flow(w, wind, water_flow_velocity(&off)), still));
        const WaterFlow calm = flow_of(45.0f, 0.0f);
        CHECK(same_set(water_wave_set_flow(w, wind, water_flow_velocity(&calm)), still));
        // a current too slow for any wave to gain a whole cycle in 64 s keeps every omega
        CHECK(same_set(water_wave_set_flow(w, wind, math::vec2{1.0e-4f, -1.0e-4f}), still));
        // NaN velocity counts as zero
        CHECK(same_set(water_wave_set_flow(w, wind, math::vec2{kNaN, kNaN}), still));
    }
    // the flowing gameplay queries with a null flow are the water.h ones, bit for bit
    for (const f64 t : {0.0, 3.25, 63.9, 100.5, 1.0e5}) {
        for (const math::vec2 p : {math::vec2{0.0f, 0.0f}, math::vec2{12.5f, -7.25f}, math::vec2{-300.0f, 41.0f}}) {
            CHECK(same_bits(water_height_at(w, nullptr, p, t), water_height_at(w, p, t)));
            CHECK(same_bits(water_height_at(w, nullptr, p, t, 77.0f), water_height_at(w, p, t, 77.0f)));
            const math::vec3 a = water_normal_at(w, nullptr, p, t), b = water_normal_at(w, p, t);
            CHECK((same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z)));
        }
    }
}

TEST_CASE("water flow: every flowing omega is the still one plus whole cycles per period (render's rounding)") {
    Water w;
    w.wave_count = 8;
    w.amplitude = 0.4f;
    w.wavelength = 24.0f;
    u32 shifted = 0;
    for (const f32 wind : {0.0f, 25.0f, 160.0f, 300.0f}) {
        for (const f32 dir : {0.0f, 45.0f, 90.0f, 200.0f, 333.0f}) {
            for (const f32 speed : {0.1f, 0.5f, 1.2f, 3.0f, 5.0f}) {
                const WaterFlow f = flow_of(dir, speed);
                const math::vec2 u = water_flow_velocity(&f);
                const WaterWaveSet still = water_wave_set(w, wind), flow = water_wave_set_flow(w, wind, u);
                REQUIRE(flow.count == still.count);
                for (u32 i = 0; i < flow.count; ++i) {
                    const WaterWave &a = flow.wave[i], &b = still.wave[i];
                    // only omega changes
                    CHECK((same_bits(a.k, b.k) && same_bits(a.dir.x, b.dir.x) && same_bits(a.dir.y, b.dir.y) &&
                           same_bits(a.amplitude, b.amplitude) && same_bits(a.steepness, b.steepness)));
                    const math::vec2 kv{b.k * b.dir.x, b.k * b.dir.y};
                    const f64 c = water_flow_cycles(kv, u);
                    CHECK(c == std::round(c));
                    CHECK(std::bit_cast<u64>(c) == std::bit_cast<u64>(render::water_flow_cycles(kv, u)));
                    CHECK(c == std::round((f64(kv.x) * f64(u.x) + f64(kv.y) * f64(u.y)) * 64.0 /
                                          (2.0 * 3.14159265358979323846)));
                    const f64 da = f64(a.omega) / kBase, db = f64(b.omega) / kBase;
                    CHECK(std::fabs(da - std::round(da)) < 1e-4);
                    CHECK(std::round(da) - std::round(db) == c);
                    if (c == 0.0) CHECK(same_bits(a.omega, b.omega));
                    shifted += c != 0.0 ? 1u : 0u;
                }
            }
        }
    }
    CHECK(shifted > 200);  // not vacuous
    // the two roundings agree on awkward inputs too (halves, negatives, NaN, infinity)
    const f32 inf = std::numeric_limits<f32>::infinity();
    for (const math::vec2 k : {math::vec2{0.0f, 0.0f}, math::vec2{0.0981748f, 0.0f}, math::vec2{-4.5f, 2.25f},
                               math::vec2{kNaN, 1.0f}, math::vec2{inf, 0.0f}, math::vec2{1.0e30f, 1.0e30f}})
        for (const math::vec2 u : {math::vec2{1.0f, 0.0f}, math::vec2{-1.0f, 0.0f}, math::vec2{0.3f, -4.9f},
                                   math::vec2{0.0f, kNaN}, math::vec2{1.0e30f, 0.0f}}) {
            const f64 a = water_flow_cycles(k, u), b = render::water_flow_cycles(k, u);
            CHECK(std::bit_cast<u64>(a) == std::bit_cast<u64>(b));
            CHECK(std::bit_cast<u64>(water_flow_cycles(k, math::vec2{-u.x, -u.y})) ==
                  std::bit_cast<u64>(a == 0.0 ? 0.0 : -a));  // odd in the velocity
        }
}

TEST_CASE("water flow: the height at t equals the height at t plus the period with a current") {
    Water w;
    w.wave_count = 6;
    w.amplitude = 0.6f;
    w.wavelength = 9.0f;
    w.wind_dir_deg = 20.0f;
    const WaterFlow f = flow_of(110.0f, 1.7f);
    const WaterWaveSet set = water_wave_set_flow(w, 20.0f, water_flow_velocity(&f));
    for (const f64 t : {0.0, 0.5, 3.25, 17.0, 40.75, 63.5}) {
        for (const math::vec2 p : {math::vec2{0.0f, 0.0f}, math::vec2{13.5f, -7.25f}, math::vec2{-250.0f, 99.0f}}) {
            const f32 h = water_height_at(w, &f, p, t);
            CHECK(same_bits(h, water_height_at(w, &f, p, t + 64.0)));
            CHECK(same_bits(h, water_height_at(w, &f, p, t + 6400.0)));
            CHECK(same_bits(h, water_height(set, p, f32(t))));
            const math::vec3 a = water_normal_at(w, &f, p, t), b = water_normal_at(w, &f, p, t + 64.0);
            CHECK((same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z)));
        }
    }
    // and the current changes the surface (not vacuous)
    CHECK_FALSE(same_bits(water_height_at(w, &f, math::vec2{3.0f, 4.0f}, 7.0),
                          water_height_at(w, math::vec2{3.0f, 4.0f}, 7.0)));
}

namespace {

// The largest |h_a(x + shift, t) - h_b(x, t)| over a line of points along `dir` (steepness 0: no horizontal
// displacement, so the height is a plain sum of sines).
f32 max_mismatch(const WaterWaveSet& a, const WaterWaveSet& b, math::vec2 shift, f32 t, math::vec2 dir) {
    f32 m = 0.0f;
    for (f32 s = -30.0f; s <= 30.0f; s += 0.173f) {
        const math::vec2 p{dir.x * s + 0.37f * dir.y, dir.y * s - 0.37f * dir.x};
        m = std::max(m,
                     std::fabs(water_height(a, math::vec2{p.x + shift.x, p.y + shift.y}, t) - water_height(b, p, t)));
    }
    return m;
}

}  // namespace

TEST_CASE("water flow: a wave's crest is carried by U * 10 s within its rounding bound") {
    constexpr f32 kT = 10.0f;
    const f32 speed = 1.2f;
    for (const f32 wavelength : {1.4f, 24.0f}) {
        Water w;
        w.wave_count = 1;
        w.amplitude = 0.05f;
        w.wavelength = wavelength;
        w.steepness = 0.0f;
        w.wind_dir_deg = 0.0f;
        const WaterFlow f = flow_of(0.0f, speed);
        const math::vec2 u = water_flow_velocity(&f);
        const WaterWaveSet still = water_wave_set(w, 0.0f), flow = water_wave_set_flow(w, 0.0f, u);
        const f32 k = still.wave[0].k;
        // the drift speed (the omega difference over k) is U within (pi / 64) / k
        const f64 drift = (f64(flow.wave[0].omega) - f64(still.wave[0].omega)) / f64(k);
        const f64 bound = (kBase / 2.0) / f64(k);
        MESSAGE("wavelength " << wavelength << " m: drift " << drift << " m/s for a " << speed << " m/s current (bound "
                              << bound << " m/s, crest error after 10 s at most " << bound * kT << " m)");
        CHECK(std::fabs(drift - f64(speed)) <= bound + 1e-6);
        // the flowing pattern at time T is the still one at T carried by U * T, within A * (pi / 64) * T
        const math::vec2 shift{u.x * kT, u.y * kT};
        const f32 err = max_mismatch(flow, still, shift, kT, math::vec2{1.0f, 0.0f});
        const f32 allowed = w.amplitude * f32(kBase / 2.0) * kT + 1e-4f;
        const f32 control = max_mismatch(still, still, shift, kT, math::vec2{1.0f, 0.0f});
        MESSAGE("  height mismatch " << err << " m (allowed " << allowed << "), without the current " << control
                                     << " m");
        CHECK(err <= allowed);
        if (wavelength < 2.0f) {
            CHECK(bound * kT < 0.12);  // a 1.4 m wave follows a 1.2 m/s current to about a decimetre in 10 s
            CHECK(control > 4.0f * err);
        } else {
            CHECK(bound * kT < 2.0);  // the 24 m swell moves in steps of 0.19 m/s
        }
    }
}

TEST_CASE("water flow: a current across the wind carries the pattern across") {
    constexpr f32 kT = 2.0f;
    Water w;
    w.wave_count = 8;
    w.amplitude = 0.1f;
    w.wavelength = 3.0f;
    w.steepness = 0.0f;
    w.wind_dir_deg = 0.0f;
    const WaterFlow f = flow_of(90.0f, 1.5f);  // along +z, across the main direction
    const math::vec2 u = water_flow_velocity(&f);
    const WaterWaveSet still = water_wave_set(w, 0.0f), flow = water_wave_set_flow(w, 0.0f, u);
    CHECK(same_bits(flow.wave[0].omega, still.wave[0].omega));  // the main wave runs straight across the current
    u32 moved = 0;
    f32 allowed = 1e-4f;
    for (u32 i = 0; i < flow.count; ++i) {
        moved += same_bits(flow.wave[i].omega, still.wave[i].omega) ? 0u : 1u;
        allowed += still.wave[i].amplitude * f32(kBase / 2.0) * kT;
    }
    CHECK(moved >= 6);
    const math::vec2 shift{u.x * kT, u.y * kT};
    const f32 err = max_mismatch(flow, still, shift, kT, math::vec2{0.0f, 1.0f});
    const f32 control = max_mismatch(still, still, shift, kT, math::vec2{0.0f, 1.0f});
    MESSAGE("across: mismatch " << err << " m (allowed " << allowed << "), without the current " << control << " m");
    CHECK(err <= allowed);
    CHECK(control > 2.0f * allowed);
}

TEST_CASE("water flow: an out-of-range velocity clamps as the carrier does") {
    Water w;
    w.wave_count = 4;
    CHECK(same_set(water_wave_set_flow(w, 0.0f, math::vec2{9.0f, -12.0f}),
                   water_wave_set_flow(w, 0.0f, math::vec2{5.0f, -5.0f})));
    CHECK(same_set(water_wave_set_flow(w, 0.0f, math::vec2{kNaN, 2.0f}),
                   water_wave_set_flow(w, 0.0f, math::vec2{0.0f, 2.0f})));
}

namespace {

struct Rig {
    World w;
    u64 t = 0;
    Entity wxe, we, fe;
    explicit Rig(bool with_flow) {
        w.register_components<SaveKey>();
        install_weather(w, nullptr);
        Weather c;
        c.target = WeatherKind::Rain;
        c.transition_seconds = 0.5f;
        wxe = w.spawn_with(SaveKey{1}, c);
        Water water;
        water.amplitude = 0.3f;
        we = w.spawn_with(SaveKey{2}, water);
        if (with_flow) fe = w.spawn_with(SaveKey{3}, flow_of(75.0f, 1.25f));
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
};

}  // namespace

TEST_CASE("water flow: the component round-trips save and load") {
    Array<byte> bytes;
    u64 hash_a = 0;
    {
        Rig a(true);
        WaterFlow& f = *a.w.try_get_mut<WaterFlow>(a.fe);
        f.enabled = 0;
        f.dir_deg = 212.5f;
        f.speed = 3.75f;
        a.run(30);
        save::write_save(a.w, {}, bytes);
        hash_a = a.w.state_hash();
    }
    Rig b(true);
    b.run(5);
    REQUIRE(save::read_save(b.w, bytes.span()));
    const WaterFlow& f = *b.w.try_get<WaterFlow>(b.fe);
    CHECK(f.enabled == 0);
    CHECK(f.dir_deg == 212.5f);
    CHECK(f.speed == 3.75f);
    CHECK(b.w.state_hash() == hash_a);
}

TEST_CASE(
    "water flow: the Water and Weather columns are the same with and without a WaterFlow, which is never written") {
    Rig a(false), b(true);
    for (u32 i = 0; i < 300; ++i) {
        a.run(1);
        b.run(1);
        const Weather& wa = *a.w.try_get<Weather>(a.wxe);
        const Weather& wb = *b.w.try_get<Weather>(b.wxe);
        // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison): both copies come from the same writer, padding
        // included
        CHECK(std::memcmp(&wa, &wb, sizeof(Weather)) == 0);
        const Water& ma = *a.w.try_get<Water>(a.we);
        const Water& mb = *b.w.try_get<Water>(b.we);
        // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison): both copies come from the same writer, padding
        // included
        CHECK(std::memcmp(&ma, &mb, sizeof(Water)) == 0);
    }
    const WaterFlow& f = *b.w.try_get<WaterFlow>(b.fe);
    CHECK(f.enabled == 1);
    CHECK(f.dir_deg == 75.0f);
    CHECK(f.speed == 1.25f);
    CHECK(f.pad_[0] == 0);
    Rig c(false), d(false);
    c.run(300);
    d.run(300);
    CHECK(c.w.state_hash() == d.w.state_hash());
}
