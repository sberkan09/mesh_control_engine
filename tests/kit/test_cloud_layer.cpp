// CloudLayer (M-CLOUDS 9c): a plain saved component, no system. Kit3DExtract builds render::Clouds only when the scene
// has a Weather and a CloudLayer: coverage = (fair + (overcast - fair) * kind * intensity) * strength, speed =
// Weather.wind * speed_scale, wind_dir from the first WindDirection (+x without one); any other scene keeps coverage 0.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/cloud_layer.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/weather.h"
#include "mce/kit/wind_direction.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

struct Rig {
    render::MeshStore store;
    World world;
    Kit3DExtract extract;
    u64 t = 0;
    Entity wind_e;
    Rig(bool with_weather, bool with_layer, WeatherKind kind = WeatherKind::Rain, bool with_wind = false,
        f32 degrees = 0.0f) {
        world.register_components<SaveKey>();
        install_kit3d(world, nullptr);
        install_weather(world, nullptr);
        extract.init(store, nullptr);
        world.spawn_with(LocalTransform{}, Camera3DComponent{});
        if (with_weather) {
            Weather w;
            w.target = kind;
            w.transition_seconds = 1.0f;
            world.spawn_with(SaveKey{1}, w);
        }
        if (with_layer) world.spawn_with(SaveKey{2}, CloudLayer{});
        if (with_wind) {
            WindDirection d;
            d.degrees = degrees;
            wind_e = world.spawn_with(SaveKey{3}, d);
        }
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, t++);
    }
    void set_layer(const CloudLayer& l) {
        world.query<CloudLayer&>().each([&](CloudLayer& c) { c = l; });
    }
    Weather weather() {
        Weather w;
        world.query<const Weather&>().each([&](const Weather& c) { w = c; });
        return w;
    }
    render::Clouds carrier() {
        render::Camera3D cam;
        REQUIRE(extract.camera(world, 1.0f, cam));
        render::Scene3D s(store, mem::heap());
        s.begin(cam, {320, 180});
        extract.extract(world, s, 1.0f);
        return s.clouds();
    }
};

}  // namespace

TEST_CASE("cloud_layer: defaults") {
    const CloudLayer c;
    CHECK(c.strength == 1.0f);
    CHECK(c.fair_coverage == 0.35f);
    CHECK(c.overcast_coverage == 0.9f);
    CHECK(c.scale_m == 900.0f);
    CHECK(c.speed_scale == 1.0f);
    CHECK(c.height_m == 2000.0f);
}

TEST_CASE("cloud_layer: a scene without the layer or without a Weather keeps coverage 0") {
    Rig no_layer(true, false);
    no_layer.run(120);
    REQUIRE(no_layer.weather().intensity > 0.0f);
    CHECK(no_layer.carrier().coverage == 0.0f);
    Rig no_weather(false, true);
    no_weather.run(60);
    CHECK(no_weather.carrier().coverage == 0.0f);
    Rig none(false, false);
    none.run(60);
    CHECK(none.carrier().coverage == 0.0f);
}

TEST_CASE("cloud_layer: clear weather gives the fair coverage, overcast kinds the overcast one") {
    Rig clear(true, true, WeatherKind::Clear);
    clear.run(120);
    CHECK(clear.carrier().coverage == doctest::Approx(0.35f));
    for (WeatherKind k : {WeatherKind::Overcast, WeatherKind::Rain, WeatherKind::Storm}) {
        Rig r(true, true, k);
        r.run(120);
        REQUIRE(r.weather().intensity == 1.0f);
        CHECK(r.carrier().coverage == doctest::Approx(0.9f));
    }
    Rig snow(true, true, WeatherKind::Snow);
    snow.run(120);
    REQUIRE(snow.weather().intensity == 1.0f);
    CHECK(snow.carrier().coverage == doctest::Approx(0.35f + (0.9f - 0.35f) * 0.9f));
}

TEST_CASE("cloud_layer: coverage rises with the weather intensity and scales with strength") {
    Rig r(true, true, WeatherKind::Overcast);
    f32 prev = 0.0f;
    for (u32 i = 0; i < 90; ++i) {
        r.run(1);
        const f32 inten = r.weather().intensity;
        const render::Clouds c = r.carrier();
        CHECK(c.coverage == doctest::Approx(0.35f + 0.55f * inten));
        CHECK(c.coverage >= prev);
        prev = c.coverage;
    }
    CloudLayer l;
    l.strength = 0.5f;
    r.set_layer(l);
    CHECK(r.carrier().coverage == doctest::Approx(0.45f));
    l.strength = 0.0f;
    r.set_layer(l);
    CHECK(r.carrier().coverage == 0.0f);
}

TEST_CASE("cloud_layer: speed is the weather wind times speed_scale, scale and height are copied") {
    Rig r(true, true, WeatherKind::Storm);
    r.run(120);
    CloudLayer l;
    l.speed_scale = 2.0f;
    l.scale_m = 500.0f;
    l.height_m = 3000.0f;
    r.set_layer(l);
    const render::Clouds c = r.carrier();
    CHECK(c.speed == doctest::Approx(r.weather().wind * 2.0f));
    CHECK(c.scale_m == doctest::Approx(500.0f));
    CHECK(c.height_m == doctest::Approx(3000.0f));
    CHECK(c.enabled);
}

TEST_CASE("cloud_layer: the drift follows the WindDirection, +x without one") {
    Rig plain(true, true);
    plain.run(120);
    render::Clouds c = plain.carrier();
    CHECK(c.wind_dir.x == doctest::Approx(1.0f));
    CHECK(c.wind_dir.y == doctest::Approx(0.0f));
    Rig turned(true, true, WeatherKind::Rain, true, 90.0f);
    turned.run(120);
    c = turned.carrier();
    CHECK(c.wind_dir.x == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(c.wind_dir.y == doctest::Approx(1.0f));
    turned.world.try_get_mut<WindDirection>(turned.wind_e)->degrees = 180.0f;
    turned.run(1);
    c = turned.carrier();
    CHECK(c.wind_dir.x == doctest::Approx(-1.0f));
    Rig no_weather(false, true, WeatherKind::Rain, true, 90.0f);  // a heading alone never creates clouds
    no_weather.run(10);
    CHECK(no_weather.carrier().coverage == 0.0f);
}

TEST_CASE("cloud_layer: NaN and out-of-range fields clamp") {
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    Rig r(true, true);
    r.run(120);
    CloudLayer l;
    l.strength = 7.0f;
    l.scale_m = 1e9f;
    l.speed_scale = 1000.0f;
    l.height_m = 1e9f;
    r.set_layer(l);
    render::Clouds c = r.carrier();
    CHECK(c.coverage == 1.0f);
    CHECK(c.scale_m <= 20000.0f);
    CHECK(c.speed <= 10.0f);
    CHECK(c.height_m <= 20000.0f);
    l = CloudLayer{};
    l.strength = -3.0f;
    l.scale_m = -5.0f;
    l.height_m = -5.0f;
    r.set_layer(l);
    c = r.carrier();
    CHECK(c.coverage == 0.0f);
    CHECK(c.scale_m >= 50.0f);
    CHECK(c.height_m >= 100.0f);
    l.strength = nan;
    l.fair_coverage = nan;
    l.overcast_coverage = nan;
    l.scale_m = nan;
    l.speed_scale = nan;
    l.height_m = nan;
    r.set_layer(l);
    c = r.carrier();
    CHECK(c.coverage == 0.0f);
    CHECK(std::isfinite(c.scale_m));
    CHECK(std::isfinite(c.speed));
    CHECK(std::isfinite(c.height_m));
    CHECK(std::isfinite(c.wind_dir.x));
    CHECK(std::isfinite(c.wind_dir.y));
}

TEST_CASE("cloud_layer: the Low tier drops the clouds") {
    Rig r(true, true);
    r.run(120);
    CHECK(r.carrier().coverage > 0.0f);
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    CHECK(r.carrier().coverage == 0.0f);
}

TEST_CASE("cloud_layer: a second CloudLayer is ignored and the extract is deterministic") {
    Rig r(true, true, WeatherKind::Clear);
    CloudLayer l;
    l.fair_coverage = 0.2f;
    r.set_layer(l);
    CloudLayer other;
    other.fair_coverage = 0.8f;
    r.world.spawn_with(SaveKey{4}, other);
    r.run(120);
    const render::Clouds a = r.carrier();
    const render::Clouds b = r.carrier();
    CHECK(a.coverage == doctest::Approx(0.2f));
    CHECK(a.coverage == b.coverage);
    CHECK(a.speed == b.speed);
}

TEST_CASE("cloud_layer: save and load round-trips the fields and the state hash") {
    Array<byte> bytes;
    u64 hash_a = 0;
    u64 hash_s = 0;
    CloudLayer l;
    l.strength = 0.7f;
    l.fair_coverage = 0.2f;
    l.overcast_coverage = 0.8f;
    l.scale_m = 400.0f;
    l.speed_scale = 3.0f;
    l.height_m = 1500.0f;
    {
        Rig a(true, true);
        a.set_layer(l);
        a.run(100);
        save::write_save(a.world, {}, bytes);
        hash_s = a.world.state_hash();
        a.run(50);
        hash_a = a.world.state_hash();
    }
    Rig b(true, true);
    b.run(77);  // past the 1 s weather fade, so the unsaved Environment3D resource matches
    REQUIRE(save::read_save(b.world, bytes.span()));
    CHECK(b.world.state_hash() == hash_s);
    b.t = 100;
    b.run(50);
    CHECK(b.world.state_hash() == hash_a);
    CloudLayer got;
    b.world.query<const CloudLayer&>().each([&](const CloudLayer& c) { got = c; });
    CHECK(got.strength == 0.7f);
    CHECK(got.fair_coverage == 0.2f);
    CHECK(got.overcast_coverage == 0.8f);
    CHECK(got.scale_m == 400.0f);
    CHECK(got.speed_scale == 3.0f);
    CHECK(got.height_m == 1500.0f);
}

TEST_CASE("cloud_layer: a scene without the layer hashes as before") {
    Rig a(true, false);
    Rig b(true, false);
    a.run(30);
    b.run(30);
    CHECK(a.world.state_hash() == b.world.state_hash());
}
