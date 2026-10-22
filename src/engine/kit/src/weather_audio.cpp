#include "mce/kit/weather_audio.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "mce/audio/components.h"
#include "mce/ecs/world.h"
#include "mce/kit/weather.h"
#include "mce/kit/wind_gust.h"

namespace mce::kit {
namespace {

constexpr u32 kLevelMax = 1u << 24;
constexpr u32 kKinds = 5;
constexpr f64 kRainWeight[kKinds] = {0.0, 0.15, 1.0, 1.0, 0.35};  // clear, overcast, rain, storm, snow
constexpr f32 kFloorDb = -60.0f;
constexpr u64 kMaxTicks = 1ull << 31;

f64 clamp_unit(f64 v) { return std::isnan(v) ? 0.0 : std::clamp(v, 0.0, 1.0); }
f32 clamp_db(f32 v) { return std::isnan(v) ? 0.0f : std::clamp(v, -60.0f, 12.0f); }

u32 target_q(f64 level) { return static_cast<u32>(std::llround(clamp_unit(level) * static_cast<f64>(kLevelMax))); }

u32 move_toward(u32 level, u32 target, u32 step) {
    if (level > kLevelMax) level = kLevelMax;  // a hand-edited level
    if (level < target) return target - level <= step ? target : level + step;
    return level - target <= step ? target : level - step;
}

f32 gain_of(u32 level, f32 max_db) {
    if (level == 0) return kFloorDb;
    const f64 db = static_cast<f64>(max_db) + 20.0 * std::log10(static_cast<f64>(level) / static_cast<f64>(kLevelMax));
    return db < static_cast<f64>(kFloorDb) ? kFloorDb : static_cast<f32>(db);
}

void drive(World& w, Entity e, f32 gain) {
    if (e.is_null() || !w.alive(e)) return;
    audio::AudioSource* s = w.try_get_mut<audio::AudioSource>(e);
    if (!s || std::bit_cast<u32>(s->gain_db) == std::bit_cast<u32>(gain)) return;
    s->gain_db = gain;
}

void kit_weather_audio(Frame& f) {
    World& w = f.world();
    WeatherAudio* wa = nullptr;
    w.query<WeatherAudio>().each([&](Entity, WeatherAudio& c) {
        if (!wa) wa = &c;
    });
    if (!wa || !wa->enabled) return;
    const Weather* weather = nullptr;
    w.query<const Weather>().each([&](Entity, const Weather& c) {
        if (!weather) weather = &c;
    });
    if (!weather) return;
    if (!w.event_queue<audio::PlaySfx>() && !w.try_resource<audio::AudioRef>()) return;  // no audio in this world

    const u32 kind = static_cast<u32>(weather->current);
    const f64 weight = kind < kKinds ? kRainWeight[kind] : 0.0;
    const u32 rain_target = target_q(weight * clamp_unit(static_cast<f64>(weather->intensity)));
    const WindGust* gust = nullptr;  // M-WINDGUST 12c: the first in ECS order; the fade ramp below smooths the gust
    w.query<const WindGust>().each([&](Entity, const WindGust& c) {
        if (!gust) gust = &c;
    });
    const u32 wind_target = target_q(clamp_unit(static_cast<f64>(effective_wind(*weather, gust, kGustAudio))));

    const f64 dt = static_cast<f64>(f.dt);
    const f64 fade = std::isnan(wa->fade_seconds) ? 3.0 : std::clamp(static_cast<f64>(wa->fade_seconds), 0.0, 60.0);
    u64 ticks = 1;
    if (dt > 0.0 && fade > 0.0) {
        const f64 x = fade / dt;
        ticks = x >= static_cast<f64>(kMaxTicks) ? kMaxTicks : std::max<u64>(1, static_cast<u64>(std::llround(x)));
    }
    const u32 step = static_cast<u32>(std::max<u64>(1, kLevelMax / ticks));

    wa->rain_q = move_toward(wa->rain_q, rain_target, step);
    wa->wind_q = move_toward(wa->wind_q, wind_target, step);
    drive(w, wa->rain_source, gain_of(wa->rain_q, clamp_db(wa->rain_db)));
    drive(w, wa->wind_source, gain_of(wa->wind_q, clamp_db(wa->wind_db)));
}

}  // namespace

void install_weather_audio(World& world) { world.add_system(Phase::FixedUpdate_Pre, "kit_weather_audio", &kit_weather_audio, "kit_weather"); }

}  // namespace mce::kit
