// GroundMist (M-GROUNDMIST 13a): mist_level is 0 far from the peak and at the peak 1 for a wet dawn; wraps over
// midnight; base_humidity, wet_gain and wetness compose; the weather factor per kind and weather_damp 0; no Weather
// means no damping and wetness 0; time_of_day -1 gives 0; NaN and out-of-range authoring fall back or clamp; the level
// is monotone in strength; a zero level is exactly 0.0f; the component round-trips save/load; the Weather column hash
// is the same with and without a GroundMist; the level at fixed ticks of a WorldClock run is the same in one go and in
// two halves around a save.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/world.h"
#include "mce/kit/ground_mist.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/weather.h"
#include "mce/kit/world_clock.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }

// A mist that is 1 at its peak on a wet day: humidity 1 (0.25 + 0.75 * wetness 1).
GroundMist wet_mist() {
    GroundMist m;
    return m;
}

Weather wx(WeatherKind kind, f32 intensity, f32 wetness) {
    Weather w;
    w.current = kind;
    w.target = kind;
    w.intensity = intensity;
    w.wetness = wetness;
    return w;
}

}  // namespace

TEST_CASE("ground_mist: defaults") {
    const GroundMist m;
    CHECK(m.strength == 1.0f);
    CHECK(m.peak_hour == 6.0f);
    CHECK(m.rise_hours == 3.0f);
    CHECK(m.burn_hours == 3.0f);
    CHECK(m.base_humidity == 0.25f);
    CHECK(m.wet_gain == 0.75f);
    CHECK(m.weather_damp == 1.0f);
    CHECK(m.density == 0.06f);
    CHECK(m.thickness_m == 6.0f);
    CHECK(m.base_height_m == 0.0f);
    CHECK(m.exp2_density == 0.02f);
    CHECK(m.color.x == 0.78f);
    CHECK(m.color.y == 0.82f);
    CHECK(m.color.z == 0.88f);
    CHECK(m.enabled == 1);
    CHECK(m.pad_[0] == 0);
    CHECK(m.pad_[1] == 0);
    CHECK(m.pad_[2] == 0);
}

TEST_CASE("ground_mist: the bump is 0 far from the peak, 1 at it and smooth between") {
    const GroundMist m;
    const Weather wet = wx(WeatherKind::Clear, 0.0f, 1.0f);
    CHECK(mist_level(m, 6.0f, &wet) == 1.0f);
    CHECK(same_bits(mist_level(m, 12.0f, &wet), 0.0f));  // burn_hours after the peak and beyond
    CHECK(same_bits(mist_level(m, 15.0f, &wet), 0.0f));
    CHECK(same_bits(mist_level(m, 3.0f, &wet), 0.0f));   // rise_hours before the peak
    CHECK(same_bits(mist_level(m, 20.0f, &wet), 0.0f));
    CHECK(mist_level(m, 4.5f, &wet) == doctest::Approx(0.5f));  // smoothstep(0.5)
    CHECK(mist_level(m, 7.5f, &wet) == doctest::Approx(0.5f));
    f32 prev = 0.0f;
    for (f32 h = 3.0f; h <= 6.0f; h += 0.125f) {  // rising side is monotone
        const f32 l = mist_level(m, h, &wet);
        CHECK(l >= prev);
        prev = l;
    }
    for (f32 h = 6.0f; h <= 12.0f; h += 0.125f) {  // falling side is monotone
        const f32 l = mist_level(m, h, &wet);
        CHECK(l <= prev);
        prev = l;
    }
}

TEST_CASE("ground_mist: the bump wraps over midnight") {
    GroundMist m;
    m.peak_hour = 23.0f;
    m.rise_hours = 2.0f;
    m.burn_hours = 4.0f;
    const Weather wet = wx(WeatherKind::Clear, 0.0f, 1.0f);
    CHECK(mist_level(m, 23.0f, &wet) == 1.0f);
    CHECK(mist_level(m, 1.0f, &wet) > 0.0f);   // 2 h after the peak, across midnight, of a 4 h burn-off
    CHECK(mist_level(m, 1.0f, &wet) == doctest::Approx(0.5f));
    CHECK(same_bits(mist_level(m, 3.0f, &wet), 0.0f));  // 4 h after
    CHECK(mist_level(m, 22.0f, &wet) == doctest::Approx(0.5f));
    CHECK(same_bits(mist_level(m, 21.0f, &wet), 0.0f));
    GroundMist e = m;
    e.peak_hour = 0.0f;  // the peak on the wrap point itself
    CHECK(mist_level(e, 0.0f, &wet) == 1.0f);
    CHECK(mist_level(e, 24.0f, &wet) == 1.0f);
    CHECK(mist_level(e, 23.0f, &wet) == doctest::Approx(0.5f));  // 1 h before, rise 2 h
    CHECK(mist_level(e, 2.0f, &wet) == doctest::Approx(0.5f));   // 2 h after, burn 4 h
}

TEST_CASE("ground_mist: base_humidity, wet_gain and wetness compose") {
    GroundMist m;
    m.base_humidity = 0.25f;
    m.wet_gain = 0.5f;
    const Weather dry = wx(WeatherKind::Clear, 0.0f, 0.0f);
    const Weather half = wx(WeatherKind::Clear, 0.0f, 0.5f);
    const Weather full = wx(WeatherKind::Clear, 0.0f, 1.0f);
    CHECK(mist_level(m, 6.0f, &dry) == doctest::Approx(0.25f));
    CHECK(mist_level(m, 6.0f, &half) == doctest::Approx(0.5f));
    CHECK(mist_level(m, 6.0f, &full) == doctest::Approx(0.75f));
    m.base_humidity = 0.8f;
    CHECK(mist_level(m, 6.0f, &full) == 1.0f);  // humidity clamps to 1
    m.base_humidity = 0.0f;
    m.wet_gain = 0.0f;
    CHECK(same_bits(mist_level(m, 6.0f, &full), 0.0f));  // nothing to make mist from
}

TEST_CASE("ground_mist: the weather factor per kind and weather_damp") {
    GroundMist m;
    m.base_humidity = 1.0f;
    const struct {
        WeatherKind kind;
        f32 factor;
    } rows[] = {{WeatherKind::Clear, 1.0f}, {WeatherKind::Overcast, 0.8f}, {WeatherKind::Rain, 0.2f},
                {WeatherKind::Storm, 0.0f}, {WeatherKind::Snow, 0.4f}};
    for (const auto& r : rows) {
        const Weather full = wx(r.kind, 1.0f, 0.0f);
        CHECK(mist_level(m, 6.0f, &full) == doctest::Approx(r.factor));
        const Weather none = wx(r.kind, 0.0f, 0.0f);  // intensity 0: no weather yet
        CHECK(mist_level(m, 6.0f, &none) == 1.0f);
        const Weather half = wx(r.kind, 0.5f, 0.0f);
        CHECK(mist_level(m, 6.0f, &half) == doctest::Approx(1.0f + (r.factor - 1.0f) * 0.5f));
        GroundMist undamped = m;
        undamped.weather_damp = 0.0f;
        CHECK(mist_level(undamped, 6.0f, &full) == 1.0f);
        GroundMist partial = m;
        partial.weather_damp = 0.5f;
        CHECK(mist_level(partial, 6.0f, &full) == doctest::Approx(1.0f + (r.factor - 1.0f) * 0.5f));
    }
    const Weather storm = wx(WeatherKind::Storm, 1.0f, 1.0f);
    CHECK(same_bits(mist_level(m, 6.0f, &storm), 0.0f));
}

TEST_CASE("ground_mist: no Weather means wetness 0 and no damping") {
    GroundMist m;
    CHECK(mist_level(m, 6.0f, nullptr) == doctest::Approx(0.25f));
    const Weather dry = wx(WeatherKind::Clear, 0.0f, 0.0f);
    CHECK(same_bits(mist_level(m, 6.0f, nullptr), mist_level(m, 6.0f, &dry)));
    m.base_humidity = 1.0f;
    CHECK(mist_level(m, 6.0f, nullptr) == 1.0f);
}

TEST_CASE("ground_mist: time_of_day -1 and other bad hours") {
    const GroundMist m = wet_mist();
    const Weather wet = wx(WeatherKind::Clear, 0.0f, 1.0f);
    CHECK(same_bits(mist_level(m, -1.0f, &wet), 0.0f));
    CHECK(same_bits(mist_level(m, -0.001f, &wet), 0.0f));
    CHECK(same_bits(mist_level(m, kNaN, &wet), 0.0f));
    CHECK(same_bits(mist_level(m, -std::numeric_limits<f32>::infinity(), &wet), 0.0f));
    CHECK(mist_level(m, 30.0f, &wet) == doctest::Approx(mist_level(m, 6.0f, &wet)));  // above 24 wraps
    CHECK(mist_level(m, std::numeric_limits<f32>::infinity(), &wet) >= 0.0f);          // no crash, in range
    CHECK(mist_level(m, std::numeric_limits<f32>::infinity(), &wet) <= 1.0f);
}

TEST_CASE("ground_mist: NaN, out-of-range and zero ranges fall back or clamp") {
    const Weather wet = wx(WeatherKind::Clear, 0.0f, 1.0f);
    const GroundMist d;
    const f32 ref = mist_level(d, 5.0f, &wet);
    GroundMist n;
    n.strength = kNaN;
    n.peak_hour = kNaN;
    n.rise_hours = kNaN;
    n.burn_hours = kNaN;
    n.base_humidity = kNaN;
    n.wet_gain = kNaN;
    n.weather_damp = kNaN;
    CHECK(same_bits(mist_level(n, 5.0f, &wet), ref));  // every NaN means the default
    CHECK(mist_level(n, 5.0f, nullptr) == mist_level(d, 5.0f, nullptr));

    GroundMist z;  // zero widths clamp to 0.1 h: no division by zero, still 1 at the peak, 0 a little away
    z.rise_hours = 0.0f;
    z.burn_hours = 0.0f;
    CHECK(mist_level(z, 6.0f, &wet) == 1.0f);
    CHECK(same_bits(mist_level(z, 6.2f, &wet), 0.0f));
    CHECK(same_bits(mist_level(z, 5.8f, &wet), 0.0f));
    GroundMist neg = z;
    neg.rise_hours = -4.0f;
    neg.burn_hours = -4.0f;
    CHECK(same_bits(mist_level(neg, 6.2f, &wet), mist_level(z, 6.2f, &wet)));

    GroundMist big;  // widths above 12 h clamp to 12
    big.rise_hours = 1000.0f;
    big.burn_hours = 1000.0f;
    GroundMist twelve;
    twelve.rise_hours = 12.0f;
    twelve.burn_hours = 12.0f;
    CHECK(same_bits(mist_level(big, 9.0f, &wet), mist_level(twelve, 9.0f, &wet)));

    GroundMist hi;  // peak_hour out of the range clamps
    hi.peak_hour = 99.0f;
    GroundMist top;
    top.peak_hour = 24.0f;
    CHECK(same_bits(mist_level(hi, 23.0f, &wet), mist_level(top, 23.0f, &wet)));
    GroundMist s;
    s.strength = 100.0f;
    GroundMist s2;
    s2.strength = 2.0f;
    CHECK(same_bits(mist_level(s, 5.0f, &wet), mist_level(s2, 5.0f, &wet)));
    const Weather bad = wx(WeatherKind::Rain, kNaN, kNaN);  // NaN intensity and wetness count as 0
    CHECK(same_bits(mist_level(d, 5.0f, &bad), mist_level(d, 5.0f, nullptr)));
}

TEST_CASE("ground_mist: the level is monotone in strength and stays in 0..1") {
    const Weather wet = wx(WeatherKind::Overcast, 0.5f, 0.6f);
    for (f32 hour : {3.5f, 5.0f, 6.0f, 8.0f}) {
        f32 prev = 0.0f;
        for (f32 s = 0.0f; s <= 2.0f; s += 0.125f) {
            GroundMist m;
            m.strength = s;
            const f32 l = mist_level(m, hour, &wet);
            CHECK(l >= prev);
            CHECK(l <= 1.0f);
            prev = l;
        }
    }
    GroundMist off;
    off.strength = 0.0f;
    CHECK(same_bits(mist_level(off, 6.0f, &wet), 0.0f));
    GroundMist hot;
    hot.strength = 2.0f;
    hot.base_humidity = 1.0f;
    CHECK(mist_level(hot, 6.0f, nullptr) == 1.0f);  // clamped
}

TEST_CASE("ground_mist: a zero level is exactly 0.0f") {
    const Weather wet = wx(WeatherKind::Clear, 0.0f, 1.0f);
    const GroundMist m;
    for (f32 hour : {0.0f, 1.0f, 12.0f, 18.0f, 23.99f, -1.0f}) {
        const f32 l = mist_level(m, hour, &wet);
        CHECK(l == 0.0f);
        CHECK(same_bits(l, 0.0f));  // +0, not -0
    }
    const Weather storm = wx(WeatherKind::Storm, 1.0f, 1.0f);
    CHECK(same_bits(mist_level(m, 6.0f, &storm), 0.0f));
    GroundMist none;
    none.strength = 0.0f;
    CHECK(same_bits(mist_level(none, 6.0f, &wet), 0.0f));
}

namespace {

struct Rig {
    World w;
    u64 t = 0;
    Entity wxe, me, ce;
    explicit Rig(bool with_mist, bool with_clock = false, f32 hour = 4.0f) {
        w.register_components<SaveKey>();
        install_weather(w, nullptr);
        if (Physics3D* p = w.try_resource<Physics3D>()) p->auto_step = 0;
        Weather c;
        c.target = WeatherKind::Rain;
        c.transition_seconds = 0.5f;
        wxe = w.spawn_with(SaveKey{1}, c);
        if (with_mist) {
            GroundMist m;
            m.peak_hour = 6.0f;
            m.density = 0.1f;
            m.color = {0.1f, 0.2f, 0.3f};
            m.enabled = 0;
            me = w.spawn_with(SaveKey{2}, m);
        }
        if (with_clock) {
            WorldClock k;
            k.hour = hour;
            k.day_length_seconds = 60.0f;  // 24 h in 3600 ticks
            ce = w.spawn_with(SaveKey{3}, k);
        }
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    f32 level() {
        const Environment3D* env = w.try_resource<Environment3D>();
        const GroundMist* m = w.try_get<GroundMist>(me);
        const Weather* wc = w.try_get<Weather>(wxe);
        return mist_level(*m, env->time_of_day, wc);
    }
};

}  // namespace

TEST_CASE("ground_mist: the component round-trips save and load") {
    Array<byte> bytes;
    u64 hash_a = 0;
    {
        Rig a(true);
        GroundMist& m = *a.w.try_get_mut<GroundMist>(a.me);
        m.strength = 1.5f;
        m.peak_hour = 7.25f;
        m.rise_hours = 2.0f;
        m.burn_hours = 4.5f;
        m.base_humidity = 0.4f;
        m.wet_gain = 0.6f;
        m.weather_damp = 0.3f;
        m.density = 0.2f;
        m.thickness_m = 12.0f;
        m.base_height_m = -3.0f;
        m.exp2_density = 0.05f;
        m.color = {0.5f, 0.6f, 0.7f};
        m.enabled = 1;
        a.run(30);
        save::write_save(a.w, {}, bytes);
        hash_a = a.w.state_hash();
    }
    Rig b(true);
    b.run(5);
    REQUIRE(save::read_save(b.w, bytes.span()));
    const GroundMist& m = *b.w.try_get<GroundMist>(b.me);
    CHECK(m.strength == 1.5f);
    CHECK(m.peak_hour == 7.25f);
    CHECK(m.rise_hours == 2.0f);
    CHECK(m.burn_hours == 4.5f);
    CHECK(m.base_humidity == 0.4f);
    CHECK(m.wet_gain == 0.6f);
    CHECK(m.weather_damp == 0.3f);
    CHECK(m.density == 0.2f);
    CHECK(m.thickness_m == 12.0f);
    CHECK(m.base_height_m == -3.0f);
    CHECK(m.exp2_density == 0.05f);
    CHECK(m.color.x == 0.5f);
    CHECK(m.color.y == 0.6f);
    CHECK(m.color.z == 0.7f);
    CHECK(m.enabled == 1);
    CHECK(b.w.state_hash() == hash_a);
}

TEST_CASE("ground_mist: the Weather column is the same with and without a GroundMist") {
    Rig a(false), b(true);
    for (u32 i = 0; i < 300; ++i) {
        a.run(1);
        b.run(1);
        const Weather& wa = *a.w.try_get<Weather>(a.wxe);
        const Weather& wb = *b.w.try_get<Weather>(b.wxe);
        // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison): both copies come from the same writer, padding included
        CHECK(std::memcmp(&wa, &wb, sizeof(Weather)) == 0);
    }
    // the mist code never writes the component either: it is still as authored
    const GroundMist& m = *b.w.try_get<GroundMist>(b.me);
    CHECK(m.strength == 1.0f);
    CHECK(m.enabled == 0);
    // a scene without one has one more column in the hash only through the component itself
    Rig c(false), d(false);
    c.run(300);
    d.run(300);
    CHECK(c.w.state_hash() == d.w.state_hash());
}

TEST_CASE("ground_mist: the level at fixed ticks of a WorldClock run is the same in one go and in two halves") {
    constexpr u32 kN = 1800;
    constexpr u32 kEvery = 100;
    std::vector<f32> whole;
    {
        Rig a(true, true, 3.0f);
        for (u32 i = 0; i < kN; ++i) {
            a.run(1);
            if ((i + 1) % kEvery == 0) whole.push_back(a.level());
        }
    }
    CHECK(whole.size() == kN / kEvery);
    bool rises = false, falls = false;
    for (usize i = 1; i < whole.size(); ++i) {
        rises = rises || whole[i] > whole[i - 1];
        falls = falls || whole[i] < whole[i - 1];
    }
    CHECK(rises);
    CHECK(falls);

    std::vector<f32> halves;
    Array<byte> bytes;
    {
        Rig a(true, true, 3.0f);
        for (u32 i = 0; i < kN / 2; ++i) {
            a.run(1);
            if ((i + 1) % kEvery == 0) halves.push_back(a.level());
        }
        save::write_save(a.w, {}, bytes);
    }
    Rig b(true, true, 3.0f);
    b.run(11);  // another state, replaced by the save
    REQUIRE(save::read_save(b.w, bytes.span()));
    b.t = kN / 2;
    for (u32 i = kN / 2; i < kN; ++i) {
        b.run(1);
        if ((i + 1) % kEvery == 0) halves.push_back(b.level());
    }
    REQUIRE(halves.size() == whole.size());
    for (usize i = 0; i < whole.size(); ++i) CHECK(same_bits(whole[i], halves[i]));
}
