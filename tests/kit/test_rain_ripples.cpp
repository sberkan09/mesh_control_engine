// RainRipples (M-RAINFX 7c): a plain saved component, no system. Kit3DExtract builds render::Ripples only when the
// scene has a Weather, a WetSurfaces and a RainRipples and Weather.intensity > 0 (strength = strength * intensity);
// any other scene keeps strength 0.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/rain_ripples.h"
#include "mce/kit/weather.h"
#include "mce/kit/wet_surfaces.h"
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
    Rig(bool with_weather, bool with_wet, bool with_ripples, WeatherKind kind = WeatherKind::Rain) {
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
        if (with_wet) world.spawn_with(SaveKey{2}, WetSurfaces{});
        if (with_ripples) {
            RainRipples r;
            r.density = 0.4f;
            r.speed = 2.0f;
            r.ring_width = 0.3f;
            world.spawn_with(SaveKey{3}, r);
        }
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, t++);
    }
    void set_ripples(f32 strength, f32 density = 0.4f, f32 speed = 2.0f, f32 ring = 0.3f) {
        world.query<RainRipples&>().each([&](RainRipples& c) {
            c.strength = strength;
            c.density = density;
            c.speed = speed;
            c.ring_width = ring;
        });
    }
    Weather weather() {
        Weather w;
        world.query<const Weather&>().each([&](const Weather& c) { w = c; });
        return w;
    }
    render::Ripples carrier() {
        render::Camera3D cam;
        REQUIRE(extract.camera(world, 1.0f, cam));
        render::Scene3D s(store, mem::heap());
        s.begin(cam, {320, 180});
        extract.extract(world, s, 1.0f);
        return s.ripples();
    }
};

}  // namespace

TEST_CASE("rain_ripples: defaults") {
    const RainRipples r;
    CHECK(r.strength == 1.0f);
    CHECK(r.density == 0.5f);
    CHECK(r.speed == 1.0f);
    CHECK(r.ring_width == 0.15f);
}

TEST_CASE("rain_ripples: a scene without the component keeps ripples at strength 0") {
    Rig r(true, true, false);
    r.run(120);
    REQUIRE(r.weather().intensity > 0.0f);
    CHECK(r.carrier().strength == 0.0f);
    Rig none(false, false, false);
    none.run(60);
    CHECK(none.carrier().strength == 0.0f);
}

TEST_CASE("rain_ripples: requires Weather and WetSurfaces") {
    Rig no_wet(true, false, true);
    no_wet.run(120);
    REQUIRE(no_wet.weather().intensity > 0.0f);
    CHECK(no_wet.carrier().strength == 0.0f);
    Rig no_weather(false, true, true);
    no_weather.run(120);
    CHECK(no_weather.carrier().strength == 0.0f);
    Rig all(true, true, true);
    all.run(120);
    CHECK(all.carrier().strength > 0.0f);
}

TEST_CASE("rain_ripples: clear weather (intensity 0) spawns nothing") {
    Rig r(true, true, true, WeatherKind::Clear);
    r.run(120);
    CHECK(r.carrier().strength == 0.0f);
}

TEST_CASE("rain_ripples: strength scales with the weather intensity and the field") {
    Rig r(true, true, true);
    r.set_ripples(0.5f);
    f32 prev = 0.0f;
    for (u32 i = 0; i < 90; ++i) {
        r.run(1);
        const f32 expect = r.weather().intensity * 0.5f;
        const render::Ripples c = r.carrier();
        CHECK(c.strength == doctest::Approx(expect));
        CHECK(c.strength >= prev);
        prev = c.strength;
    }
    REQUIRE(r.weather().intensity == 1.0f);
    const render::Ripples c = r.carrier();
    CHECK(c.strength == doctest::Approx(0.5f));
    CHECK(c.density == doctest::Approx(0.4f));
    CHECK(c.speed == doctest::Approx(2.0f));
    CHECK(c.ring_width == doctest::Approx(0.3f));
    CHECK(c.enabled);
    r.set_ripples(0.0f);
    CHECK(r.carrier().strength == 0.0f);
}

TEST_CASE("rain_ripples: NaN and out-of-range fields clamp") {
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    Rig r(true, true, true);
    r.run(120);
    REQUIRE(r.weather().intensity == 1.0f);
    r.set_ripples(7.0f, 5.0f, 100.0f, 9.0f);
    render::Ripples c = r.carrier();
    CHECK(c.strength == 1.0f);
    CHECK(c.density == 1.0f);
    CHECK(c.ring_width == 1.0f);
    CHECK(c.speed <= 10.0f);
    CHECK(c.speed >= 0.1f);
    r.set_ripples(-3.0f, -1.0f, -4.0f, -2.0f);
    c = r.carrier();
    CHECK(c.strength == 0.0f);
    CHECK(c.density == 0.0f);
    CHECK(c.ring_width == 0.0f);
    CHECK(c.speed >= 0.1f);
    r.set_ripples(nan, nan, nan, nan);
    c = r.carrier();
    CHECK(c.strength == 0.0f);
    CHECK(std::isfinite(c.density));
    CHECK(std::isfinite(c.ring_width));
    CHECK(std::isfinite(c.speed));
    CHECK(c.speed >= 0.1f);
    CHECK(c.density >= 0.0f);
    CHECK(c.density <= 1.0f);
}

TEST_CASE("rain_ripples: the Low tier drops the ripples") {
    Rig r(true, true, true);
    r.run(120);
    CHECK(r.carrier().strength > 0.0f);
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    CHECK(r.carrier().strength == 0.0f);
}

TEST_CASE("rain_ripples: a second RainRipples is ignored") {
    Rig r(true, true, true);
    r.set_ripples(0.5f);
    RainRipples other;
    other.strength = 1.0f;
    r.world.spawn_with(SaveKey{4}, other);
    r.run(120);
    CHECK(r.carrier().strength == doctest::Approx(0.5f));
}

TEST_CASE("rain_ripples: save and load round-trips the fields and the state hash") {
    Array<byte> bytes;
    u64 hash_a = 0;
    u64 hash_s = 0;
    {
        Rig a(true, true, true);
        a.set_ripples(0.7f, 0.2f, 3.0f, 0.4f);
        a.run(100);
        save::write_save(a.world, {}, bytes);
        hash_s = a.world.state_hash();
        a.run(50);
        hash_a = a.world.state_hash();
    }
    Rig b(true, true, true);
    b.run(77);  // past the 1 s weather fade, so the unsaved Environment3D resource matches
    REQUIRE(save::read_save(b.world, bytes.span()));
    CHECK(b.world.state_hash() == hash_s);
    b.t = 100;
    b.run(50);
    CHECK(b.world.state_hash() == hash_a);
    RainRipples got;
    b.world.query<const RainRipples&>().each([&](const RainRipples& c) { got = c; });
    CHECK(got.strength == 0.7f);
    CHECK(got.density == 0.2f);
    CHECK(got.speed == 3.0f);
    CHECK(got.ring_width == 0.4f);
}
