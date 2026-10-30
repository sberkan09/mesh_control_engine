// Extract feed of the meteors (M-METEOR 18b): Kit3DExtract fills render::MeteorSky from the first Meteor, the night fade
// the night-sky feed uses, the cloud coverage, the first Weather's rain and the extract's frame time; no Meteor (or
// enabled 0, a closed gate, an equirect sky texture) leaves the carrier default; Weather, WorldClock, Moon, Water,
// CloudLayer and Environment3D are never written.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>

#include "mce/containers/array.h"
#include "mce/convar.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/cloud_layer.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/meteor.h"
#include "mce/kit/moon.h"
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
bool same_meteor(const render::MeteorSky& a, const render::MeteorSky& b) {
    return same_bits(a.level, b.level) && same_bits(a.rate, b.rate) && same_bits(a.brightness, b.brightness) &&
           same_bits(a.length, b.length) && same_bits(a.speed, b.speed) && same_bits(a.time, b.time) && a.seed == b.seed &&
           a.enabled == b.enabled;
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
    Entity meteor_e;

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
        env().time_of_day = 0.0f;  // midnight
        REQUIRE(extract.camera(world, 1.0f, cam));
    }
    void add_meteor(const Meteor& m = {}) { meteor_e = world.spawn_with(SaveKey{2}, m); }
    void add_weather(WeatherKind k, f32 intensity) {
        Weather w;
        w.current = k;
        w.target = k;
        w.intensity = intensity;
        world.spawn_with(SaveKey{3}, w);
    }
    void add_clouds(const CloudLayer& l = {}) { world.spawn_with(SaveKey{4}, l); }
    Meteor& meteor() { return *world.try_get_mut<Meteor>(meteor_e); }
    Environment3D& env() { return world.resource<Environment3D>(); }
    const render::MeteorSky& frame() {
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
        return scene.meteor();
    }
};

}  // namespace

TEST_CASE("meteor extract: no Meteor gives the default carrier") {
    Rig a;
    a.frame();
    const render::MeteorSky def;
    CHECK(same_meteor(a.scene.meteor(), def));
    CHECK(a.scene.meteor().level == 0.0f);

    Rig off;
    off.add_meteor();
    off.meteor().enabled = 0;
    off.frame();
    CHECK(same_meteor(off.scene.meteor(), def));

    Rig silent;  // rate 0: the helper's level is exactly 0
    silent.add_meteor();
    silent.meteor().rate = 0.0f;
    CHECK(same_meteor(silent.frame(), def));
}

TEST_CASE("meteor extract: day gives level 0 with night_only, midnight the helper's level") {
    Rig r;
    r.add_meteor();
    const Meteor m = r.meteor();
    const render::MeteorSky& s = r.frame();
    CHECK(s.enabled);
    CHECK(s.level == doctest::Approx(meteor_level(m, 1.0f, 0.0f, 0.0f)));
    CHECK(s.level == doctest::Approx(1.0f));
    r.env().time_of_day = 12.0f;
    CHECK(r.frame().level == 0.0f);
    r.meteor().night_only = 0;  // shows by day too
    CHECK(r.frame().level == doctest::Approx(meteor_level(r.meteor(), 0.0f, 0.0f, 0.0f)));
    r.meteor().night_only = 1;
    r.env().time_of_day = 18.5f;  // dusk: the fade is the one the night sky uses
    const f32 dusk = r.frame().level;
    r.add_meteor();  // (a second Meteor is ignored: the first in ECS order is read)
    CHECK(r.frame().level == doctest::Approx(dusk));
}

TEST_CASE("meteor extract: thick cloud and rain lower the level") {
    Rig clear;
    clear.add_meteor();
    const f32 full = clear.frame().level;

    Rig c;
    c.add_meteor();
    c.add_weather(WeatherKind::Overcast, 1.0f);
    c.add_clouds();
    const f32 covered = c.frame().level;
    const f32 cov = c.scene.clouds().coverage;
    REQUIRE(cov > 0.0f);
    CHECK(covered < full);
    CHECK(covered == doctest::Approx(meteor_level(c.meteor(), 1.0f, cov, 0.0f)));

    Rig r;
    r.add_meteor();
    r.add_weather(WeatherKind::Rain, 0.5f);
    const f32 wet = r.frame().level;
    CHECK(wet < full);
    CHECK(wet == doctest::Approx(meteor_level(r.meteor(), 1.0f, r.scene.clouds().coverage, 0.5f)));

    Rig storm;
    storm.add_meteor();
    storm.add_weather(WeatherKind::Rain, 1.0f);
    CHECK(storm.frame().level == 0.0f);
}

TEST_CASE("meteor extract: no clock means no night fade unless night_only is 0") {
    Rig r;
    r.add_meteor();
    r.env().time_of_day = -1.0f;
    CHECK(r.frame().level == 0.0f);
    r.meteor().night_only = 0;
    CHECK(r.frame().level == doctest::Approx(1.0f));
}

TEST_CASE("meteor extract: an equirect sky texture suppresses the meteors") {
    Rig r;
    r.add_meteor();
    CHECK(r.frame().level > 0.0f);
    r.env().sky_texture.guid = asset::Guid{1, 2};
    const render::MeteorSky def;
    CHECK(same_meteor(r.frame(), def));  // also clears last frame's carrier
}

TEST_CASE("meteor extract: the Low tier and r.meteor 0 zero the level") {
    Rig r;
    r.add_meteor();
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    CHECK(r.frame().level == 0.0f);
    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    CHECK(r.frame().level > 0.0f);

    ConVar* cv = ConVar::find(StringId("r.meteor"));
    REQUIRE(cv != nullptr);
    cv->set_float(0.0f);
    const f32 off = r.frame().level;
    cv->reset();
    CHECK(off == 0.0f);
    CHECK(r.frame().level > 0.0f);
}

TEST_CASE("meteor extract: time is the extract's frame time, wrapped, and is set without a Moon") {
    Rig r;
    r.add_meteor();
    r.extract.set_frame_dt(0.25f);
    const render::MeteorSky& first = r.frame();
    CHECK(first.time == doctest::Approx(0.25f));
    CHECK(same_bits(first.time, r.scene.time()));  // the value set_time got in the same extract
    r.frame();
    CHECK(r.scene.meteor().time == doctest::Approx(0.5f));
    CHECK(same_bits(r.scene.meteor().time, r.scene.time()));
    for (int i = 0; i < 400; ++i) r.frame();  // past the 60 s period
    const f32 expect = meteor_wrap_time(static_cast<f64>(r.scene.time()));
    CHECK(same_bits(r.scene.meteor().time, expect));
    CHECK(r.scene.meteor().time < static_cast<f32>(kMeteorPeriodSeconds));
    CHECK(r.scene.time() > static_cast<f32>(kMeteorPeriodSeconds));
}

TEST_CASE("meteor extract: the authoring is passed through, clamped") {
    Rig r;
    Meteor m;
    m.rate = 12.0f;
    m.brightness = 2.5f;
    m.length = 1.5f;
    m.speed = 0.75f;
    m.seed = 321;
    r.add_meteor(m);
    const render::MeteorSky s = r.frame();
    CHECK(s.rate == doctest::Approx(12.0f));
    CHECK(s.brightness == doctest::Approx(2.5f));
    CHECK(s.length == doctest::Approx(1.5f));
    CHECK(s.speed == doctest::Approx(0.75f));
    CHECK(s.seed == 321u);
    r.meteor().rate = 99.0f;
    r.meteor().brightness = 9.0f;
    r.meteor().length = 9.0f;
    r.meteor().speed = 0.0f;  // outside the range: the default scale
    r.meteor().seed = 100000;
    const render::MeteorSky c = r.frame();
    CHECK(c.rate == doctest::Approx(30.0f));
    CHECK(c.brightness == doctest::Approx(4.0f));
    CHECK(c.length == doctest::Approx(2.0f));
    CHECK(c.speed == doctest::Approx(1.0f));
    CHECK(c.seed == 65535u);
}

TEST_CASE("meteor extract: Weather, WorldClock, Moon, Water, CloudLayer and Environment3D are never written") {
    const auto build = [](Rig& r, bool with_meteor) {
        r.add_weather(WeatherKind::Overcast, 0.5f);
        r.add_clouds();
        r.world.spawn_with(SaveKey{5}, Moon{});
        r.world.spawn_with(SaveKey{6}, Water{});
        if (with_meteor) r.add_meteor();
    };
    const auto hashes = [](Rig& r) {
        u32 h[5] = {};
        r.world.query<const Weather&>().each([&](const Weather& c) { h[0] = bytes_hash(c); });
        r.world.query<const WorldClock&>().each([&](const WorldClock& c) { h[1] = bytes_hash(c); });
        r.world.query<const Moon&>().each([&](const Moon& c) { h[2] = bytes_hash(c); });
        r.world.query<const Water&>().each([&](const Water& c) { h[3] = bytes_hash(c); });
        r.world.query<const CloudLayer&>().each([&](const CloudLayer& c) { h[4] = bytes_hash(c); });
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
    CHECK(b.scene.meteor().level > 0.0f);
    CHECK(hashes(a) == hashes(b));
    CHECK(bytes_hash(b.env()) == env_before);
    CHECK(bytes_hash(a.env()) == bytes_hash(b.env()));
}
