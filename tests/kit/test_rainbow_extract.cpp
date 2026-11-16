// Extract feed of the rainbow (M-RAINBOW 17b): Kit3DExtract fills render::RainbowSky from the first Rainbow, the sun
// elevation, the first Weather (wetness, rain) and the cloud coverage; no Rainbow (or enabled 0, a closed gate, an
// equirect sky texture) leaves the carrier default and the Scene3D as before; Weather, WorldClock, Moon, Aurora, Water,
// CloudLayer and Environment3D are never written.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>

#include "mce/containers/array.h"
#include "mce/convar.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/aurora.h"
#include "mce/kit/cloud_layer.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/moon.h"
#include "mce/kit/rainbow.h"
#include "mce/kit/water.h"
#include "mce/kit/weather.h"
#include "mce/kit/world_clock.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"
#include "mce/string_id.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
bool same_vec(const math::vec3& a, const math::vec3& b) { return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z); }
bool same_aurora(const render::AuroraSky& a, const render::AuroraSky& b) {
    return same_bits(a.level, b.level) && same_bits(a.base_height, b.base_height) && same_bits(a.height_span, b.height_span) &&
           same_bits(a.azimuth.x, b.azimuth.x) && same_bits(a.azimuth.y, b.azimuth.y) &&
           same_bits(a.half_width_cos, b.half_width_cos) && a.curtain_count == b.curtain_count && same_bits(a.time, b.time) &&
           same_bits(a.speed, b.speed) && same_vec(a.low_color, b.low_color) && same_vec(a.high_color, b.high_color) &&
           a.enabled == b.enabled;
}
bool same_rainbow(const render::RainbowSky& a, const render::RainbowSky& b) {
    return same_bits(a.level, b.level) && same_bits(a.band_width, b.band_width) && same_bits(a.secondary, b.secondary) &&
           same_vec(a.anti_solar, b.anti_solar) && a.enabled == b.enabled;
}
bool same_scene(const render::Scene3D& a, const render::Scene3D& b) {
    const render::DirectionalLight &la = a.light(), &lb = b.light();
    return same_vec(la.direction, lb.direction) && same_vec(la.color, lb.color) && same_bits(la.intensity, lb.intensity) &&
           same_vec(la.ambient_sky, lb.ambient_sky) && same_vec(la.ambient_ground, lb.ambient_ground) &&
           la.shadows == lb.shadows && same_bits(a.sky().sun_intensity, b.sky().sun_intensity) &&
           same_bits(a.sky().sun_size, b.sky().sun_size) && same_vec(a.sky().horizon, b.sky().horizon) &&
           same_bits(a.fog().density, b.fog().density) && same_vec(a.fog().color, b.fog().color) &&
           same_bits(a.clouds().coverage, b.clouds().coverage) &&
           same_bits(a.night_sky().night_fade, b.night_sky().night_fade) &&
           same_bits(a.night_sky().star_density, b.night_sky().star_density) && same_bits(a.water().level_y, b.water().level_y) &&
           a.water().enabled == b.water().enabled && same_bits(a.time(), b.time()) && same_aurora(a.aurora(), b.aurora()) &&
           same_rainbow(a.rainbow(), b.rainbow());
}

template <class T>
u32 bytes_hash(const T& v) {
    u32 h = 2166136261u;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(&v);
    for (usize i = 0; i < sizeof(T); ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

struct Rig {
    render::MeshStore store;
    World world;
    Kit3DExtract extract;
    render::Scene3D scene;
    render::Camera3D cam;
    Entity rainbow_e;

    Rig() : scene(store, mem::heap()) {
        mesh_fixture::Builder box;
        box.add_node(-1);
        mesh_fixture::Box b;
        b.half = {0.5f, 0.5f, 0.5f};
        box.add_box(b);
        const Array<byte> blob = box.build();
        REQUIRE(store.upload(1, 1, blob.span()).has_value());
        world.register_components<SaveKey>();
        install_kit3d(world, nullptr);
        install_weather(world, nullptr);
        extract.init(store, nullptr);
        world.spawn_with(LocalTransform{}, Camera3DComponent{});
        world.spawn_with(SaveKey{1}, WorldClock{});
        env().time_of_day = -1.0f;  // no clock: the fixed sun_direction below, 30 degrees above the horizon
        env().sun_direction = math::vec3{0.8660254f, -0.5f, 0.0f};  // unit, sun height 0.5
        REQUIRE(extract.camera(world, 1.0f, cam));
    }
    void add_rainbow(const Rainbow& r = {}) { rainbow_e = world.spawn_with(SaveKey{2}, r); }
    void add_weather(WeatherKind k, f32 intensity, f32 wetness = 0.0f) {
        Weather w;
        w.current = k;
        w.target = k;
        w.intensity = intensity;
        w.wetness = wetness;
        world.spawn_with(SaveKey{3}, w);
    }
    void add_clouds(const CloudLayer& l = {}) { world.spawn_with(SaveKey{4}, l); }
    Rainbow& rainbow() { return *world.try_get_mut<Rainbow>(rainbow_e); }
    Environment3D& env() { return world.resource<Environment3D>(); }
    const render::RainbowSky& frame() {
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
        return scene.rainbow();
    }
};

}  // namespace

TEST_CASE("rainbow extract: no Rainbow gives the default carrier and the Scene3D as before") {
    Rig a;
    a.add_weather(WeatherKind::Clear, 0.0f, 1.0f);
    a.frame();
    const render::RainbowSky def;
    CHECK(same_rainbow(a.scene.rainbow(), def));
    CHECK(a.scene.rainbow().level == 0.0f);

    Rig off;
    off.add_weather(WeatherKind::Clear, 0.0f, 1.0f);
    off.add_rainbow();
    off.rainbow().enabled = 0;
    off.frame();
    CHECK(same_scene(a.scene, off.scene));

    Rig nan;
    nan.add_weather(WeatherKind::Clear, 0.0f, std::nanf(""));
    nan.add_rainbow();
    nan.frame();
    CHECK(same_rainbow(nan.scene.rainbow(), def));

    Rig noweather;  // no Weather: wetness 0, no bow
    noweather.add_rainbow();
    CHECK(noweather.frame().level == 0.0f);
}

TEST_CASE("rainbow extract: a Rainbow changes only the rainbow carrier of the Scene3D") {
    Rig a, b;
    a.add_weather(WeatherKind::Clear, 0.0f, 1.0f);
    b.add_weather(WeatherKind::Clear, 0.0f, 1.0f);
    b.add_rainbow();
    a.frame();
    b.frame();
    CHECK(b.scene.rainbow().level > 0.0f);
    CHECK(b.scene.rainbow().enabled);
    b.scene.set_rainbow(a.scene.rainbow());
    CHECK(same_scene(a.scene, b.scene));
}

TEST_CASE("rainbow extract: clear after rain gives the helper's level; rain, night, dry ground give 0") {
    Rig r;
    r.add_weather(WeatherKind::Clear, 0.0f, 1.0f);
    r.add_rainbow();
    const f32 el = std::asin(0.5f) * (180.0f / 3.14159265358979323846f);  // sun height 0.5 -> 30 degrees
    CHECK(el == doctest::Approx(30.0f));
    const f32 expect = rainbow_level(r.rainbow(), el, 1.0f, 0.0f, 0.0f);
    REQUIRE(expect > 0.0f);
    CHECK(r.frame().level == doctest::Approx(expect).epsilon(1e-5));
    CHECK(r.scene.rainbow().band_width == doctest::Approx(1.0f));
    CHECK(r.scene.rainbow().secondary == doctest::Approx(1.0f));

    Rig rain;  // raining: the (1 - rain) factor closes it
    rain.add_weather(WeatherKind::Rain, 1.0f, 1.0f);
    rain.add_rainbow();
    CHECK(rain.frame().level == 0.0f);
    Rig storm;
    storm.add_weather(WeatherKind::Storm, 1.0f, 1.0f);
    storm.add_rainbow();
    CHECK(storm.frame().level == 0.0f);
    Rig half;  // light rain: scaled by 1 - intensity
    half.add_weather(WeatherKind::Rain, 0.5f, 1.0f);
    half.add_rainbow();
    CHECK(half.frame().level == doctest::Approx(expect * 0.5f).epsilon(1e-5));

    Rig dry;
    dry.add_weather(WeatherKind::Clear, 0.0f, 0.0f);
    dry.add_rainbow();
    CHECK(dry.frame().level == 0.0f);

    r.env().time_of_day = 0.0f;  // midnight: the sun is below the horizon
    CHECK(r.frame().level == 0.0f);
    r.env().time_of_day = -1.0f;
    r.env().sun_direction = math::vec3{0.0f, 1.0f, 0.0f};  // light travelling up: the sun is below the horizon
    CHECK(r.frame().level == 0.0f);
    r.env().sun_direction = math::vec3{0.0f, -1.0f, 0.0f};  // sun at the zenith: past max_sun_elevation_deg
    CHECK(r.frame().level == 0.0f);
}

TEST_CASE("rainbow extract: a second Rainbow is ignored, authoring is passed through") {
    Rig r;
    r.add_weather(WeatherKind::Clear, 0.0f, 1.0f);
    Rainbow a;
    a.band_width = 1.5f;
    a.secondary = 0.25f;
    a.strength = 2.0f;
    r.add_rainbow(a);
    const f32 first = r.frame().level;
    CHECK(r.scene.rainbow().band_width == doctest::Approx(1.5f));
    CHECK(r.scene.rainbow().secondary == doctest::Approx(0.25f));
    r.world.spawn_with(SaveKey{9}, Rainbow{});
    CHECK(r.frame().level == doctest::Approx(first));
    r.rainbow().band_width = 5.0f;  // clamped to the Range
    r.rainbow().secondary = -1.0f;
    r.frame();
    CHECK(r.scene.rainbow().band_width == doctest::Approx(2.0f));
    CHECK(r.scene.rainbow().secondary == doctest::Approx(0.0f));
}

TEST_CASE("rainbow extract: thick cloud lowers the level by the cloud coverage the clouds block set") {
    Rig clear;
    clear.add_weather(WeatherKind::Clear, 0.0f, 1.0f);
    clear.add_rainbow();
    const f32 full = clear.frame().level;

    Rig r;
    r.add_weather(WeatherKind::Overcast, 1.0f, 1.0f);  // overcast: not rain, so the cloud term is what lowers it
    r.add_clouds();
    r.add_rainbow();
    const f32 covered = r.frame().level;
    const f32 cov = r.scene.clouds().coverage;
    REQUIRE(cov > 0.0f);
    CHECK(covered < full);
    const f32 el = std::asin(0.5f) * (180.0f / 3.14159265358979323846f);
    CHECK(covered == doctest::Approx(rainbow_level(r.rainbow(), el, 1.0f, 0.0f, cov)).epsilon(1e-5));
}

TEST_CASE("rainbow extract: an equirect sky texture suppresses the rainbow") {
    Rig r;
    r.add_weather(WeatherKind::Clear, 0.0f, 1.0f);
    r.add_rainbow();
    CHECK(r.frame().level > 0.0f);
    r.env().sky_texture.guid = asset::Guid{1, 2};
    const render::RainbowSky def;
    CHECK(same_rainbow(r.frame(), def));  // also clears last frame's carrier
}

TEST_CASE("rainbow extract: the Low tier and r.rainbow 0 zero the level") {
    Rig r;
    r.add_weather(WeatherKind::Clear, 0.0f, 1.0f);
    r.add_rainbow();
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    CHECK(r.frame().level == 0.0f);
    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    CHECK(r.frame().level > 0.0f);

    ConVar* cv = ConVar::find(StringId("r.rainbow"));
    REQUIRE(cv != nullptr);
    cv->set_float(0.0f);
    const f32 off = r.frame().level;
    cv->reset();
    CHECK(off == 0.0f);
    CHECK(r.frame().level > 0.0f);
}

TEST_CASE("rainbow extract: anti_solar is the unit direction the light travels (negated direction toward the sun)") {
    Rig r;
    r.add_weather(WeatherKind::Clear, 0.0f, 1.0f);
    r.add_rainbow();
    const math::vec3 toward_sun = math::vec3{-0.8660254f, 0.5f, 0.0f};
    const render::RainbowSky s = r.frame();
    const math::vec3 a = s.anti_solar;
    CHECK(std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z) == doctest::Approx(1.0f).epsilon(1e-5));
    CHECK(a.x == doctest::Approx(-toward_sun.x).epsilon(1e-5));
    CHECK(a.y == doctest::Approx(-toward_sun.y).epsilon(1e-5));
    CHECK(std::abs(a.z + toward_sun.z) < 1e-5f);
    CHECK(a.y < 0.0f);  // the sun is up, so the anti-solar point is below the horizon from the camera
    // an unnormalised direction is normalised
    r.env().sun_direction = math::vec3{4.0f, -1.0f, 0.0f};  // 14 degrees up
    const math::vec3 b = r.frame().anti_solar;
    CHECK(b.x == doctest::Approx(4.0f / std::sqrt(17.0f)).epsilon(1e-5));
    CHECK(b.y == doctest::Approx(-1.0f / std::sqrt(17.0f)).epsilon(1e-5));
    // with a clock the direction is the clock's sun
    r.env().time_of_day = 16.0f;
    const math::vec3 clock = r.frame().anti_solar;
    const math::vec3 expect = render::sun_direction_at(16.0f);
    CHECK(clock.x == doctest::Approx(expect.x).epsilon(1e-4));
    CHECK(clock.y == doctest::Approx(expect.y).epsilon(1e-4));
    CHECK(clock.z == doctest::Approx(expect.z).epsilon(1e-4));
}

TEST_CASE("rainbow extract: Weather, WorldClock, Moon, Aurora, Water, CloudLayer and Environment3D are never written") {
    const auto build = [](Rig& r, bool with_rainbow) {
        r.add_weather(WeatherKind::Clear, 0.0f, 0.8f);
        r.add_clouds();
        r.world.spawn_with(SaveKey{5}, Moon{});
        r.world.spawn_with(SaveKey{6}, Water{});
        r.world.spawn_with(SaveKey{7}, Aurora{});
        if (with_rainbow) r.add_rainbow();
    };
    const auto hashes = [](Rig& r) {
        u32 h[7] = {};
        r.world.query<const Weather&>().each([&](const Weather& c) { h[0] = bytes_hash(c); });
        r.world.query<const WorldClock&>().each([&](const WorldClock& c) { h[1] = bytes_hash(c); });
        r.world.query<const Moon&>().each([&](const Moon& c) { h[2] = bytes_hash(c); });
        r.world.query<const Aurora&>().each([&](const Aurora& c) { h[3] = bytes_hash(c); });
        r.world.query<const Water&>().each([&](const Water& c) { h[4] = bytes_hash(c); });
        r.world.query<const CloudLayer&>().each([&](const CloudLayer& c) { h[5] = bytes_hash(c); });
        h[6] = bytes_hash(r.env());
        u32 x = 2166136261u;
        for (const u32 v : h) x = (x ^ v) * 16777619u;
        return x;
    };
    Rig a, b;
    build(a, false);
    build(b, true);
    const u32 env_before = bytes_hash(b.env());
    for (int i = 0; i < 3; ++i) {
        a.frame();
        b.frame();
    }
    CHECK(b.scene.rainbow().level > 0.0f);
    CHECK(hashes(a) == hashes(b));
    CHECK(bytes_hash(b.env()) == env_before);
    CHECK(bytes_hash(a.env()) == bytes_hash(b.env()));
}
