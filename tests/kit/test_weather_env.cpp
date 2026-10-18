// Weather to environment (M-WEATHER 2): pinned rain numbers at full and half intensity (with and without a
// DayCycle at noon), no drift over 1000 ticks, clearing returns the authored bits, clear leaves the environment
// bit-identical, an edit mid-run becomes the new base, a stable state hash and an exact save/resume mid-rain.

#include <doctest/doctest.h>

#include <cstring>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/world.h"
#include "mce/kit/day_cycle.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/weather.h"
#include "mce/kit/world_clock.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

struct Wev {
    World w;
    u64 t = 0;
    Entity e;
    explicit Wev(WeatherKind target, bool with_weather = true, bool with_cycle = false, f32 transition = 30.0f) {
        w.register_components<SaveKey>();
        install_weather(w, nullptr);
        install_day_cycle(w, nullptr);
        if (!w.try_resource<Environment3D>()) w.set_resource(Environment3D{});
        if (with_cycle) {
            w.spawn_with(SaveKey{2}, DayCycle{});
            WorldClock c;
            c.hour = 12.0f;
            c.day_length_seconds = 1.0e9f;  // noon for the whole run
            w.spawn_with(SaveKey{3}, c);
        }
        if (with_weather) {
            Weather c;
            c.target = target;
            c.transition_seconds = transition;
            e = w.spawn_with(SaveKey{1}, c);
        } else {
            e = w.spawn_with(SaveKey{1});
        }
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    Environment3D& env() { return *w.try_resource<Environment3D>(); }
    Weather& wx() { return *w.try_get_mut<Weather>(e); }
};

bool same(const Environment3D& a, const Environment3D& b) {
    auto bits = [](f32 x) { u32 u; std::memcpy(&u, &x, 4); return u; };
    auto v3 = [&](const math::vec3& p, const math::vec3& q) {
        return bits(p.x) == bits(q.x) && bits(p.y) == bits(q.y) && bits(p.z) == bits(q.z);
    };
    return bits(a.sun_intensity) == bits(b.sun_intensity) && bits(a.exposure) == bits(b.exposure) &&
           bits(a.fog_density) == bits(b.fog_density) && v3(a.sun_color, b.sun_color) &&
           v3(a.ambient_sky, b.ambient_sky) && v3(a.ambient_ground, b.ambient_ground) && v3(a.fog_color, b.fog_color);
}

f32 luma(const math::vec3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

// The rain target of an ambient colour at full intensity: desaturate 0.6 toward luma, x0.7.
f32 rain_amb(f32 c, f32 l) { return (c + (l - c) * 0.6f) * 0.7f; }

}  // namespace

TEST_CASE("weather_env: rain at full intensity equals the formula, half intensity is the midpoint") {
    Wev x(WeatherKind::Rain);
    const Environment3D base = x.env();
    x.run(900);  // intensity 0.5
    REQUIRE(x.wx().intensity == 0.5f);
    const Environment3D half = x.env();
    x.run(900);
    REQUIRE(x.wx().intensity == 1.0f);
    const Environment3D& full = x.env();
    CHECK(full.sun_intensity == doctest::Approx(0.4f));  // 2.0 x 0.2
    CHECK(full.exposure == doctest::Approx(1.1f));
    CHECK(full.fog_density == doctest::Approx(0.006f));
    const f32 ls = luma(base.ambient_sky);
    CHECK(full.ambient_sky.x == doctest::Approx(rain_amb(base.ambient_sky.x, ls)));
    CHECK(full.ambient_sky.z == doctest::Approx(rain_amb(base.ambient_sky.z, ls)));
    CHECK(full.fog_color.x == doctest::Approx(base.fog_color.x + (0.50f - base.fog_color.x) * 0.6f));
    CHECK(half.sun_intensity == doctest::Approx(1.2f));  // midpoint of 2.0 and 0.4
    CHECK(half.exposure == doctest::Approx(1.05f));
    CHECK(half.fog_density == doctest::Approx(0.003f));
    CHECK(half.ambient_sky.y == doctest::Approx((base.ambient_sky.y + full.ambient_sky.y) * 0.5f));
}

TEST_CASE("weather_env: with a DayCycle at noon the weather stacks on the cycle's values") {
    Wev ref(WeatherKind::Clear, false, true);
    ref.run(1);
    const Environment3D noon = ref.env();
    Wev x(WeatherKind::Rain, true, true);
    x.run(1800);
    REQUIRE(x.wx().intensity == 1.0f);
    CHECK(x.env().sun_intensity == doctest::Approx(noon.sun_intensity * 0.2f));
    CHECK(x.env().exposure == doctest::Approx(noon.exposure * 1.1f));
    CHECK(x.env().fog_density == doctest::Approx(noon.fog_density + 0.006f));
    const f32 l = luma(noon.ambient_ground);
    CHECK(x.env().ambient_ground.x == doctest::Approx(rain_amb(noon.ambient_ground.x, l)));
    x.run(300);  // still the same: the cycle rewrites, the weather re-applies
    CHECK(x.env().sun_intensity == doctest::Approx(noon.sun_intensity * 0.2f));
}

TEST_CASE("weather_env: no drift over 1000 ticks of constant weather without a DayCycle") {
    Wev x(WeatherKind::Storm, true, false, 1.0f);  // full after 60 ticks
    x.run(100);
    REQUIRE(x.wx().intensity == 1.0f);
    const Environment3D at100 = x.env();
    x.run(900);
    CHECK(same(x.env(), at100));
    CHECK(at100.sun_intensity == doctest::Approx(2.0f * 0.08f));
}

TEST_CASE("weather_env: clearing returns the authored bits; clear leaves the environment untouched") {
    Wev x(WeatherKind::Snow, true, false, 2.0f);
    const Environment3D authored = x.env();
    x.run(300);
    CHECK_FALSE(same(x.env(), authored));
    x.wx().target = WeatherKind::Clear;
    x.run(300);
    REQUIRE(x.wx().intensity == 0.0f);
    CHECK(same(x.env(), authored));

    Wev c(WeatherKind::Clear);
    Wev plain(WeatherKind::Clear, false);
    c.run(200);
    plain.run(200);
    CHECK(same(c.env(), plain.env()));
    CHECK(same(c.env(), authored));
}

TEST_CASE("weather_env: a write to the environment mid-run becomes the new base") {
    Wev x(WeatherKind::Rain, true, false, 1.0f);
    x.run(100);
    CHECK(x.env().sun_intensity == doctest::Approx(0.4f));
    x.env().sun_intensity = 5.0f;  // a script or edit
    x.run(1);
    CHECK(x.env().sun_intensity == doctest::Approx(1.0f));  // 5.0 x 0.2
    x.run(500);
    CHECK(x.env().sun_intensity == doctest::Approx(1.0f));
    x.wx().target = WeatherKind::Clear;
    x.run(200);
    CHECK(x.env().sun_intensity == 5.0f);
}

TEST_CASE("weather_env: the same run twice has the same state hash") {
    auto run = [](bool with) {
        Wev x(WeatherKind::Storm, with, true, 10.0f);
        x.run(1500);
        return x.w.state_hash();
    };
    CHECK(run(true) == run(true));
    CHECK(run(true) != run(false));
}

TEST_CASE("weather_env: a save mid-rain resumes exactly") {
    Array<byte> bytes;
    Environment3D expected;
    {
        Wev a(WeatherKind::Rain, true, false, 20.0f);
        a.run(700);
        REQUIRE(a.wx().intensity > 0.0f);
        REQUIRE(a.wx().intensity < 1.0f);
        save::write_save(a.w, {}, bytes);
        a.run(1000);
        expected = a.env();
    }
    Wev b(WeatherKind::Clear, true, false, 20.0f);  // the authored environment, as a fresh scene has it
    auto st = save::read_save(b.w, bytes.span());
    REQUIRE(st);
    b.t = 700;
    b.run(1000);
    CHECK(same(b.env(), expected));
}
