#include "mce/kit/weather.h"

#include <cmath>

#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/timed_light.h"

namespace mce::kit {
namespace {

struct WeatherInstalled {};

constexpr u32 kWetMax = 1u << 24;
constexpr f64 kWetSeconds = 60.0;  // fully wet after this long at full intensity
constexpr u32 kKinds = 5;
constexpr u32 kWindMicro[kKinds] = {100000, 300000, 500000, 1000000, 200000};  // clear, overcast, rain, storm, snow

u64 mul_div(u64 a, u64 b, u64 c) { return (a / c) * b + (a % c) * b / c; }

// An integer mix (splitmix-style) of the seed and the pick counter.
u32 hash_pick(u32 seed, u32 counter) {
    u64 z = (static_cast<u64>(seed) << 32 | counter) + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<u32>(z >> 32);
}

u64 ticks_of(f64 seconds, f64 dt) {
    if (!(dt > 0.0) || !(seconds > 0.0)) return 1;
    const u64 t = static_cast<u64>(std::llround(seconds / dt));
    return t < 1 ? 1 : t;
}

bool wets(WeatherKind k) { return k == WeatherKind::Rain || k == WeatherKind::Storm || k == WeatherKind::Snow; }

void tick_weather(Frame& f, Weather& c) {
    const f64 dt = static_cast<f64>(f.dt);
    const u32 den = static_cast<u32>(ticks_of(static_cast<f64>(c.transition_seconds), dt));
    if (c.fade_den == 0) {
        c.fade = 0;
    } else if (c.fade_den != den) {  // the transition length changed: keep the ratio
        c.fade = static_cast<u32>(mul_div(c.fade, den, c.fade_den));
    }
    c.fade_den = den;
    if (c.fade > den) c.fade = den;
    if (static_cast<u32>(c.target) >= kKinds) c.target = WeatherKind::Clear;
    if (static_cast<u32>(c.current) >= kKinds) c.current = WeatherKind::Clear;

    if (c.auto_change_seconds > 0.0f) {  // the seeded schedule
        if (++c.timer >= ticks_of(static_cast<f64>(c.auto_change_seconds), dt)) {
            c.timer = 0;
            c.target = static_cast<WeatherKind>(hash_pick(c.seed, c.counter) % kKinds);
            ++c.counter;
        }
    } else {
        c.timer = 0;
    }

    if (c.current != c.target) {  // fade the old kind out, then switch
        if (c.fade > 0) --c.fade;
        if (c.fade == 0) {
            f.events().push(WeatherChanged{static_cast<u32>(c.current), static_cast<u32>(c.target)});
            c.current = c.target;
        }
    }
    if (c.current == c.target && c.current != WeatherKind::Clear && c.fade < den) ++c.fade;  // fade in

    // Wetness: rise with the intensity under rain, storm and snow, else dry.
    if (wets(c.current)) {
        const u64 rise = mul_div(static_cast<u64>(kWetMax) * c.fade, 1, static_cast<u64>(den) * ticks_of(kWetSeconds, dt));
        const u64 w = c.wet_q + rise;
        c.wet_q = w > kWetMax ? kWetMax : static_cast<u32>(w);
    } else {
        const u64 fall = c.dry_seconds > 0.0f ? kWetMax / ticks_of(static_cast<f64>(c.dry_seconds), dt) : kWetMax;
        c.wet_q = c.wet_q > fall ? static_cast<u32>(c.wet_q - fall) : 0;
    }

    const u64 micro = 100000 + mul_div(kWindMicro[static_cast<u32>(c.current)] - 100000, c.fade, den);
    c.intensity = static_cast<f32>(static_cast<f64>(c.fade) / static_cast<f64>(den));
    c.wind = static_cast<f32>(static_cast<f64>(micro) / 1e6);
    c.wetness = static_cast<f32>(static_cast<f64>(c.wet_q) / static_cast<f64>(kWetMax));
}

void kit_weather(Frame& f) {
    bool first = true;  // one Weather per scene: the first in ECS order drives
    f.query<Weather>().each([&](Entity, Weather& c) {
        if (!first) return;
        first = false;
        tick_weather(f, c);
    });
}

}  // namespace

void install_weather(World& world, asset::AssetSystem* assets) {
    install_timed_light(world, assets);  // it adds kit_timed_light, which this system follows
    if (world.try_resource<WeatherInstalled>()) return;
    world.set_resource(WeatherInstalled{});
    world.register_components<Weather>();
    world.register_event<WeatherChanged>(Phase::FixedUpdate_Pre);
    world.add_system(Phase::FixedUpdate_Pre, "kit_weather", &kit_weather, "kit_timed_light");
}

}  // namespace mce::kit
