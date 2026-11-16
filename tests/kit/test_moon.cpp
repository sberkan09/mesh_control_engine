// Moon (M-NIGHTSKY 14a): the phase is 0.5 at day 0 hour 0 with the default offset, cycles after cycle_days and wraps;
// freeze_phase pins it; the illumination is 0 at new, 1 at full and monotone to full; a full moon is opposite the sun
// and a new moon is with it; the direction is a unit vector; the level is 0 below the horizon and with time_of_day -1;
// the weather damps it per kind and no Weather means none; NaN, inverted and zero ranges fall back; the helpers agree
// in one run and in a split run around a save; the component round-trips save/load; the WorldClock, DayCycle and
// Weather column hashes are the same with and without a Moon.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/world.h"
#include "mce/kit/day_cycle.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/moon.h"
#include "mce/kit/weather.h"
#include "mce/kit/world_clock.h"
#include "mce/render/atmosphere.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }

f32 vdot(math::vec3 a, math::vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Weather wx(WeatherKind kind, f32 intensity) {
    Weather w;
    w.current = kind;
    w.target = kind;
    w.intensity = intensity;
    return w;
}

}  // namespace

TEST_CASE("moon: defaults") {
    const Moon m;
    CHECK(m.phase_offset == 0.5f);
    CHECK(m.cycle_days == 29.53f);
    CHECK(m.freeze_phase == -1.0f);
    CHECK(m.inclination_deg == 5.0f);
    CHECK(m.light_intensity == 0.12f);
    CHECK(m.light_color.x == 0.55f);
    CHECK(m.light_color.y == 0.65f);
    CHECK(m.light_color.z == 1.0f);
    CHECK(m.light_shadows == 1);
    CHECK(m.disk_size == 0.9990f);
    CHECK(m.disk_intensity == 6.0f);
    CHECK(m.star_density == 0.6f);
    CHECK(m.star_brightness == 1.0f);
    CHECK(m.star_twinkle == 0.3f);
    CHECK(m.enabled == 1);
    CHECK(m.pad_[0] == 0);
    CHECK(m.pad_[1] == 0);
}

TEST_CASE("moon: the phase at day 0 hour 0 is the offset, then cycles and wraps") {
    const Moon m;
    CHECK(moon_phase(m, 0, 0.0f) == 0.5f);
    CHECK(moon_phase(m, 0, 12.0f) == doctest::Approx(0.5f + 0.5f / 29.53f).epsilon(1e-5));
    CHECK(moon_phase(m, 1, 0.0f) == doctest::Approx(0.5f + 1.0f / 29.53f).epsilon(1e-5));
    Moon n = m;
    n.cycle_days = 10.0f;
    n.phase_offset = 0.0f;
    CHECK(moon_phase(n, 0, 0.0f) == 0.0f);
    CHECK(moon_phase(n, 5, 0.0f) == 0.5f);
    CHECK(same_bits(moon_phase(n, 10, 0.0f), 0.0f));  // a full cycle wraps to exactly 0
    CHECK(same_bits(moon_phase(n, 20, 0.0f), 0.0f));
    CHECK(moon_phase(n, 12, 0.0f) == doctest::Approx(0.2f));
    CHECK(moon_phase(n, 9, 23.0f) == doctest::Approx(0.9f + 23.0f / 240.0f));
    f32 prev = 0.0f;
    u32 wraps = 0;
    for (u32 day = 0; day < 40; ++day) {  // always in [0, 1), rising except at a wrap
        const f32 p = moon_phase(m, day, 6.0f);
        CHECK(p >= 0.0f);
        CHECK(p < 1.0f);
        if (day > 0 && p < prev) ++wraps;
        prev = p;
    }
    CHECK(wraps == 1);
}

TEST_CASE("moon: freeze_phase pins the phase") {
    Moon m;
    m.freeze_phase = 0.25f;
    for (u32 day : {0u, 3u, 100u})
        for (f32 hour : {0.0f, 13.0f, -1.0f}) CHECK(moon_phase(m, day, hour) == 0.25f);
    m.freeze_phase = 0.0f;
    CHECK(same_bits(moon_phase(m, 7, 5.0f), 0.0f));  // 0 pins to new, it does not follow the clock
    m.freeze_phase = 1.0f;
    CHECK(same_bits(moon_phase(m, 7, 5.0f), 0.0f));  // 1 wraps
    m.freeze_phase = -0.001f;
    CHECK(moon_phase(m, 0, 0.0f) == 0.5f);  // negative follows the clock
}

TEST_CASE("moon: the illumination is 0 at new, 1 at full and monotone to full") {
    CHECK(same_bits(moon_illumination(0.0f), 0.0f));
    CHECK(moon_illumination(0.5f) == 1.0f);
    CHECK(moon_illumination(0.25f) == doctest::Approx(0.5f).epsilon(1e-5));
    CHECK(moon_illumination(0.75f) == doctest::Approx(0.5f).epsilon(1e-5));
    f32 prev = 0.0f;
    for (u32 i = 0; i <= 500; ++i) {
        const f32 p = static_cast<f32>(i) / 1000.0f;
        const f32 l = moon_illumination(p);
        CHECK(l >= prev);
        CHECK(l <= 1.0f);
        CHECK(l == doctest::Approx(0.5f - 0.5f * std::cos(2.0f * math::kPi * p)).epsilon(1e-3));
        CHECK(moon_illumination(1.0f - p) == doctest::Approx(l).epsilon(1e-6));  // symmetric about full
        prev = l;
    }
    CHECK(moon_illumination(1.0f) == 0.0f);   // wraps
    CHECK(moon_illumination(1.5f) == 1.0f);
    CHECK(moon_illumination(-0.5f) == 1.0f);
    CHECK(same_bits(moon_illumination(kNaN), 0.0f));
}

TEST_CASE("moon: a full moon is opposite the sun and a new moon is with it") {
    Moon m;
    m.inclination_deg = 0.0f;
    for (f32 hour : {0.0f, 3.0f, 9.5f, 18.0f, 23.0f}) {
        const math::vec3 sun = render::sun_direction_at(hour);
        const math::vec3 full = moon_direction(m, hour, 0.5f);
        const math::vec3 now = moon_direction(m, hour, 0.0f);
        CHECK(vdot(full, sun) == doctest::Approx(-1.0f).epsilon(1e-4));
        CHECK(vdot(now, sun) == doctest::Approx(1.0f).epsilon(1e-4));
    }
    // a full moon rises at sunset (the sun is on the horizon at 18:00) and is up at midnight
    const math::vec3 sunset_full = moon_direction(m, 18.0f, 0.5f);
    CHECK(-sunset_full.y == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(-moon_direction(m, 0.0f, 0.5f).y > 0.8f);
    CHECK(-moon_direction(m, 12.0f, 0.0f).y > 0.8f);  // a new moon is up at noon
    // with the default inclination the moon is a few degrees off the arc, not far
    const Moon d;
    CHECK(vdot(moon_direction(d, 6.0f, 0.5f), render::sun_direction_at(6.0f)) < -0.99f);
    CHECK(vdot(moon_direction(d, 6.0f, 0.5f), render::sun_direction_at(6.0f)) > -0.9999f);  // but not on it
}

TEST_CASE("moon: the direction is a unit vector") {
    for (f32 inc : {0.0f, 5.0f, 30.0f})
        for (f32 hour = 0.0f; hour <= 24.0f; hour += 1.5f)
            for (f32 phase = 0.0f; phase < 1.0f; phase += 0.125f) {
                Moon m;
                m.inclination_deg = inc;
                const math::vec3 d = moon_direction(m, hour, phase);
                CHECK(vdot(d, d) == doctest::Approx(1.0f).epsilon(1e-5));
            }
    const Moon m;
    const math::vec3 bad = moon_direction(m, kNaN, kNaN);
    CHECK(vdot(bad, bad) == doctest::Approx(1.0f).epsilon(1e-5));
    CHECK(same_bits(bad.x, moon_direction(m, 0.0f, 0.0f).x));  // NaN means hour 0 and phase 0
    const math::vec3 neg = moon_direction(m, -1.0f, 0.3f);
    CHECK(same_bits(neg.y, moon_direction(m, 0.0f, 0.3f).y));
}

TEST_CASE("moon: the level is 0 below the horizon and with time_of_day -1") {
    Moon m;
    m.inclination_deg = 0.0f;
    m.freeze_phase = 0.5f;
    CHECK(same_bits(moon_level(m, 0, -1.0f, nullptr), 0.0f));
    CHECK(same_bits(moon_level(m, 0, kNaN, nullptr), 0.0f));
    CHECK(same_bits(moon_level(m, 0, 12.0f, nullptr), 0.0f));  // full moon at noon is below the horizon
    CHECK(same_bits(moon_level(m, 0, 15.0f, nullptr), 0.0f));
    CHECK(moon_level(m, 0, 0.0f, nullptr) > 0.9f);  // high at midnight
    CHECK(moon_level(m, 0, 0.0f, nullptr) <= 1.0f);
    f32 prev = 0.0f;
    for (f32 h = 18.0f; h <= 24.0f; h += 0.25f) {  // rises after sunset
        const f32 l = moon_level(m, 0, h, nullptr);
        CHECK(l >= 0.0f);
        CHECK(l <= 1.0f);
        if (h > 18.0f && h < 21.0f) CHECK(l >= prev);
        prev = l;
    }
    CHECK(same_bits(moon_level(m, 0, 18.0f, nullptr), 0.0f));
    m.freeze_phase = 0.0f;  // new moon: no light at all
    CHECK(same_bits(moon_level(m, 0, 12.0f, nullptr), 0.0f));
    m.freeze_phase = 0.25f;  // a half-lit moon trails the sun by 6 h: at 18:00 it stands where the sun stood at noon
    const f32 half = moon_level(m, 0, 18.0f, nullptr);
    CHECK(half == doctest::Approx(0.5f).epsilon(1e-3));
}

TEST_CASE("moon: the weather damps the level per kind and no Weather means none") {
    Moon m;
    m.freeze_phase = 0.5f;
    m.inclination_deg = 0.0f;
    const f32 clear = moon_level(m, 0, 0.0f, nullptr);
    REQUIRE(clear > 0.9f);
    const struct {
        WeatherKind kind;
        f32 factor;
    } rows[] = {{WeatherKind::Clear, 1.0f}, {WeatherKind::Overcast, 0.5f}, {WeatherKind::Rain, 0.3f},
                {WeatherKind::Storm, 0.15f}, {WeatherKind::Snow, 0.5f}};
    for (const auto& r : rows) {
        const Weather full = wx(r.kind, 1.0f);
        CHECK(moon_level(m, 0, 0.0f, &full) == doctest::Approx(clear * r.factor).epsilon(1e-5));
        const Weather none = wx(r.kind, 0.0f);  // intensity 0: no weather yet
        CHECK(same_bits(moon_level(m, 0, 0.0f, &none), clear));
        const Weather half = wx(r.kind, 0.5f);
        CHECK(moon_level(m, 0, 0.0f, &half) == doctest::Approx(clear * (1.0f + (r.factor - 1.0f) * 0.5f)).epsilon(1e-5));
    }
    const Weather clear_w = wx(WeatherKind::Clear, 0.0f);
    CHECK(same_bits(moon_level(m, 0, 0.0f, nullptr), moon_level(m, 0, 0.0f, &clear_w)));
    const Weather bad = wx(WeatherKind::Storm, kNaN);  // NaN intensity counts as 0
    CHECK(same_bits(moon_level(m, 0, 0.0f, &bad), clear));
}

TEST_CASE("moon: NaN, inverted and zero ranges fall back or clamp") {
    const Moon d;
    Moon n;
    n.phase_offset = kNaN;
    n.cycle_days = kNaN;
    n.freeze_phase = kNaN;
    n.inclination_deg = kNaN;
    for (u32 day : {0u, 4u, 31u})
        for (f32 hour : {0.0f, 7.5f, 20.0f}) {
            CHECK(same_bits(moon_phase(n, day, hour), moon_phase(d, day, hour)));  // every NaN means the default
            CHECK(same_bits(moon_level(n, day, hour, nullptr), moon_level(d, day, hour, nullptr)));
            const f32 p = moon_phase(d, day, hour);
            const math::vec3 a = moon_direction(n, hour, p), b = moon_direction(d, hour, p);
            CHECK(same_bits(a.x, b.x));
            CHECK(same_bits(a.y, b.y));
            CHECK(same_bits(a.z, b.z));
        }

    Moon z = d;  // a zero or negative or tiny cycle clamps to 2 days: no division by zero
    z.cycle_days = 0.0f;
    Moon two = d;
    two.cycle_days = 2.0f;
    Moon neg = d;
    neg.cycle_days = -5.0f;
    CHECK(same_bits(moon_phase(z, 3, 5.0f), moon_phase(two, 3, 5.0f)));
    CHECK(same_bits(moon_phase(neg, 3, 5.0f), moon_phase(two, 3, 5.0f)));
    Moon big = d;  // above 120 clamps to 120
    big.cycle_days = 1.0e9f;
    Moon c120 = d;
    c120.cycle_days = 120.0f;
    CHECK(same_bits(moon_phase(big, 50, 1.0f), moon_phase(c120, 50, 1.0f)));
    Moon inf = d;
    inf.cycle_days = std::numeric_limits<f32>::infinity();
    CHECK(same_bits(moon_phase(inf, 50, 1.0f), moon_phase(c120, 50, 1.0f)));

    Moon off = d;  // offset outside 0..1 clamps
    off.phase_offset = 7.0f;
    Moon one = d;
    one.phase_offset = 1.0f;
    CHECK(same_bits(moon_phase(off, 2, 2.0f), moon_phase(one, 2, 2.0f)));
    off.phase_offset = -3.0f;
    Moon zero = d;
    zero.phase_offset = 0.0f;
    CHECK(same_bits(moon_phase(off, 2, 2.0f), moon_phase(zero, 2, 2.0f)));

    Moon fz = d;  // freeze above 1 clamps to 1, which wraps to 0
    fz.freeze_phase = 5.0f;
    CHECK(same_bits(moon_phase(fz, 0, 0.0f), 0.0f));

    Moon inc = d;  // inclination clamps to 0..30
    inc.inclination_deg = 90.0f;
    Moon i30 = d;
    i30.inclination_deg = 30.0f;
    const math::vec3 a = moon_direction(inc, 3.0f, 0.4f), b = moon_direction(i30, 3.0f, 0.4f);
    CHECK(same_bits(a.x, b.x));
    CHECK(same_bits(a.y, b.y));
    CHECK(same_bits(a.z, b.z));

    CHECK(moon_phase(d, 0, kNaN) == 0.5f);  // a NaN hour counts as 0 for the phase
    CHECK(moon_phase(d, 0, 48.0f) == doctest::Approx(0.5f).epsilon(1e-5));  // above 24 wraps
    CHECK(moon_phase(d, 0, std::numeric_limits<f32>::infinity()) >= 0.0f);   // no crash, in range
}

TEST_CASE("moon: a zero level is exactly +0.0f and the level stays in 0..1") {
    const Moon m;
    for (u32 day = 0; day < 30; ++day)
        for (f32 hour = -1.0f; hour <= 24.0f; hour += 1.0f) {
            const f32 l = moon_level(m, day, hour, nullptr);
            CHECK(l >= 0.0f);
            CHECK(l <= 1.0f);
            if (l == 0.0f) CHECK(same_bits(l, 0.0f));
        }
}

namespace {

struct Rig {
    World w;
    u64 t = 0;
    Entity wxe, me, ce, de;
    explicit Rig(bool with_moon, f32 hour = 20.0f, bool with_day_cycle = false) {
        w.register_components<SaveKey>();
        install_weather(w, nullptr);
        if (Physics3D* p = w.try_resource<Physics3D>()) p->auto_step = 0;
        Weather c;
        c.target = WeatherKind::Rain;
        c.transition_seconds = 0.5f;
        wxe = w.spawn_with(SaveKey{1}, c);
        if (with_moon) {
            Moon m;
            m.enabled = 0;
            me = w.spawn_with(SaveKey{2}, m);
        }
        WorldClock k;
        k.hour = hour;
        k.day_length_seconds = 20.0f;  // 24 h in 1200 ticks
        ce = w.spawn_with(SaveKey{3}, k);
        if (with_day_cycle) de = w.spawn_with(SaveKey{4}, DayCycle{});
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    // The helpers on the current clock, packed into bits so a run can be compared exactly.
    std::vector<u32> sample() {
        const Environment3D* env = w.try_resource<Environment3D>();
        const WorldClock* wc = w.try_get<WorldClock>(ce);
        const Moon* m = w.try_get<Moon>(me);
        const Weather* wt = w.try_get<Weather>(wxe);
        const f32 phase = moon_phase(*m, wc->day, env->time_of_day);
        const math::vec3 d = moon_direction(*m, env->time_of_day, phase);
        return {std::bit_cast<u32>(phase), std::bit_cast<u32>(moon_illumination(phase)), std::bit_cast<u32>(d.x),
                std::bit_cast<u32>(d.y), std::bit_cast<u32>(d.z),
                std::bit_cast<u32>(moon_level(*m, wc->day, env->time_of_day, wt))};
    }
};

}  // namespace

TEST_CASE("moon: the component round-trips save and load") {
    Array<byte> bytes;
    u64 hash_a = 0;
    {
        Rig a(true);
        Moon& m = *a.w.try_get_mut<Moon>(a.me);
        m.phase_offset = 0.25f;
        m.cycle_days = 12.5f;
        m.freeze_phase = 0.75f;
        m.inclination_deg = 11.0f;
        m.light_intensity = 0.4f;
        m.light_color = {0.1f, 0.2f, 0.3f};
        m.light_shadows = 0;
        m.disk_size = 0.995f;
        m.disk_intensity = 12.0f;
        m.star_density = 0.3f;
        m.star_brightness = 2.0f;
        m.star_twinkle = 0.7f;
        m.enabled = 1;
        a.run(30);
        save::write_save(a.w, {}, bytes);
        hash_a = a.w.state_hash();
    }
    Rig b(true);
    b.run(5);
    REQUIRE(save::read_save(b.w, bytes.span()));
    const Moon& m = *b.w.try_get<Moon>(b.me);
    CHECK(m.phase_offset == 0.25f);
    CHECK(m.cycle_days == 12.5f);
    CHECK(m.freeze_phase == 0.75f);
    CHECK(m.inclination_deg == 11.0f);
    CHECK(m.light_intensity == 0.4f);
    CHECK(m.light_color.x == 0.1f);
    CHECK(m.light_color.y == 0.2f);
    CHECK(m.light_color.z == 0.3f);
    CHECK(m.light_shadows == 0);
    CHECK(m.disk_size == 0.995f);
    CHECK(m.disk_intensity == 12.0f);
    CHECK(m.star_density == 0.3f);
    CHECK(m.star_brightness == 2.0f);
    CHECK(m.star_twinkle == 0.7f);
    CHECK(m.enabled == 1);
    CHECK(b.w.state_hash() == hash_a);
}

TEST_CASE("moon: the WorldClock, DayCycle and Weather columns are the same with and without a Moon") {
    Rig a(false, 20.0f, true), b(true, 20.0f, true);
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
        const DayCycle& da = *a.w.try_get<DayCycle>(a.de);
        const DayCycle& db = *b.w.try_get<DayCycle>(b.de);
        // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison): both copies come from the same writer, padding included
        CHECK(std::memcmp(&da, &db, sizeof(DayCycle)) == 0);
    }
    // the moon code never writes the component either: it is still as authored
    const Moon& m = *b.w.try_get<Moon>(b.me);
    CHECK(m.phase_offset == 0.5f);
    CHECK(m.enabled == 0);
    // a scene without one has one more column in the hash only through the component itself
    Rig c(false), d(false);
    c.run(300);
    d.run(300);
    CHECK(c.w.state_hash() == d.w.state_hash());
}

TEST_CASE("moon: the helpers at fixed ticks of a WorldClock run are the same in one go and in two halves") {
    constexpr u32 kN = 3600;  // three clock days
    constexpr u32 kEvery = 100;
    std::vector<std::vector<u32>> whole;
    {
        Rig a(true, 20.0f);
        for (u32 i = 0; i < kN; ++i) {
            a.run(1);
            if ((i + 1) % kEvery == 0) whole.push_back(a.sample());
        }
        CHECK(a.w.try_get<WorldClock>(a.ce)->day >= 2);
    }
    CHECK(whole.size() == kN / kEvery);
    bool phase_moves = false, level_moves = false;
    for (usize i = 1; i < whole.size(); ++i) {
        phase_moves = phase_moves || whole[i][0] != whole[i - 1][0];
        level_moves = level_moves || whole[i][5] != whole[i - 1][5];
    }
    CHECK(phase_moves);
    CHECK(level_moves);

    std::vector<std::vector<u32>> halves;
    Array<byte> bytes;
    {
        Rig a(true, 20.0f);
        for (u32 i = 0; i < kN / 2; ++i) {
            a.run(1);
            if ((i + 1) % kEvery == 0) halves.push_back(a.sample());
        }
        save::write_save(a.w, {}, bytes);
    }
    Rig b(true, 20.0f);
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

TEST_CASE("moon: a save at midnight gives the same phase") {
    Array<byte> bytes;
    f32 phase_a = 0.0f;
    {
        Rig a(true, 23.9f);
        a.run(60);  // crosses midnight: the day counter is 1
        const WorldClock& k = *a.w.try_get<WorldClock>(a.ce);
        CHECK(k.day == 1);
        phase_a = moon_phase(*a.w.try_get<Moon>(a.me), k.day, a.w.try_resource<Environment3D>()->time_of_day);
        save::write_save(a.w, {}, bytes);
    }
    Rig b(true, 5.0f);
    REQUIRE(save::read_save(b.w, bytes.span()));
    const WorldClock& k = *b.w.try_get<WorldClock>(b.ce);
    CHECK(k.day == 1);
    b.run(0);
    CHECK(moon_phase(*b.w.try_get<Moon>(b.me), k.day, k.hour) == doctest::Approx(phase_a).epsilon(1e-3));
}
