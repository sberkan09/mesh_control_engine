#include "mce/kit/weather.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include "mce/ecs/world.h"
#include "mce/kit/cloud_layer.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/rain_ripples.h"
#include "mce/kit/rain_splash.h"
#include "mce/kit/lightning_bolt.h"
#include "mce/kit/snow_cover.h"
#include "mce/kit/thunder_delay.h"
#include "mce/kit/timed_light.h"
#include "mce/kit/weather_audio.h"
#include "mce/kit/ground_mist.h"
#include "mce/kit/moon.h"
#include "mce/kit/wet_surfaces.h"
#include "mce/kit/wind_direction.h"
#include "mce/kit/wind_gust.h"
#include "mce/kit/wind_sway.h"

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

// The longest duration in ticks (2^31: it fits Weather.fade_den, a u32, and the products with it stay far inside u64).
constexpr u64 kMaxTicks = 1ull << 31;

u64 ticks_of(f64 seconds, f64 dt) {
    if (!(dt > 0.0) || !(seconds > 0.0)) return 1;
    const f64 x = seconds / dt;
    if (!(x < static_cast<f64>(kMaxTicks))) return kMaxTicks;  // +Inf, FLT_MAX, anything past the clamp
    const u64 t = static_cast<u64>(std::llround(x));
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

struct KindEnv {
    f32 sun_mul, sun_desat, amb_mul, amb_desat;
    f32 amb_tint[3];
    f32 exp_mul, fog_add;
    f32 fog_to[3];
    f32 fog_mix;
};
constexpr KindEnv kKindEnv[kKinds] = {
    {1.0f, 0.0f, 1.0f, 0.0f, {1, 1, 1}, 1.0f, 0.0f, {0, 0, 0}, 0.0f},                                    // clear
    {0.35f, 0.4f, 0.85f, 0.5f, {1, 1, 1}, 1.0f, 0.002f, {0.60f, 0.60f, 0.62f}, 0.4f},                    // overcast
    {0.2f, 0.5f, 0.7f, 0.6f, {1, 1, 1}, 1.1f, 0.006f, {0.50f, 0.52f, 0.55f}, 0.6f},                      // rain
    {0.08f, 0.5f, 0.5f, 0.5f, {0.85f, 0.9f, 1.05f}, 1.2f, 0.012f, {0.30f, 0.32f, 0.36f}, 0.7f},          // storm
    {0.4f, 0.3f, 1.05f, 0.3f, {0.95f, 1.0f, 1.08f}, 1.0f, 0.008f, {0.85f, 0.88f, 0.92f}, 0.7f},          // snow
};
enum Slot : u32 { kSun = 0, kExp = 1, kFogD = 2, kSunC = 3, kSky = 6, kGround = 9, kFogC = 12, kSlots = 15 };

f32 lerpf(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
f32 luma(const f32* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

// The kind's target at intensity 1 for the base values.
void kind_target(const KindEnv& k, const f32* b, f32* t) {
    t[kSun] = b[kSun] * k.sun_mul;
    t[kExp] = b[kExp] * k.exp_mul;
    t[kFogD] = b[kFogD] + k.fog_add;
    const f32 ls = luma(b + kSunC);
    for (u32 i = 0; i < 3; ++i) t[kSunC + i] = lerpf(b[kSunC + i], ls, k.sun_desat);
    for (u32 base : {static_cast<u32>(kSky), static_cast<u32>(kGround)}) {
        const f32 l = luma(b + base);
        for (u32 i = 0; i < 3; ++i) t[base + i] = lerpf(b[base + i], l, k.amb_desat) * k.amb_mul * k.amb_tint[i];
    }
    for (u32 i = 0; i < 3; ++i) t[kFogC + i] = lerpf(b[kFogC + i], k.fog_to[i], k.fog_mix);
}

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }

void env_slots(Environment3D& e, f32* out_ptrs[kSlots]) {
    out_ptrs[kSun] = &e.sun_intensity;
    out_ptrs[kExp] = &e.exposure;
    out_ptrs[kFogD] = &e.fog_density;
    f32* v3[4] = {&e.sun_color.x, &e.ambient_sky.x, &e.ambient_ground.x, &e.fog_color.x};
    for (u32 g = 0; g < 4; ++g)
        for (u32 i = 0; i < 3; ++i) out_ptrs[kSunC + g * 3 + i] = v3[g] + i;
}

void apply_environment(Frame& f, Weather& c) {
    Environment3D* env = f.world().try_resource<Environment3D>();
    if (!env) return;
    f32* p[kSlots];
    env_slots(*env, p);
    // A field that is not what this system wrote last tick was rewritten (DayCycle, edit): it is the new base.
    for (u32 i = 0; i < kSlots; ++i)
        if (!c.env_valid || !same_bits(*p[i], c.env_applied[i])) c.env_base[i] = *p[i];
    c.env_valid = 1;
    const u32 kind = static_cast<u32>(c.current);
    f32 t[kSlots];
    kind_target(kKindEnv[kind], c.env_base, t);
    const bool off = !(c.intensity > 0.0f) || c.current == WeatherKind::Clear;
    for (u32 i = 0; i < kSlots; ++i) {
        f32 v = off ? c.env_base[i] : lerpf(c.env_base[i], t[i], c.intensity);
        if (c.flash_mul != 1.0f && (i == kSun || (i >= kSky && i < kFogC))) v *= c.flash_mul;  // lightning flash
        *p[i] = v;
        c.env_applied[i] = v;
    }
}

void kit_weather(Frame& f) {
    bool first = true;  // one Weather per scene: the first in ECS order drives
    f.query<Weather>().each([&](Entity, Weather& c) {
        if (!first) return;
        first = false;
        tick_weather(f, c);
        apply_environment(f, c);
    });
}

// Snow cover (M-SNOWCOVER 6c): the same integer idiom as Weather.wet_q. Rises by intensity while it snows, else melts.
void kit_snow_cover(Frame& f) {
    const Weather* weather = nullptr;
    f.query<const Weather>().each([&](Entity, const Weather& w) {
        if (!weather) weather = &w;
    });
    const f64 dt = static_cast<f64>(f.dt);
    bool first = true;  // one SnowCover per scene: the first in ECS order drives
    f.query<SnowCover>().each([&](Entity, SnowCover& c) {
        if (!first) return;
        first = false;
        if (c.snow_q > kWetMax) c.snow_q = kWetMax;
        if (weather && weather->current == WeatherKind::Snow && weather->fade_den > 0) {
            const u64 rise = mul_div(static_cast<u64>(kWetMax) * weather->fade, 1,
                                     static_cast<u64>(weather->fade_den) * ticks_of(static_cast<f64>(c.build_seconds), dt));
            const u64 q = c.snow_q + rise;
            c.snow_q = q > kWetMax ? kWetMax : static_cast<u32>(q);
        } else {
            const u64 fall = kWetMax / ticks_of(static_cast<f64>(c.melt_seconds), dt);
            c.snow_q = c.snow_q > fall ? static_cast<u32>(c.snow_q - fall) : 0;
        }
        c.cover = static_cast<f32>(static_cast<f64>(c.snow_q) / static_cast<f64>(kWetMax));
    });
}

// Wind direction (M-WINDDIR 8a): the heading in 1/65536 degree as an integer, advanced by a fixed-point drift.
constexpr i64 kWindFull = 360ll * 65536;  // one turn in angle_q units

i64 wind_wrap(i64 q) {
    q %= kWindFull;
    return q < 0 ? q + kWindFull : q;
}

// Degrees to angle_q: NaN is 0, anything is clamped to +-1e9 first, rounded to the nearest quantum (half away from
// zero).
i64 wind_q_of_degrees(f32 degrees) {
    const f64 d = std::isnan(degrees) ? 0.0 : std::clamp(static_cast<f64>(degrees), -1e9, 1e9);
    return wind_wrap(std::llround(std::fmod(d, 360.0) * 65536.0));
}

void kit_wind_direction(Frame& f) {
    const f64 dt = static_cast<f64>(f.dt);
    bool first = true;  // one WindDirection per scene: the first in ECS order drives
    f.query<WindDirection>().each([&](Entity, WindDirection& c) {
        if (!first) return;
        first = false;
        const f64 drift = std::isnan(c.drift_degrees_per_minute)
                              ? 0.0
                              : std::clamp(static_cast<f64>(c.drift_degrees_per_minute), -3600.0, 3600.0);
        c.pad_ = 0;
        if (drift == 0.0) {  // the authored heading, no history
            c.angle_q = static_cast<u32>(wind_q_of_degrees(c.degrees));
            c.frac = 0;
            c.seeded = 0;
            return;
        }
        i64 q = c.seeded == 1 ? wind_wrap(c.angle_q) : wind_q_of_degrees(c.degrees);
        i64 frac = c.seeded == 1 ? static_cast<i64>(c.frac & 0xFFFFu) : 0;
        if (dt > 0.0) {
            // Ticks per minute: snapped to the integer when dt is a whole tick rate (f32 1/60 is 3599.9998 otherwise).
            f64 tpm = std::min(60.0 / dt, 1e12);
            const f64 r = std::round(tpm);
            if (r >= 1.0 && std::abs(tpm - r) < 1e-4 * r) tpm = r;
            // Step in 1/2^32 degree per tick, rounded toward +inf so the error is never negative: a negative drift
            // then floors to the same quantum as a positive one, and whole turns land exactly.
            const i64 step = static_cast<i64>(std::ceil(drift * 4294967296.0 / tpm));
            const i64 acc = frac + step;
            q = wind_wrap(q + (acc >> 16));  // floor: the arithmetic shift carries a negative drift correctly
            frac = acc & 0xFFFF;
        }
        c.angle_q = static_cast<u32>(q);
        c.frac = static_cast<u32>(frac);
        c.seeded = 1;
    });
}

}  // namespace

void install_weather(World& world, asset::AssetSystem* assets) {
    install_timed_light(world, assets);  // it adds kit_timed_light, which this system follows
    if (world.try_resource<WeatherInstalled>()) return;
    world.set_resource(WeatherInstalled{});
    world.register_components<Weather>();
    world.register_components<WindSway>();     // read by Kit3DExtract (M-WINDSWAY 1); no system
    world.register_components<WetSurfaces>();  // read by Kit3DExtract (M-WETSURFACE 5c); no system
    world.register_event<WeatherChanged>(Phase::FixedUpdate_Pre);
    world.add_system(Phase::FixedUpdate_Pre, "kit_weather", &kit_weather, "kit_timed_light");
    world.register_components<RainSplash>();  // M-RAINFX 7f: driven by kit_rain_splash (install_precipitation)
    world.register_components<ThunderDelay>();  // M-RAINFX 7e: advanced by kit_thunder_delay (install_precipitation)
    world.register_components<SnowCover>();  // M-SNOWCOVER 6c: advanced by kit_snow_cover, read by Kit3DExtract
    world.add_system(Phase::FixedUpdate_Pre, "kit_snow_cover", &kit_snow_cover, "kit_weather");
    world.register_components<WindDirection>();  // M-WINDDIR 8a: advanced by kit_wind_direction, read by Kit3DExtract (8d)
    world.add_system(Phase::FixedUpdate_Pre, "kit_wind_direction", &kit_wind_direction, "kit_weather");
    world.register_components<WindGust>();  // M-WINDGUST 12a: advanced by kit_wind_gust, read through effective_wind
    install_wind_gust(world);                // after kit_wind_direction (and so after kit_weather)
    world.register_components<GroundMist>();  // M-GROUNDMIST 13a: read by Kit3DExtract (13b); no system
    world.register_components<Moon>();        // M-NIGHTSKY 14a: read by Kit3DExtract (14b, 14e); no system
    world.register_components<RainRipples>();  // M-RAINFX 7c: read by Kit3DExtract; no system
    world.register_components<CloudLayer>();  // M-CLOUDS 9c: read by Kit3DExtract; no system
    world.register_components<WeatherAudio>();  // M-WEATHERAUDIO 10a: kit_weather_audio writes AudioSource::gain_db
    install_weather_audio(world);
    world.register_components<LightningBolt>();  // M-LIGHTNINGBOLT 11a: read by kit_lightning
}

}  // namespace mce::kit
