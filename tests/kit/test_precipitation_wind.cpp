// Precipitation follows the scene WindDirection (M-WINDDIR 8f): without one the rain and snow emitter lean is the old
// +x formula bit for bit; with one the lean turns along the heading (90 = +z, 180 = -x). Weather.wind never reaches 0
// (its floor is 0.1), so a calm lean is not reachable through the weather.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <new>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/precipitation.h"
#include "mce/kit/weather.h"
#include "mce/kit/wind_direction.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

ParticleEmitter3D clean_emitter() {
    alignas(ParticleEmitter3D) unsigned char buf[sizeof(ParticleEmitter3D)];
    std::memset(buf, 0, sizeof buf);
    return *new (buf) ParticleEmitter3D{};
}

u32 bits(f32 x) { return std::bit_cast<u32>(x); }

struct Wx {
    World w;
    u64 t = 0;
    Entity we, pe;
    // degrees < 0: no WindDirection in the scene.
    Wx(WeatherKind kind, f32 degrees) {
        install_precipitation(w, nullptr);
        w.set_resource(Environment3D{});
        Weather c;
        c.target = kind;
        c.transition_seconds = 1.0f;
        we = w.spawn_with(c);
        pe = w.spawn_with(Precipitation3D{}, clean_emitter(), LocalTransform{});
        if (degrees >= 0.0f) {
            WindDirection d;
            d.degrees = degrees;
            w.spawn_with(d);
        }
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    ParticleEmitter3D& em() { return *w.try_get_mut<ParticleEmitter3D>(pe); }
    Weather& wx() { return *w.try_get_mut<Weather>(we); }
};

struct Old {
    math::vec3 dir, grav;
};
Old old_formula(bool snow, f32 wind, const Precipitation3D& p) {
    const f32 k = snow ? 0.8f : 0.5f;
    const f32 x = k * wind;
    const f32 inv = 1.0f / std::sqrt(x * x + 1.0f);
    const f32 g = snow ? 2.0f : 3.0f;
    const f32 v = snow ? -0.3f * p.snow_speed : -0.5f * p.rain_speed;
    return {{x * inv, -inv, 0.0f}, {g * wind, v, 0.0f}};
}

}  // namespace

TEST_CASE("precipitation wind: without a WindDirection the lean equals the old formula bit for bit") {
    for (const bool snow : {false, true}) {
        Wx x(snow ? WeatherKind::Snow : WeatherKind::Rain, -1.0f);
        x.run(60);
        const f32 wind = x.wx().wind;
        REQUIRE(wind > 0.0f);
        const Old o = old_formula(snow, wind, Precipitation3D{});
        CHECK(bits(x.em().direction.x) == bits(o.dir.x));
        CHECK(bits(x.em().direction.y) == bits(o.dir.y));
        CHECK(bits(x.em().direction.z) == bits(o.dir.z));
        CHECK(bits(x.em().gravity.x) == bits(o.grav.x));
        CHECK(bits(x.em().gravity.y) == bits(o.grav.y));
        CHECK(bits(x.em().gravity.z) == bits(o.grav.z));
        CHECK(bits(x.em().direction.z) == bits(0.0f));
    }
}

TEST_CASE("precipitation wind: a WindDirection at degrees 0 is the same bits as none") {
    for (const bool snow : {false, true}) {
        Wx a(snow ? WeatherKind::Snow : WeatherKind::Rain, -1.0f);
        Wx b(snow ? WeatherKind::Snow : WeatherKind::Rain, 0.0f);
        a.run(60);
        b.run(60);
        CHECK(bits(a.em().direction.x) == bits(b.em().direction.x));
        CHECK(bits(a.em().direction.z) == bits(b.em().direction.z));
        CHECK(bits(a.em().gravity.x) == bits(b.em().gravity.x));
        CHECK(bits(a.em().gravity.z) == bits(b.em().gravity.z));
    }
}

TEST_CASE("precipitation wind: 90 degrees leans along +z, 180 degrees mirrors x") {
    for (const bool snow : {false, true}) {
        Wx n(snow ? WeatherKind::Snow : WeatherKind::Rain, -1.0f);
        n.run(60);
        const Old o = old_formula(snow, n.wx().wind, Precipitation3D{});
        Wx z(snow ? WeatherKind::Snow : WeatherKind::Rain, 90.0f);
        z.run(60);
        CHECK(z.em().direction.x == doctest::Approx(0.0f).epsilon(1e-6));
        CHECK(z.em().direction.z == doctest::Approx(o.dir.x));
        CHECK(z.em().direction.y == doctest::Approx(o.dir.y));
        CHECK(z.em().gravity.x == doctest::Approx(0.0f).epsilon(1e-6));
        CHECK(z.em().gravity.z == doctest::Approx(o.grav.x));
        CHECK(z.em().gravity.y == doctest::Approx(o.grav.y));
        Wx m(snow ? WeatherKind::Snow : WeatherKind::Rain, 180.0f);
        m.run(60);
        CHECK(m.em().direction.x == doctest::Approx(-o.dir.x));
        CHECK(m.em().direction.z == doctest::Approx(0.0f).epsilon(1e-6));
        CHECK(m.em().gravity.x == doctest::Approx(-o.grav.x));
        CHECK(m.em().gravity.z == doctest::Approx(0.0f).epsilon(1e-6));
    }
}

TEST_CASE("precipitation wind: no Weather returns early, a WindDirection alone changes nothing") {
    World w;
    install_precipitation(w, nullptr);
    WindDirection d;
    d.degrees = 90.0f;
    w.spawn_with(d);
    const Entity pe = w.spawn_with(Precipitation3D{}, clean_emitter(), LocalTransform{});
    const ParticleEmitter3D before = *w.try_get<ParticleEmitter3D>(pe);
    for (u64 i = 0; i < 30; ++i) w.tick(kDt, i);
    const ParticleEmitter3D& after = *w.try_get<ParticleEmitter3D>(pe);
    CHECK(bits(after.direction.x) == bits(before.direction.x));
    CHECK(bits(after.gravity.z) == bits(before.gravity.z));
    CHECK(after.emitting == before.emitting);
}

TEST_CASE("precipitation wind: the same run twice has the same state hash") {
    Wx a(WeatherKind::Storm, 123.0f), b(WeatherKind::Storm, 123.0f);
    a.run(200);
    b.run(200);
    CHECK(bits(a.em().direction.z) == bits(b.em().direction.z));
    CHECK(bits(a.em().gravity.z) == bits(b.em().gravity.z));
    CHECK(a.w.state_hash() == b.w.state_hash());
}
