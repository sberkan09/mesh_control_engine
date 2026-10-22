// WindGust in the two system consumers (M-WINDGUST 12c): kit_precipitation (rain and snow slant) and kit_weather_audio
// (wind gain target) read effective_wind. Both run after kit_wind_gust by registration order (World::add_system order IS
// execution order), so in tick T they see the level the gust produced in tick T. Without a WindGust, or with the bit
// clear, both are bit-identical to the old Weather.wind code; a gust that applies feeds rain a wind quantised to 1/64.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <new>
#include <string_view>

#include "mce/audio/components.h"
#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/precipitation.h"
#include "mce/kit/weather.h"
#include "mce/kit/weather_audio.h"
#include "mce/kit/wind_gust.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr u32 kMax = 1u << 24;

u32 bits(f32 x) { return std::bit_cast<u32>(x); }
u32 q_of(f64 v) { return static_cast<u32>(std::llround(v * static_cast<f64>(kMax))); }

ParticleEmitter3D clean_emitter() {
    alignas(ParticleEmitter3D) unsigned char buf[sizeof(ParticleEmitter3D)];
    std::memset(buf, 0, sizeof buf);
    return *new (buf) ParticleEmitter3D{};
}

struct Rig {
    World w;
    u64 t = 0;
    Entity wx, pe, wa, rain, wind, gust;
    Rig(bool with_gust, u32 affects = kGustAll, WeatherKind kind = WeatherKind::Rain, f32 fade_s = 0.0f) {
        w.register_components<SaveKey>();
        w.register_components<audio::AudioSource>();
        install_precipitation(w, nullptr);
        w.register_event<audio::PlaySfx>(Phase::FixedUpdate_Pre);
        w.set_resource(Environment3D{});
        rain = w.spawn_with(SaveKey{10}, audio::AudioSource{});
        wind = w.spawn_with(SaveKey{11}, audio::AudioSource{});
        Weather c;
        c.target = kind;
        c.transition_seconds = 0.05f;
        wx = w.spawn_with(SaveKey{1}, c);
        pe = w.spawn_with(Precipitation3D{}, clean_emitter(), LocalTransform{});
        WeatherAudio a;
        a.rain_source = rain;
        a.wind_source = wind;
        a.fade_seconds = fade_s;
        wa = w.spawn_with(SaveKey{2}, a);
        if (with_gust) {
            WindGust g;
            g.seed = 5;
            g.min_gap_s = 0.2f;
            g.max_gap_s = 0.5f;
            g.affects = affects;
            gust = w.spawn_with(SaveKey{3}, g);
        }
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    ParticleEmitter3D& em() { return *w.try_get_mut<ParticleEmitter3D>(pe); }
    const Weather& weather() { return *w.try_get<Weather>(wx); }
    WeatherAudio& audio() { return *w.try_get_mut<WeatherAudio>(wa); }
    const WindGust& g() { return *w.try_get<WindGust>(gust); }
    f32 wind_db() { return w.try_get<audio::AudioSource>(wind)->gain_db; }
};

f32 quantised(f32 v) { return std::round(v * 64.0f) / 64.0f; }

}  // namespace

TEST_CASE("wind gust systems: kit_wind_gust is registered before both consumers") {
    Rig r(true);
    const Span<const char* const> names = r.w.system_names(Phase::FixedUpdate_Pre);
    int gust = -1, precip = -1, audio = -1;
    for (usize i = 0; i < names.size(); ++i) {
        const std::string_view n = names[i];
        if (n == "kit_wind_gust") gust = static_cast<int>(i);
        if (n == "kit_precipitation") precip = static_cast<int>(i);
        if (n == "kit_weather_audio") audio = static_cast<int>(i);
    }
    REQUIRE(gust >= 0);
    CHECK(gust < precip);
    CHECK(gust < audio);
}

TEST_CASE("wind gust systems: the consumers see the level of the same tick") {
    Rig r(true);
    u32 active = 0;
    for (u32 i = 0; i < 900; ++i) {
        r.run(1);
        const f32 eff = effective_wind(r.weather(), &r.g(), kGustRain);
        const f32 wind = bits(eff) != bits(r.weather().wind) ? quantised(eff) : eff;
        const f32 x = 0.5f * wind;
        CHECK(bits(r.em().direction.x) == bits(x * (1.0f / std::sqrt(x * x + 1.0f))));
        CHECK(bits(r.em().gravity.x) == bits(3.0f * wind));
        CHECK(r.audio().wind_q == q_of(static_cast<f64>(effective_wind(r.weather(), &r.g(), kGustAudio))));
        if (r.g().gust_q != 0 && i > 10) ++active;
    }
    CHECK(active > 100);
}

TEST_CASE("wind gust systems: a gust moves the slant and the wind gain, and follows the helper") {
    Rig plain(false), gusty(true);
    f32 max_dx = 0, max_dg = 0;
    for (u32 i = 0; i < 900; ++i) {
        plain.run(1);
        gusty.run(1);
        max_dx = std::max(max_dx, gusty.em().gravity.x - plain.em().gravity.x);
        max_dg = std::max(max_dg, gusty.wind_db() - plain.wind_db());
        CHECK(gusty.em().gravity.x >= plain.em().gravity.x);
        CHECK(gusty.wind_db() >= plain.wind_db());
    }
    CHECK(max_dx > 0.1f);
    CHECK(max_dg > 0.5f);
}

TEST_CASE("wind gust systems: without a WindGust the emitter and the gains are the old Weather.wind code bit for bit") {
    for (const bool snow : {false, true}) {
        Rig r(false, kGustAll, snow ? WeatherKind::Snow : WeatherKind::Rain);
        r.run(120);
        const f32 wind = r.weather().wind;
        const f32 x = (snow ? 0.8f : 0.5f) * wind;
        CHECK(bits(r.em().direction.x) == bits(x * (1.0f / std::sqrt(x * x + 1.0f))));
        CHECK(bits(r.em().gravity.x) == bits((snow ? 2.0f : 3.0f) * wind));
        CHECK(r.audio().wind_q == q_of(static_cast<f64>(wind)));
    }
}

TEST_CASE("wind gust systems: a cleared bit leaves that consumer unchanged") {
    Rig base(false);
    Rig no_rain(true, kGustAll & ~kGustRain), no_audio(true, kGustAll & ~kGustAudio), none(true, 0u);
    for (u32 i = 0; i < 600; ++i) {
        base.run(1);
        no_rain.run(1);
        no_audio.run(1);
        none.run(1);
        CHECK(bits(no_rain.em().gravity.x) == bits(base.em().gravity.x));
        CHECK(bits(no_rain.em().direction.x) == bits(base.em().direction.x));
        CHECK(no_audio.audio().wind_q == base.audio().wind_q);
        CHECK(bits(none.em().gravity.x) == bits(base.em().gravity.x));
        CHECK(none.audio().wind_q == base.audio().wind_q);
    }
}

TEST_CASE("wind gust systems: the rain wind steps in 1/64 while a gust applies") {
    Rig r(true);
    for (u32 i = 0; i < 600; ++i) {
        r.run(1);
        if (r.g().gust_q == 0) continue;
        const f32 gz = r.em().gravity.x / 3.0f;
        const f32 steps = gz * 64.0f;
        CHECK(std::fabs(steps - std::round(steps)) < 1e-3f);
    }
}

TEST_CASE("wind gust systems: the Weather column hash is the same with and without a WindGust") {
    Rig a(false), b(true);
    a.run(400);
    b.run(400);
    CHECK(bits(a.weather().wind) == bits(b.weather().wind));
    CHECK(bits(a.weather().intensity) == bits(b.weather().intensity));
    CHECK(bits(a.weather().wetness) == bits(b.weather().wetness));
    CHECK(a.weather().fade == b.weather().fade);
    CHECK(a.weather().timer == b.weather().timer);
    CHECK(a.weather().wet_q == b.weather().wet_q);
    CHECK(a.weather().current == b.weather().current);
}

TEST_CASE("wind gust systems: save and load mid-gust, N ticks after equal N ticks without, with the wind gain fade") {
    Array<byte> bytes;
    u64 hash_a = 0, end_a = 0;
    u32 wind_q_a = 0;
    {
        Rig a(true, kGustAll, WeatherKind::Rain, 2.0f);
        a.run(60);
        while (a.g().gust_q == 0 || a.g().gust_q > kMax / 2) a.run(1);  // in the rise, the wind bed is mid-ramp
        wind_q_a = a.audio().wind_q;
        save::write_save(a.w, {}, bytes);
        const u64 t0 = a.t;
        a.run(1);
        hash_a = a.w.state_hash();
        a.run(200);
        end_a = a.w.state_hash();
        Rig b(true, kGustAll, WeatherKind::Rain, 2.0f);
        b.run(31);
        REQUIRE(save::read_save(b.w, bytes.span()));
        CHECK(b.audio().wind_q == wind_q_a);
        b.t = t0;
        b.run(1);
        CHECK(b.w.state_hash() == hash_a);
        b.run(200);
        CHECK(b.w.state_hash() == end_a);
    }
}
