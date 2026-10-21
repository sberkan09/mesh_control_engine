#include "mce/kit/rain_splash.h"

#include <bit>
#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/weather.h"

namespace mce::kit {
namespace {

struct SplashInstalled {};

constexpr f32 kPerTick = 4.0f;  // drops per tick at intensity 1 and rate 1
constexpr f32 kGravity = 9.81f;
constexpr f32 kTwoPi = 6.28318530717958647692f;

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

f32 clamp_field(f32 v, f32 def, f32 lo, f32 hi) {
    if (std::isnan(v)) return def;
    return v < lo ? lo : (v > hi ? hi : v);
}

const Weather* first_weather(World& w) {
    const Weather* out = nullptr;
    w.query<const Weather>().each([&](Entity, const Weather& c) {
        if (!out) out = &c;
    });
    return out;
}

void switch_off(ParticleEmitter3D& e) {
    set(e.emitting, 0u);
    set(e.rate, 0.0f);
    set(e.burst, 0u);
}

void kit_rain_splash(Frame& f) {
    World& w = f.world();
    const Weather* wx = first_weather(w);
    const bool wet =
        wx && wx->intensity > 0.0f && (wx->current == WeatherKind::Rain || wx->current == WeatherKind::Storm);
    const f32 intensity = wet ? (wx->intensity > 1.0f ? 1.0f : wx->intensity) : 0.0f;

    bool cam_found = false, cam_primary = false;
    math::vec3 cam;
    w.query<const Camera3DComponent&, const LocalTransform&>().each(
        [&](Entity, const Camera3DComponent& c, const LocalTransform& t) {
            if (cam_found && (cam_primary || !c.primary)) return;
            cam_found = true;
            cam_primary = c.primary != 0;
            cam = t.position;
        });

    const f32 dt = f.dt;
    f.query<RainSplash, ParticleEmitter3D, LocalTransform>().each(
        [&](Entity, RainSplash& s, ParticleEmitter3D& e, LocalTransform& t) {
            if (!wet || !cam_found) {
                switch_off(e);
                return;
            }
            const f32 rate = clamp_field(s.rate, 1.0f, 0.0f, 64.0f);
            const f32 height = clamp_field(s.height_m, 0.15f, 0.01f, 5.0f);
            const f32 life = clamp_field(s.life_seconds, 0.25f, 0.02f, 5.0f);
            const f32 radius = clamp_field(s.radius_m, 25.0f, 0.5f, 500.0f);

            const u32 count = static_cast<u32>(std::lround(kPerTick * intensity * rate));
            const f32 speed = std::sqrt(2.0f * kGravity * height);
            const f32 ticks_alive = dt > 0.0f ? std::ceil(life / dt) + 1.0f : 1.0f;
            const f32 live = static_cast<f32>(count) * (ticks_alive > 4096.0f ? 4096.0f : ticks_alive);
            const u32 max_particles = static_cast<u32>(live < 64.0f ? 64.0f : (live > 65536.0f ? 65536.0f : live));

            set(e.box, math::vec3{0.4f, 0.0f, 0.4f});
            set(e.offset, math::vec3{0.0f, 0.0f, 0.0f});
            set(e.rate, 0.0f);
            set(e.emitting, 1u);
            set(e.additive, 0u);
            set(e.collide, 0u);
            set(e.bounce, 0.0f);
            set(e.drag, 0.0f);
            set(e.direction, math::vec3{0.0f, 1.0f, 0.0f});
            set(e.gravity, math::vec3{0.0f, -kGravity, 0.0f});
            set(e.spread_deg, 35.0f);
            set(e.speed_min, speed * 0.6f);
            set(e.speed_max, speed);
            set(e.lifetime_min, life * 0.8f);
            set(e.lifetime_max, life);
            set(e.size_start, 0.03f);
            set(e.size_end, 0.05f);
            set(e.color_start, math::vec4{0.8f, 0.86f, 0.95f, 0.5f});
            set(e.color_end, math::vec4{0.8f, 0.86f, 0.95f, 0.0f});
            set(e.max_particles, max_particles);

            if (count == 0) {
                set(e.burst, 0u);
                return;
            }
            const u32 h = hash_mix(s.seed, s.counter);
            const f32 a = kTwoPi * (static_cast<f32>(h >> 8) / 16777216.0f);
            const f32 r = radius * std::sqrt(static_cast<f32>(hash_mix(h, 0x51u) >> 8) / 16777216.0f);
            set(t.position, math::vec3{cam.x + r * std::cos(a), t.position.y, cam.z + r * std::sin(a)});
            set(e.seed, hash_mix(h, 0xA3u));
            set(e.burst, count > 256u ? 256u : count);
            ++s.counter;
            set(e.burst_version, s.counter);
        });
}

}  // namespace

void install_rain_splash(World& world) {
    if (world.try_resource<SplashInstalled>()) return;
    world.set_resource(SplashInstalled{});
    world.register_components<RainSplash, ParticleEmitter3D, LocalTransform, Camera3DComponent>();
    world.add_system(Phase::FixedUpdate_Pre, "kit_rain_splash", &kit_rain_splash, "kit_precipitation");
}

}  // namespace mce::kit
