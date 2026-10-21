// WeatherAudio (M-WEATHERAUDIO 10a): the system kit_weather_audio drives AudioSource::gain_db of two named entities
// from the Weather. Levels follow kind, intensity and wind; the fade is an integer ramp; authoring is clamped and NaN
// safe; null, dead and non-source entities are skipped; a world without audio resources, without Weather or with
// enabled 0 is not touched; save/load mid-fade resumes; the Weather bytes are never written.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <limits>

#include "mce/audio/components.h"
#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/world.h"
#include "mce/kit/weather.h"
#include "mce/kit/weather_audio.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr u32 kMax = 1u << 24;

struct Rig {
    World w;
    u64 t = 0;
    Entity wx, wa, rain, wind;
    explicit Rig(WeatherKind kind = WeatherKind::Storm, bool with_wa = true, bool with_wx = true, bool audio = true,
                 f32 fade_s = 0.0f, bool enabled = true) {
        w.register_components<SaveKey>();
        w.register_components<audio::AudioSource>();
        install_weather(w, nullptr);
        if (audio) w.register_event<audio::PlaySfx>(Phase::FixedUpdate_Pre);
        rain = w.spawn_with(SaveKey{10}, audio::AudioSource{});
        wind = w.spawn_with(SaveKey{11}, audio::AudioSource{});
        if (with_wx) {
            Weather c;
            c.target = kind;
            c.transition_seconds = 0.05f;  // 3 ticks
            wx = w.spawn_with(SaveKey{1}, c);
        }
        if (with_wa) {
            WeatherAudio c;
            c.rain_source = rain;
            c.wind_source = wind;
            c.fade_seconds = fade_s;
            c.enabled = enabled ? 1 : 0;
            wa = w.spawn_with(SaveKey{2}, c);
        }
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    WeatherAudio& c() { return *w.try_get_mut<WeatherAudio>(wa); }
    Weather& weather() { return *w.try_get_mut<Weather>(wx); }
    f32 rain_db() { return w.try_get<audio::AudioSource>(rain)->gain_db; }
    f32 wind_db() { return w.try_get<audio::AudioSource>(wind)->gain_db; }
};

u32 q_of(f64 v) { return static_cast<u32>(std::llround(v * static_cast<f64>(kMax))); }

}  // namespace

TEST_CASE("weather_audio: defaults") {
    const WeatherAudio c;
    CHECK(c.rain_source.is_null());
    CHECK(c.wind_source.is_null());
    CHECK(c.rain_db == 0.0f);
    CHECK(c.wind_db == 0.0f);
    CHECK(c.fade_seconds == 3.0f);
    CHECK(c.enabled == 1);
    CHECK(c.rain_q == 0);
    CHECK(c.wind_q == 0);
    for (u8 b : c.pad_) CHECK(b == 0);
}

TEST_CASE("weather_audio: levels follow the kind, the intensity and the wind") {
    struct Row {
        WeatherKind kind;
        f64 weight;
    };
    const Row rows[] = {{WeatherKind::Clear, 0.0},  {WeatherKind::Overcast, 0.15}, {WeatherKind::Rain, 1.0},
                        {WeatherKind::Storm, 1.0}, {WeatherKind::Snow, 0.35}};
    for (const Row& r : rows) {
        CAPTURE(static_cast<int>(r.kind));
        Rig g(r.kind);
        g.run(40);
        const Weather& wx = g.weather();
        CHECK(wx.current == r.kind);
        const f64 intensity = static_cast<f64>(wx.intensity);
        CHECK(g.c().rain_q == q_of(r.weight * intensity));
        CHECK(g.c().wind_q == q_of(static_cast<f64>(wx.wind)));
        const f32 expect_rain = g.c().rain_q == 0 ? -60.0f : static_cast<f32>(20.0 * std::log10(r.weight * intensity));
        CHECK(g.rain_db() == doctest::Approx(expect_rain).epsilon(1e-4));
        CHECK(g.wind_db() == doctest::Approx(static_cast<f32>(20.0 * std::log10(static_cast<f64>(wx.wind)))).epsilon(1e-4));
    }
    {  // a full storm is exactly full level: gain = max_db
        Rig g(WeatherKind::Storm);
        g.c().rain_db = -6.0f;
        g.c().wind_db = 3.0f;
        g.run(40);
        CHECK(g.c().rain_q == kMax);
        CHECK(g.c().wind_q == kMax);
        CHECK(g.rain_db() == -6.0f);
        CHECK(g.wind_db() == 3.0f);
    }
    {  // clear: rain is silent (-60 exactly), the calm wind of 0.1 is a quiet bed
        Rig g(WeatherKind::Clear);
        g.c().rain_db = 12.0f;
        g.run(10);
        CHECK(g.c().rain_q == 0);
        CHECK(g.rain_db() == -60.0f);
        CHECK(g.c().wind_q == q_of(static_cast<f64>(g.weather().wind)));
        CHECK(g.wind_db() == doctest::Approx(-20.0f).epsilon(1e-3));
    }
    {  // mid-transition: the intensity scales the rain
        Rig g(WeatherKind::Rain);
        g.weather().transition_seconds = 1.0f;  // 60 ticks
        g.run(30);
        CHECK(g.weather().intensity > 0.0f);
        CHECK(g.weather().intensity < 1.0f);
        CHECK(g.c().rain_q == q_of(static_cast<f64>(g.weather().intensity)));
    }
}

TEST_CASE("weather_audio: the fade is a ramp of 2^24 / ticks per tick, then stops on the target") {
    Rig g(WeatherKind::Storm, true, true, true, 1.0f, false);  // disabled until the storm is full
    g.run(40);
    REQUIRE(g.weather().intensity == 1.0f);
    CHECK(g.c().rain_q == 0);
    CHECK(g.rain_db() == 0.0f);  // untouched while disabled
    g.c().enabled = 1;
    const u32 step = kMax / 60;
    f32 prev = -61.0f;
    for (u32 k = 1; k <= 60; ++k) {
        g.run(1);
        CAPTURE(k);
        CHECK(g.c().rain_q == k * step);
        CHECK(g.c().wind_q == k * step);
        CHECK(g.rain_db() > prev);
        prev = g.rain_db();
    }
    g.run(1);  // 60 * step is a little short of 2^24: the next tick lands exactly on it
    CHECK(g.c().rain_q == kMax);
    CHECK(g.rain_db() == 0.0f);
    g.run(20);
    CHECK(g.c().rain_q == kMax);
    // And back down to silence when the weather clears.
    g.weather().target = WeatherKind::Clear;
    g.weather().transition_seconds = 0.05f;
    g.run(10);
    CHECK(g.c().rain_q < kMax);
    g.run(200);
    CHECK(g.c().rain_q == 0);
    CHECK(g.rain_db() == -60.0f);
}

TEST_CASE("weather_audio: authoring is clamped and NaN safe, and not rewritten") {
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();
    {  // NaN gains are 0 dB, out-of-range ones clamp to -60..12
        Rig g(WeatherKind::Storm);
        g.c().rain_db = nan;
        g.c().wind_db = 100.0f;
        g.run(40);
        CHECK(g.rain_db() == 0.0f);
        CHECK(g.wind_db() == 12.0f);
        CHECK(std::isnan(g.c().rain_db));
        CHECK(g.c().wind_db == 100.0f);
        g.c().rain_db = -100.0f;
        g.run(1);
        CHECK(g.rain_db() == -60.0f);
        g.c().rain_db = -inf;
        g.run(1);
        CHECK(g.rain_db() == -60.0f);
    }
    {  // a very quiet authored gain floors at -60 for a small level
        Rig g(WeatherKind::Overcast);
        g.c().rain_db = -55.0f;
        g.run(40);
        CHECK(g.rain_db() == -60.0f);
    }
    {  // fade: NaN = 3 s (180 ticks), negative = instant, huge and +inf = 60 s (3600 ticks)
        const struct {
            f32 fade;
            u32 step;
        } rows[] = {{nan, kMax / 180}, {-5.0f, kMax}, {0.0f, kMax}, {1e9f, kMax / 3600}, {inf, kMax / 3600}};
        for (const auto& r : rows) {
            CAPTURE(r.fade);
            Rig g(WeatherKind::Storm, true, true, true, 3.0f, false);
            g.run(40);
            g.c().fade_seconds = r.fade;
            g.c().enabled = 1;
            g.run(1);
            CHECK(g.c().rain_q == r.step);
            CHECK(std::isnan(g.c().fade_seconds) == std::isnan(r.fade));
        }
    }
    {  // a hand-edited level past 2^24 is pulled back
        Rig g(WeatherKind::Storm);
        g.run(40);
        g.c().rain_q = 0xFFFFFFFFu;
        g.run(1);
        CHECK(g.c().rain_q == kMax);
    }
}

TEST_CASE("weather_audio: null, dead and non-source entities are skipped") {
    Rig g(WeatherKind::Storm);
    g.c().rain_source = Entity{};
    g.w.destroy(g.wind);
    g.run(40);
    CHECK(g.c().rain_q == kMax);  // the levels still advance
    CHECK(g.c().wind_q == kMax);
    {  // an entity with no AudioSource
        Rig h(WeatherKind::Storm);
        const Entity bare = h.w.spawn_with(SaveKey{20}, audio::AudioSource{});
        h.w.remove<audio::AudioSource>(bare);
        h.c().rain_source = bare;
        h.run(40);
        CHECK(h.c().rain_q == kMax);
        CHECK(h.w.try_get<audio::AudioSource>(bare) == nullptr);
        CHECK(h.wind_db() == 0.0f);
    }
    {  // a stale handle whose slot was reused
        Rig h(WeatherKind::Storm);
        h.c().wind_db = -10.0f;
        h.c().rain_source = h.rain;
        h.w.destroy(h.rain);
        h.run(40);
        CHECK(h.wind_db() == -10.0f);
    }
}

TEST_CASE("weather_audio: only gain_db is written; sources are never started or stopped") {
    Rig g(WeatherKind::Storm);
    g.w.try_get_mut<audio::AudioSource>(g.rain)->loop = true;
    g.w.try_get_mut<audio::AudioSource>(g.rain)->play_on_start = true;
    const audio::AudioSource before = *g.w.try_get<audio::AudioSource>(g.rain);
    g.c().rain_db = -9.0f;
    g.run(40);
    audio::AudioSource after = *g.w.try_get<audio::AudioSource>(g.rain);
    CHECK(after.gain_db == -9.0f);
    CHECK(after.started == before.started);
    CHECK(after.loop == before.loop);
    CHECK(after.play_on_start == before.play_on_start);
    CHECK(after.pan == before.pan);
    CHECK(after.applied_gain_db == before.applied_gain_db);
}

TEST_CASE("weather_audio: no Weather, no WeatherAudio, enabled 0 or no audio resources = nothing written") {
    auto untouched = [](Rig& g) {
        g.run(40);
        CHECK(g.rain_db() == 0.0f);
        CHECK(g.wind_db() == 0.0f);
    };
    {
        Rig g(WeatherKind::Storm, true, false);  // no Weather
        untouched(g);
        CHECK(g.c().rain_q == 0);
        CHECK(g.c().wind_q == 0);
    }
    {
        Rig g(WeatherKind::Storm, false);  // no WeatherAudio
        untouched(g);
    }
    {
        Rig g(WeatherKind::Storm, true, true, true, 0.0f, false);  // disabled
        untouched(g);
        CHECK(g.c().rain_q == 0);
    }
    {
        Rig g(WeatherKind::Storm, true, true, false);  // no audio resources: levels do not advance either
        untouched(g);
        CHECK(g.c().rain_q == 0);
        CHECK(g.c().wind_q == 0);
        g.w.set_resource(audio::AudioRef{});  // an AudioRef alone counts as audio (a null device is fine)
        g.run(40);
        CHECK(g.c().rain_q == kMax);
        CHECK(g.rain_db() == 0.0f);
    }
}

TEST_CASE("weather_audio: a scene without a WeatherAudio is unchanged and the Weather bytes are never written") {
    Rig with(WeatherKind::Storm);
    Rig without(WeatherKind::Storm, false);
    for (u32 i = 0; i < 120; ++i) {
        with.run(1);
        without.run(1);
        const Weather a = with.weather();
        const Weather b = without.weather();
        REQUIRE(std::memcmp(&a, &b, sizeof(Weather)) == 0);  // NOLINT(bugprone-suspicious-memory-comparison): checks bit-exact bytes, as state_hash does
    }
    CHECK(without.rain_db() == 0.0f);
    CHECK(without.wind_db() == 0.0f);
    // The same scene twice hashes the same (deterministic), and the WeatherAudio column is the only difference.
    Rig again(WeatherKind::Storm, false);
    again.run(120);
    CHECK(again.w.state_hash() == without.w.state_hash());
    CHECK(with.w.state_hash() != without.w.state_hash());
}

TEST_CASE("weather_audio: save and load mid-fade resume, and N ticks after the load equal N ticks without it") {
    Array<byte> bytes;
    u64 hash_a = 0, end_a = 0;
    u32 rain_a = 0;
    f32 db_a = 0;
    {
        Rig a(WeatherKind::Storm, true, true, true, 2.0f);
        a.c().rain_db = -6.0f;
        a.run(50);  // the weather is full, the beds are partway up the 120-tick ramp
        REQUIRE(a.c().rain_q > 0);
        REQUIRE(a.c().rain_q < kMax);
        rain_a = a.c().rain_q;
        db_a = a.rain_db();
        save::write_save(a.w, {}, bytes);
        a.run(1);
        hash_a = a.w.state_hash();
        a.run(100);
        end_a = a.w.state_hash();
    }
    Rig b(WeatherKind::Storm, true, true, true, 2.0f);
    b.c().rain_db = -6.0f;
    b.run(77);  // another state, replaced by the save
    REQUIRE(save::read_save(b.w, bytes.span()));
    CHECK(b.c().rain_q == rain_a);
    b.t = 50;
    b.run(1);
    CHECK(b.w.state_hash() == hash_a);
    CHECK(b.rain_db() != db_a);  // the next gain is a pure function of the saved level and moved on
    b.run(100);
    CHECK(b.w.state_hash() == end_a);
}
