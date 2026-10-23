// Water (M-WATER 15a): zero amplitude gives exactly the level and an up normal; the height stays within the sum of the
// amplitudes; the pattern repeats after the common period; the normal agrees with a finite difference of the height;
// steepness 1 with 8 waves never folds over; the direction rotates the pattern; NaN, inverted and zero ranges fall back
// or clamp; the helpers agree in one run and in a split run around a save; the component round-trips save/load; the
// Weather, WorldClock and WindDirection column hashes are the same with and without a Water.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/water.h"
#include "mce/kit/weather.h"
#include "mce/kit/wind_direction.h"
#include "mce/kit/world_clock.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }

bool same_vec(math::vec3 a, math::vec3 b) { return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z); }

// Positions and times that exercise the surface: a grid around the origin, negative and large coordinates.
std::vector<math::vec2> probe_points() {
    std::vector<math::vec2> pts;
    for (f32 x = -60.0f; x <= 60.0f; x += 17.5f)
        for (f32 z = -45.0f; z <= 75.0f; z += 15.0f) pts.push_back({x, z});
    pts.push_back({12345.5f, -9876.25f});
    return pts;
}

constexpr f32 kTimes[] = {0.0f, 0.5f, 3.25f, 17.0f, 40.75f, 63.5f};

f32 amplitude_sum(const WaterWaveSet& s) {
    f32 sum = 0.0f;
    for (u32 i = 0; i < s.count; ++i) sum += s.wave[i].amplitude;
    return sum;
}

}  // namespace

TEST_CASE("water: defaults") {
    const Water w;
    CHECK(w.enabled == 1);
    CHECK(w.level_y == 0.0f);
    CHECK(w.half_extent == 500.0f);
    CHECK(w.wave_count == 4);
    CHECK(w.amplitude == 0.4f);
    CHECK(w.wavelength == 24.0f);
    CHECK(w.steepness == 0.6f);
    CHECK(w.wind_dir_deg == 0.0f);
    CHECK(w.wind_follow == 1.0f);
    CHECK(w.speed_scale == 1.0f);
    CHECK(w.shallow_color.x == 0.10f);
    CHECK(w.shallow_color.y == 0.38f);
    CHECK(w.shallow_color.z == 0.40f);
    CHECK(w.deep_color.x == 0.01f);
    CHECK(w.deep_color.y == 0.08f);
    CHECK(w.deep_color.z == 0.15f);
    CHECK(w.depth_scale_m == 8.0f);
    CHECK(w.reflectivity == 1.0f);
    CHECK(w.roughness == 0.08f);
    CHECK(w.foam_amount == 0.5f);
    CHECK(w.foam_width_m == 1.5f);
    CHECK(w.pad_[0] == 0);
    CHECK(w.pad_[1] == 0);
}

TEST_CASE("water: the wave set comes from fixed tables") {
    const Water w;
    const WaterWaveSet s = water_wave_set(w);
    REQUIRE(s.count == 4);
    CHECK(s.wave[0].amplitude == doctest::Approx(0.4f));
    CHECK(s.wave[0].k == doctest::Approx(2.0f * math::kPi / 24.0f));
    CHECK(s.wave[0].dir.x == doctest::Approx(1.0f));
    CHECK(s.wave[0].dir.y == doctest::Approx(0.0f).epsilon(1e-6));
    for (u32 i = 1; i < s.count; ++i) {
        CHECK(s.wave[i].k > s.wave[i - 1].k);  // shorter every time
        CHECK(s.wave[i].amplitude < s.wave[i - 1].amplitude);
    }
    const f32 base = static_cast<f32>(2.0 * 3.14159265358979323846 / kWaterPeriodSeconds);
    for (u32 i = 0; i < s.count; ++i) {
        const WaterWave& v = s.wave[i];
        CHECK(v.dir.x * v.dir.x + v.dir.y * v.dir.y == doctest::Approx(1.0f).epsilon(1e-5));
        const f32 mult = v.omega / base;  // every omega is a whole multiple of the base frequency
        CHECK(mult == doctest::Approx(std::round(mult)).epsilon(1e-4));
        CHECK(mult >= 1.0f);
        CHECK(v.omega == doctest::Approx(water_phase_speed(v.k)).epsilon(0.5 * base / water_phase_speed(v.k)));
    }
    const WaterWaveSet again = water_wave_set(w);  // no rand, no state: the same set every time
    CHECK(same_bits(s.level_y, again.level_y));
    CHECK(s.count == again.count);
    for (u32 i = 0; i < s.count; ++i) {
        const WaterWave& a = s.wave[i];
        const WaterWave& b = again.wave[i];
        CHECK(same_bits(a.dir.x, b.dir.x));
        CHECK(same_bits(a.dir.y, b.dir.y));
        CHECK(same_bits(a.k, b.k));
        CHECK(same_bits(a.amplitude, b.amplitude));
        CHECK(same_bits(a.steepness, b.steepness));
        CHECK(same_bits(a.omega, b.omega));
        CHECK(same_bits(a.phase_seed, b.phase_seed));
    }
    Water many = w;
    many.wave_count = 8;
    CHECK(water_wave_set(many).count == 8);
    CHECK(water_phase_speed(0.0f) == 0.0f);
    CHECK(water_phase_speed(-1.0f) == 0.0f);
    CHECK(same_bits(water_phase_speed(kNaN), 0.0f));
    CHECK(water_phase_speed(1.0f) == doctest::Approx(std::sqrt(9.81f)));
    Water still = w;
    still.speed_scale = 0.0f;
    for (u32 i = 0; i < 4; ++i) CHECK(water_wave_set(still).wave[i].omega == 0.0f);
}

TEST_CASE("water: a zero amplitude gives exactly the level and an up normal") {
    Water w;
    w.amplitude = 0.0f;
    w.level_y = 3.5f;
    for (const math::vec2& p : probe_points())
        for (f32 t : kTimes) {
            CHECK(same_bits(water_height(w, p, t), 3.5f));
            CHECK(same_vec(water_normal(w, p, t), math::vec3{0.0f, 1.0f, 0.0f}));
        }
}

TEST_CASE("water: the height stays within the level plus or minus the sum of the amplitudes") {
    for (u32 count : {1u, 4u, 8u})
        for (f32 steep : {0.0f, 0.6f, 1.0f}) {
            Water w;
            w.wave_count = static_cast<u8>(count);
            w.steepness = steep;
            w.amplitude = 1.5f;
            w.level_y = -7.0f;
            const WaterWaveSet s = water_wave_set(w);
            const f32 bound = amplitude_sum(s) * 1.0001f + 1e-4f;
            f32 lo = 1.0e9f, hi = -1.0e9f;
            for (const math::vec2& p : probe_points())
                for (f32 t : kTimes) {
                    const f32 h = water_height(s, p, t);
                    CHECK(std::fabs(h - -7.0f) <= bound);
                    lo = std::min(lo, h);
                    hi = std::max(hi, h);
                }
            CHECK(hi - lo > 0.2f);  // and the surface does move
        }
}

TEST_CASE("water: the height at t equals the height at t plus the period") {
    const Water w;
    for (const math::vec2& p : probe_points())
        for (f32 t : kTimes) {
            const f32 period = static_cast<f32>(kWaterPeriodSeconds);
            CHECK(same_bits(water_height(w, p, t), water_height(w, p, t + period)));
            CHECK(same_bits(water_height(w, p, t), water_height(w, p, t + 5.0f * period)));
            CHECK(same_vec(water_normal(w, p, t), water_normal(w, p, t + 2.0f * period)));
        }
    CHECK(water_wrap_time(0.0) == 0.0f);
    CHECK(water_wrap_time(64.0) == 0.0f);
    CHECK(water_wrap_time(-1.0) == 63.0f);
    CHECK(water_wrap_time(1.0e7 + 0.25) == doctest::Approx(static_cast<f32>(std::fmod(1.0e7 + 0.25, 64.0))));
    CHECK(water_wrap_time(std::numeric_limits<f64>::infinity()) == 0.0f);
    CHECK(water_height(w, {1.0f, 2.0f}, 3.0f) != water_height(w, {1.0f, 2.0f}, 3.7f));  // it animates
}

TEST_CASE("water: the normal agrees with a finite difference of the height") {
    const Water w;
    const WaterWaveSet s = water_wave_set(w);
    constexpr f32 kH = 0.02f;
    for (f32 x = -30.0f; x <= 30.0f; x += 9.3f)
        for (f32 z = -30.0f; z <= 30.0f; z += 11.1f)
            for (f32 t : {0.0f, 4.5f, 20.25f}) {
                const f32 dhdx = (water_height(s, {x + kH, z}, t) - water_height(s, {x - kH, z}, t)) / (2.0f * kH);
                const f32 dhdz = (water_height(s, {x, z + kH}, t) - water_height(s, {x, z - kH}, t)) / (2.0f * kH);
                const math::vec3 fd{-dhdx, 1.0f, -dhdz};
                const f32 len = std::sqrt(fd.x * fd.x + fd.y * fd.y + fd.z * fd.z);
                const math::vec3 n = water_normal(s, {x, z}, t);
                CHECK(n.x == doctest::Approx(fd.x / len).epsilon(0.05).scale(1.0));
                CHECK(n.y == doctest::Approx(fd.y / len).epsilon(0.02));
                CHECK(n.z == doctest::Approx(fd.z / len).epsilon(0.05).scale(1.0));
                CHECK(n.x * n.x + n.y * n.y + n.z * n.z == doctest::Approx(1.0f).epsilon(1e-5));
            }
}

TEST_CASE("water: steepness 1 with 8 waves never folds over") {
    Water w;
    w.wave_count = 8;
    w.steepness = 1.0f;
    for (f32 amp : {0.4f, 3.0f, 10.0f})
        for (f32 len : {1.0f, 24.0f, 500.0f}) {
            w.amplitude = amp;
            w.wavelength = len;
            const WaterWaveSet s = water_wave_set(w);
            f32 q_sum = 0.0f;  // the folding term of the Gerstner normal at its worst
            for (u32 i = 0; i < s.count; ++i) q_sum += s.wave[i].steepness * s.wave[i].k * s.wave[i].amplitude;
            CHECK(q_sum <= 1.0f + 1e-5f);
            for (f32 x = -50.0f; x <= 50.0f; x += 3.7f)
                for (f32 z = -50.0f; z <= 50.0f; z += 4.3f)
                    for (f32 t : {0.0f, 1.25f, 9.0f, 33.5f}) {
                        const math::vec3 n = water_normal(s, {x, z}, t);
                        CHECK(n.y > 0.0f);
                        CHECK(std::isfinite(n.x));
                        CHECK(std::isfinite(n.z));
                    }
        }
}

TEST_CASE("water: the direction rotates the pattern") {
    const Water w;
    // A quarter turn of the main direction carries the pattern with it: (x, z) at 90 degrees is (z, -x) at 0 degrees.
    const WaterWaveSet s0 = water_wave_set(w, 0.0f), s90 = water_wave_set(w, 90.0f);
    f32 differs = 0.0f;
    for (const math::vec2& p : probe_points())
        for (f32 t : {0.0f, 7.5f}) {
            if (std::fabs(p.x) > 1000.0f) continue;  // keep the f32 rounding of the rotated phase small
            CHECK(water_height(s90, p, t) == doctest::Approx(water_height(s0, {p.y, -p.x}, t)).epsilon(1e-3).scale(0.4));
            differs = std::max(differs, std::fabs(water_height(s90, p, t) - water_height(s0, p, t)));
        }
    CHECK(differs > 0.1f);  // and it is not the same pattern
    // The authored direction is the default for the overload without one; 360 degrees is the same as 0.
    Water r = w;
    r.wind_dir_deg = 90.0f;
    CHECK(same_bits(water_height(r, {3.0f, 4.0f}, 2.0f), water_height(s90, {3.0f, 4.0f}, 2.0f)));
    const WaterWaveSet s360 = water_wave_set(w, 360.0f);
    CHECK(water_height(s360, {3.0f, 4.0f}, 2.0f) == doctest::Approx(water_height(s0, {3.0f, 4.0f}, 2.0f)).scale(0.4));
    CHECK(same_bits(water_height(water_wave_set(w, kNaN), {3.0f, 4.0f}, 2.0f), water_height(s0, {3.0f, 4.0f}, 2.0f)));
}

TEST_CASE("water: NaN, inverted and zero ranges fall back or clamp") {
    const Water d;
    Water n;
    n.level_y = kNaN;
    n.amplitude = kNaN;
    n.wavelength = kNaN;
    n.steepness = kNaN;
    n.speed_scale = kNaN;
    for (const math::vec2& p : probe_points())
        for (f32 t : kTimes) {
            CHECK(same_bits(water_height(n, p, t), water_height(d, p, t)));  // every NaN means the default
            CHECK(same_vec(water_normal(n, p, t), water_normal(d, p, t)));
        }

    const math::vec2 p{4.5f, -2.25f};
    Water z = d;  // a zero or negative wavelength clamps to 1 m: no division by zero
    z.wavelength = 0.0f;
    Water one = d;
    one.wavelength = 1.0f;
    Water neg = d;
    neg.wavelength = -9.0f;
    CHECK(same_bits(water_height(z, p, 1.0f), water_height(one, p, 1.0f)));
    CHECK(same_bits(water_height(neg, p, 1.0f), water_height(one, p, 1.0f)));
    Water big = d;
    big.wavelength = 1.0e9f;
    Water l500 = d;
    l500.wavelength = 500.0f;
    CHECK(same_bits(water_height(big, p, 1.0f), water_height(l500, p, 1.0f)));
    Water inf = d;
    inf.amplitude = std::numeric_limits<f32>::infinity();
    Water a10 = d;
    a10.amplitude = 10.0f;
    CHECK(same_bits(water_height(inf, p, 1.0f), water_height(a10, p, 1.0f)));
    Water na = d;  // negative amplitude is no amplitude
    na.amplitude = -3.0f;
    CHECK(same_bits(water_height(na, p, 1.0f), 0.0f));
    Water st = d;  // steepness clamps to 0..1
    st.steepness = 7.0f;
    Water st1 = d;
    st1.steepness = 1.0f;
    CHECK(same_bits(water_height(st, p, 1.0f), water_height(st1, p, 1.0f)));
    st.steepness = -1.0f;
    st1.steepness = 0.0f;
    CHECK(same_bits(water_height(st, p, 1.0f), water_height(st1, p, 1.0f)));
    Water lv = d;  // the level clamps to -1000..1000
    lv.level_y = 5.0e6f;
    lv.amplitude = 0.0f;
    CHECK(water_height(lv, p, 1.0f) == 1000.0f);

    Water wc = d;  // wave_count 0 counts as 1 and above 8 as 8
    wc.wave_count = 0;
    Water wc1 = d;
    wc1.wave_count = 1;
    CHECK(same_bits(water_height(wc, p, 1.0f), water_height(wc1, p, 1.0f)));
    wc.wave_count = 200;
    wc1.wave_count = 8;
    CHECK(water_wave_set(wc).count == 8);
    CHECK(same_bits(water_height(wc, p, 1.0f), water_height(wc1, p, 1.0f)));

    CHECK(same_bits(water_height(d, p, kNaN), water_height(d, p, 0.0f)));  // a NaN time counts as 0
    CHECK(std::isfinite(water_height(d, {kNaN, 1.0f}, 0.0f)) == false);    // a NaN position stays NaN, no crash
}

namespace {

struct Rig {
    World w;
    u64 t = 0;
    Entity wxe, we, ce, de;
    explicit Rig(bool with_water, f32 hour = 20.0f) {
        w.register_components<SaveKey>();
        install_weather(w, nullptr);
        if (Physics3D* p = w.try_resource<Physics3D>()) p->auto_step = 0;
        Weather c;
        c.target = WeatherKind::Rain;
        c.transition_seconds = 0.5f;
        wxe = w.spawn_with(SaveKey{1}, c);
        if (with_water) {
            Water water;
            water.enabled = 0;
            we = w.spawn_with(SaveKey{2}, water);
        }
        WorldClock k;
        k.hour = hour;
        k.day_length_seconds = 20.0f;
        ce = w.spawn_with(SaveKey{3}, k);
        WindDirection wd;
        wd.degrees = 30.0f;
        wd.drift_degrees_per_minute = 90.0f;
        de = w.spawn_with(SaveKey{4}, wd);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    // The scene time in seconds is the tick count in f64; the helpers on it packed into bits for an exact comparison.
    std::vector<u32> sample() {
        const Water* water = w.try_get<Water>(we);
        const f64 time = static_cast<f64>(t) * static_cast<f64>(kDt);
        const f32 tw = water_wrap_time(time);
        std::vector<u32> bits;
        for (const math::vec2 p : {math::vec2{0.0f, 0.0f}, math::vec2{13.5f, -7.25f}, math::vec2{-250.0f, 99.0f}}) {
            const math::vec3 n = water_normal(*water, p, tw);
            bits.push_back(std::bit_cast<u32>(water_height(*water, p, tw)));
            bits.push_back(std::bit_cast<u32>(n.x));
            bits.push_back(std::bit_cast<u32>(n.y));
            bits.push_back(std::bit_cast<u32>(n.z));
        }
        return bits;
    }
};

}  // namespace

TEST_CASE("water: the component round-trips save and load") {
    Array<byte> bytes;
    u64 hash_a = 0;
    {
        Rig a(true);
        Water& m = *a.w.try_get_mut<Water>(a.we);
        m.enabled = 1;
        m.level_y = -3.5f;
        m.half_extent = 750.0f;
        m.wave_count = 6;
        m.amplitude = 1.25f;
        m.wavelength = 40.0f;
        m.steepness = 0.3f;
        m.wind_dir_deg = 123.0f;
        m.wind_follow = 0.0f;
        m.speed_scale = 2.5f;
        m.shallow_color = {0.1f, 0.2f, 0.3f};
        m.deep_color = {0.4f, 0.5f, 0.6f};
        m.depth_scale_m = 30.0f;
        m.reflectivity = 0.7f;
        m.roughness = 0.2f;
        m.foam_amount = 0.9f;
        m.foam_width_m = 3.0f;
        a.run(30);
        save::write_save(a.w, {}, bytes);
        hash_a = a.w.state_hash();
    }
    Rig b(true);
    b.run(5);
    REQUIRE(save::read_save(b.w, bytes.span()));
    const Water& m = *b.w.try_get<Water>(b.we);
    CHECK(m.enabled == 1);
    CHECK(m.level_y == -3.5f);
    CHECK(m.half_extent == 750.0f);
    CHECK(m.wave_count == 6);
    CHECK(m.amplitude == 1.25f);
    CHECK(m.wavelength == 40.0f);
    CHECK(m.steepness == 0.3f);
    CHECK(m.wind_dir_deg == 123.0f);
    CHECK(m.wind_follow == 0.0f);
    CHECK(m.speed_scale == 2.5f);
    CHECK(m.shallow_color.x == 0.1f);
    CHECK(m.shallow_color.y == 0.2f);
    CHECK(m.shallow_color.z == 0.3f);
    CHECK(m.deep_color.x == 0.4f);
    CHECK(m.deep_color.y == 0.5f);
    CHECK(m.deep_color.z == 0.6f);
    CHECK(m.depth_scale_m == 30.0f);
    CHECK(m.reflectivity == 0.7f);
    CHECK(m.roughness == 0.2f);
    CHECK(m.foam_amount == 0.9f);
    CHECK(m.foam_width_m == 3.0f);
    CHECK(b.w.state_hash() == hash_a);
}

TEST_CASE("water: the Weather, WorldClock and WindDirection columns are the same with and without a Water") {
    Rig a(false), b(true);
    for (u32 i = 0; i < 300; ++i) {
        a.run(1);
        b.run(1);
        const Weather& wa = *a.w.try_get<Weather>(a.wxe);
        const Weather& wb = *b.w.try_get<Weather>(b.wxe);
        // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison): both copies come from the same writer, padding included
        CHECK(std::memcmp(&wa, &wb, sizeof(Weather)) == 0);
        const WorldClock& ka = *a.w.try_get<WorldClock>(a.ce);
        const WorldClock& kb = *b.w.try_get<WorldClock>(b.ce);
        // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison): both copies come from the same writer, padding included
        CHECK(std::memcmp(&ka, &kb, sizeof(WorldClock)) == 0);
        const WindDirection& da = *a.w.try_get<WindDirection>(a.de);
        const WindDirection& db = *b.w.try_get<WindDirection>(b.de);
        // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison): both copies come from the same writer, padding included
        CHECK(std::memcmp(&da, &db, sizeof(WindDirection)) == 0);
    }
    // the water code never writes the component either: it is still as authored
    const Water& m = *b.w.try_get<Water>(b.we);
    CHECK(m.amplitude == 0.4f);
    CHECK(m.enabled == 0);
    Rig c(false), d(false);
    c.run(300);
    d.run(300);
    CHECK(c.w.state_hash() == d.w.state_hash());
}

TEST_CASE("water: the helpers at fixed ticks are the same in one go and in two halves") {
    constexpr u32 kN = 4000;  // longer than one wave period (64 s = 3840 ticks)
    constexpr u32 kEvery = 100;
    std::vector<std::vector<u32>> whole;
    {
        Rig a(true);
        for (u32 i = 0; i < kN; ++i) {
            a.run(1);
            if ((i + 1) % kEvery == 0) whole.push_back(a.sample());
        }
    }
    CHECK(whole.size() == kN / kEvery);
    bool moves = false;
    for (usize i = 1; i < whole.size(); ++i) moves = moves || whole[i][0] != whole[i - 1][0];
    CHECK(moves);

    std::vector<std::vector<u32>> halves;
    Array<byte> bytes;
    {
        Rig a(true);
        for (u32 i = 0; i < kN / 2; ++i) {
            a.run(1);
            if ((i + 1) % kEvery == 0) halves.push_back(a.sample());
        }
        save::write_save(a.w, {}, bytes);
    }
    Rig b(true);
    b.run(11);  // another state, replaced by the save
    REQUIRE(save::read_save(b.w, bytes.span()));
    b.t = kN / 2;
    for (u32 i = kN / 2; i < kN; ++i) {
        b.run(1);
        if ((i + 1) % kEvery == 0) halves.push_back(b.sample());
    }
    REQUIRE(halves.size() == whole.size());
    for (usize i = 0; i < whole.size(); ++i) CHECK(whole[i] == halves[i]);
}
