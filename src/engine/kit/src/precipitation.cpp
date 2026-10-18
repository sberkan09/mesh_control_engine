#include "mce/kit/precipitation.h"

#include <bit>
#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/weather.h"

namespace mce::kit {
namespace {

struct PrecipitationInstalled {};

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
void set(f32& d, f32 v) {
    if (!same_bits(d, v)) d = v;
}
void set(u32& d, u32 v) {
    if (d != v) d = v;
}
void set(math::vec3& d, const math::vec3& v) {
    set(d.x, v.x);
    set(d.y, v.y);
    set(d.z, v.z);
}
void set(math::vec4& d, const math::vec4& v) {
    set(d.x, v.x);
    set(d.y, v.y);
    set(d.z, v.z);
    set(d.w, v.w);
}
u32 hash_mix(u32 seed, u32 counter) {
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

const Weather* first_weather(World& w) {
    const Weather* out = nullptr;
    w.query<const Weather>().each([&](Entity, const Weather& c) {
        if (!out) out = &c;
    });
    return out;
}

math::vec3 slanted(f32 x) {
    const f32 inv = 1.0f / std::sqrt(x * x + 1.0f);
    return {x * inv, -inv, 0.0f};
}

// Writes the rain or snow table into the emitter, field by field and only when the bits change.
void apply_emitter(ParticleEmitter3D& e, const Precipitation3D& p, bool snow, bool storm, f32 intensity, f32 wind) {
    const f32 rate_max = snow ? p.snow_rate : p.rain_rate * 1.6f;
    f32 rate = (snow ? p.snow_rate : p.rain_rate) * intensity;
    if (storm) rate *= 1.6f;
    const f32 speed = snow ? p.snow_speed : p.rain_speed;
    const f32 life = p.height / speed;
    const f32 life_max = life * (snow ? 1.25f : 1.2f);
    set(e.box, math::vec3{p.area, 0.5f, p.area});
    set(e.rate, rate);
    set(e.emitting, 1u);
    set(e.additive, 0u);
    set(e.collide, 0u);
    set(e.bounce, 0.0f);
    set(e.drag, 0.0f);
    set(e.speed_min, speed * (snow ? 0.8f : 0.9f));
    set(e.speed_max, speed * (snow ? 1.2f : 1.1f));
    set(e.lifetime_min, life);
    set(e.lifetime_max, life_max);
    set(e.max_particles, static_cast<u32>(std::ceil(rate_max * life_max)) + 64u);
    if (snow) {
        set(e.direction, slanted(0.8f * wind));
        set(e.gravity, math::vec3{2.0f * wind, -0.3f * p.snow_speed, 0.0f});
        set(e.spread_deg, 25.0f);
        set(e.size_start, 0.09f);
        set(e.size_end, 0.07f);
        set(e.color_start, math::vec4{0.95f, 0.97f, 1.0f, 0.9f});
        set(e.color_end, math::vec4{0.95f, 0.97f, 1.0f, 0.6f});
    } else {
        set(e.direction, slanted(0.5f * wind));
        set(e.gravity, math::vec3{3.0f * wind, -0.5f * p.rain_speed, 0.0f});
        set(e.spread_deg, 2.0f);
        set(e.size_start, 0.04f);
        set(e.size_end, 0.04f);
        set(e.color_start, math::vec4{0.55f, 0.62f, 0.75f, 0.35f});
        set(e.color_end, math::vec4{0.55f, 0.62f, 0.75f, 0.15f});
    }
}

void kit_precipitation(Frame& f) {
    World& w = f.world();
    const Weather* wx = first_weather(w);
    if (!wx) return;
    const WeatherKind kind = wx->current;
    const f32 intensity = wx->intensity;
    const f32 wind = wx->wind;
    const bool wet = intensity > 0.0f && (kind == WeatherKind::Rain || kind == WeatherKind::Storm || kind == WeatherKind::Snow);

    bool cam_found = false, cam_primary = false;
    math::vec3 cam;
    w.query<const Camera3DComponent&, const LocalTransform&>().each([&](Entity, const Camera3DComponent& c, const LocalTransform& t) {
        if (cam_found && (cam_primary || !c.primary)) return;
        cam_found = true;
        cam_primary = c.primary != 0;
        cam = t.position;
    });

    f.query<const Precipitation3D, ParticleEmitter3D, LocalTransform>().each(
        [&](Entity, const Precipitation3D& p, ParticleEmitter3D& e, LocalTransform& t) {
            if (wet) {
                apply_emitter(e, p, kind == WeatherKind::Snow, kind == WeatherKind::Storm, intensity, wind);
            } else {
                set(e.emitting, 0u);
                set(e.rate, 0.0f);
            }
            if (p.follow_camera && cam_found) set(t.position, math::vec3{cam.x, cam.y + p.height, cam.z});
        });
}

void kit_lightning(Frame& f) {
    World& w = f.world();
    Weather* wx = nullptr;
    w.query<Weather>().each([&](Entity, Weather& c) {
        if (!wx) wx = &c;
    });
    if (!wx) return;
    bool first = true;
    f.query<Lightning>().each([&](Entity, Lightning& l) {
        if (!first) return;
        first = false;
        if (wx->current != WeatherKind::Storm) {
            l.timer = 0;
            l.remaining = 0;
        } else {
            const f64 dt = static_cast<f64>(f.dt);
            if (l.timer == 0 && l.remaining == 0) {  // schedule the next strike
                const f32 lo = l.min_gap_seconds, hi = l.max_gap_seconds > lo ? l.max_gap_seconds : lo;
                const f64 u = static_cast<f64>(hash_mix(l.seed, l.counter * 2u)) / 4294967296.0;
                l.timer = static_cast<u32>(ticks_of(static_cast<f64>(lo) + static_cast<f64>(hi - lo) * u, dt));
            }
            if (l.remaining > 0) {
                --l.remaining;
            } else if (l.timer > 0 && --l.timer == 0) {
                l.remaining = l.flash_ticks;
                const u32 delay = 30u + hash_mix(l.seed, l.counter * 2u + 1u) % 271u;
                ++l.counter;
                f.events().push(LightningStruck{delay});
            }
        }
        f32 mul = 1.0f;
        if (l.remaining > 0 && l.flash_ticks > 0)
            mul = 1.0f + (l.flash_boost - 1.0f) * (static_cast<f32>(l.remaining) / static_cast<f32>(l.flash_ticks));
        if (!same_bits(wx->flash_mul, mul)) wx->flash_mul = mul;
    });
}

}  // namespace

void install_precipitation(World& world, asset::AssetSystem* assets) {
    install_weather(world, assets);
    if (world.try_resource<PrecipitationInstalled>()) return;
    world.set_resource(PrecipitationInstalled{});
    world.register_components<Precipitation3D, Lightning, LocalTransform, Camera3DComponent>();
    world.register_event<LightningStruck>(Phase::FixedUpdate_Pre);
    world.add_system(Phase::FixedUpdate_Pre, "kit_lightning", &kit_lightning, "kit_weather");
    world.add_system(Phase::FixedUpdate_Pre, "kit_precipitation", &kit_precipitation, "kit_lightning");
}

}  // namespace mce::kit
