// Extract feed of the dust (M-DUSTWIND 25b): Kit3DExtract builds one local alpha-blended render::ParticleEmitter from
// dust_look and adds it to the scene (id 0xFFFF0004, in a box following the camera). No Dust, enabled 0, strength 0 and
// an amount of 0 (calm, rain) add nothing (the emitter list and the stats stay bit-identical); Dust, Weather,
// WindDirection and every ParticleEmitter3D are never written.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/dust.h"
#include "mce/kit/firefly.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/leaf_fall.h"
#include "mce/kit/pollen.h"
#include "mce/kit/weather.h"
#include "mce/kit/wind_direction.h"
#include "mce/kit/wind_gust.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr u32 kDustId = 0xFFFF0004u;
constexpr u32 kPollenId = 0xFFFF0003u;
constexpr u32 kFireflyId = 0xFFFF0002u;
constexpr u32 kLeafId = 0xFFFF0001u;

struct Shot {
    Array<render::ParticleEmitter> emitters;
    u32 stat = 0;
};

struct Rig {
    render::MeshStore store;
    World world;
    Kit3DExtract extract;
    render::Scene3D scene;
    render::Camera3D cam;
    Entity du_e, weather_e, wd_e;

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
        REQUIRE(extract.camera(world, 1.0f, cam));
    }
    void add_dust() { du_e = world.spawn_with(SaveKey{2}, Dust{}); }
    Dust& dust() { return *world.try_get_mut<Dust>(du_e); }
    void add_wind(f32 wind) {
        Weather w;
        w.wind = wind;
        weather_e = world.spawn_with(SaveKey{5}, w);
    }
    void add_rain(f32 wind, f32 intensity) {
        Weather w;
        w.wind = wind;
        w.current = WeatherKind::Rain;
        w.intensity = intensity;
        weather_e = world.spawn_with(SaveKey{5}, w);
    }
    void add_wind_direction(f32 degrees) {
        WindDirection d;
        d.degrees = degrees;
        d.angle_q = static_cast<u32>(degrees * 65536.0f);  // engine-written by the system; there is no tick here
        wd_e = world.spawn_with(SaveKey{6}, d);
    }
    Shot frame() {
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
        Shot f;
        for (const render::ParticleEmitter& e : scene.particle_emitters()) f.emitters.push_back(e);
        f.stat = extract.stats().particle_emitters;
        return f;
    }
};

const render::ParticleEmitter* dust_of(const Shot& f) {
    for (const render::ParticleEmitter& e : f.emitters)
        if (e.id == kDustId) return &e;
    return nullptr;
}

}  // namespace

TEST_CASE("dust extract: no Dust adds nothing") {
    Rig base;
    base.add_wind(1.0f);
    const Shot a = base.frame();
    CHECK(a.emitters.size() == 0);
    CHECK(a.stat == 0);
}

TEST_CASE("dust extract: calm, no Weather, enabled 0 and strength 0 add nothing") {
    Rig calm;
    calm.add_dust();
    calm.add_wind(0.1f);  // below the default wind_start 0.3
    CHECK(calm.frame().emitters.size() == 0);
    CHECK(calm.extract.stats().particle_emitters == 0);

    Rig none;  // no Weather: calm
    none.add_dust();
    CHECK(none.frame().emitters.size() == 0);

    Rig off;
    off.add_dust();
    off.add_wind(1.0f);
    off.dust().enabled = 0;
    CHECK(off.frame().emitters.size() == 0);

    Rig zero;
    zero.add_dust();
    zero.add_wind(1.0f);
    zero.dust().strength = 0.0f;
    CHECK(zero.frame().emitters.size() == 0);
}

TEST_CASE("dust extract: a strong wind adds exactly one alpha-blended emitter with the helper's look") {
    Rig r;
    r.add_dust();
    r.add_wind(0.8f);
    const Shot f = r.frame();
    REQUIRE(f.emitters.size() == 1);
    CHECK(f.stat == 1);
    const render::ParticleEmitter& e = f.emitters[0];
    CHECK(e.id == kDustId);

    const f32 amount = dust_amount(r.dust(), 0.8f, 0.0f);
    CHECK(amount == doctest::Approx(1.0f));
    const DustLook l = dust_look(r.dust(), amount, 0.8f, math::vec2{1.0f, 0.0f});
    CHECK(e.rate == doctest::Approx(l.rate));
    CHECK(e.rate == doctest::Approx(40.0f));
    CHECK(e.speed_min == doctest::Approx(l.speed_min));
    CHECK(e.speed_max == doctest::Approx(l.speed_max));
    CHECK(e.direction.x == doctest::Approx(l.direction.x));
    CHECK(e.direction.y == doctest::Approx(l.direction.y));
    CHECK(e.direction.z == doctest::Approx(l.direction.z));
    CHECK(e.spread == doctest::Approx(l.spread));
    CHECK(e.gravity.y == doctest::Approx(l.gravity.y));
    CHECK(e.drag == doctest::Approx(l.drag));
    CHECK(e.size_start == doctest::Approx(l.size));
    CHECK(e.size_end == doctest::Approx(l.size));
    CHECK(e.lifetime_min == doctest::Approx(l.lifetime_min));
    CHECK(e.lifetime_max == doctest::Approx(l.lifetime_max));
    CHECK(e.color_start.x == doctest::Approx(l.color_start.x));
    CHECK(e.color_start.w == doctest::Approx(0.6f));
    CHECK(e.color_end.w == 0.0f);
    CHECK(e.box.x == doctest::Approx(14.0f));
    CHECK(e.box.y == 0.0f);
    CHECK(e.box.z == doctest::Approx(14.0f));
    CHECK(e.max_particles == 1024u);
    CHECK(e.seed == 1u);
    CHECK_FALSE(e.additive);
    CHECK_FALSE(e.collide);
    CHECK(e.emitting);
    CHECK(e.burst == 0u);

    // authoring flows through; max_particles is clamped to 16..4096; the manual floor works in a calm
    r.dust().max_particles = 4;
    CHECK(dust_of(r.frame())->max_particles == 16u);
    r.dust().max_particles = 100000;
    CHECK(dust_of(r.frame())->max_particles == 4096u);
    r.dust().seed = 77;
    r.dust().strength = 0.5f;
    const Shot g = r.frame();
    REQUIRE(g.emitters.size() == 1);
    CHECK(g.emitters[0].rate == doctest::Approx(20.0f));
    CHECK(g.emitters[0].seed == 77u);

    Rig manual;
    manual.add_dust();
    manual.dust().manual = 0.5f;
    const Shot m = manual.frame();
    REQUIRE(m.emitters.size() == 1);
    CHECK(dust_of(m)->rate == doctest::Approx(20.0f));
}

TEST_CASE("dust extract: rain closes the dust, only Rain and Storm count") {
    Rig rain;
    rain.add_dust();
    rain.add_rain(0.8f, 1.0f);
    CHECK(rain.frame().emitters.size() == 0);

    Rig light;  // half rain halves the amount
    light.add_dust();
    light.add_rain(0.8f, 0.5f);
    const Shot fl = light.frame();
    REQUIRE(fl.emitters.size() == 1);
    CHECK(dust_of(fl)->rate == doctest::Approx(20.0f));

    Rig snow;  // another kind with an intensity is not rain
    snow.add_dust();
    snow.add_wind(0.8f);
    Weather* w = snow.world.try_get_mut<Weather>(snow.weather_e);
    REQUIRE(w != nullptr);
    w->current = WeatherKind::Snow;
    w->intensity = 1.0f;
    CHECK(snow.frame().emitters.size() == 1);
}

TEST_CASE("dust extract: the id never collides and the emitter follows the camera") {
    Rig r;
    r.add_dust();
    r.add_wind(0.8f);
    Array<Entity> spawned;
    for (u32 i = 0; i < 4; ++i) {
        ParticleEmitter3D p;
        LocalTransform t;
        t.position = {f32(i), 0.0f, 0.0f};
        spawned.push_back(r.world.spawn_with(t, p));
    }
    const Shot f = r.frame();
    REQUIRE(f.emitters.size() == 5);
    CHECK(f.stat == 5);
    u32 count = 0;
    for (const render::ParticleEmitter& e : f.emitters) {
        if (e.id == kDustId) {
            ++count;
            continue;
        }
        bool known = false;
        for (const Entity s : spawned) known = known || e.id == s.index + 1;
        CHECK(known);
    }
    CHECK(count == 1);
    CHECK(kDustId != kPollenId);
    CHECK(kDustId != kFireflyId);
    CHECK(kDustId != kLeafId);
    for (const Entity s : spawned) CHECK(s.index + 1 != kDustId);

    const render::ParticleEmitter* e0 = dust_of(f);
    REQUIRE(e0 != nullptr);
    CHECK(e0->position.x == doctest::Approx(r.cam.position.x));
    CHECK(e0->position.y == doctest::Approx(r.cam.position.y + 3.0f));
    CHECK(e0->position.z == doctest::Approx(r.cam.position.z));

    r.cam.position = {30.0f, 4.0f, -12.0f};
    r.dust().height_m = 8.0f;
    const render::ParticleEmitter* e1 = dust_of(r.frame());
    REQUIRE(e1 != nullptr);
    CHECK(e1->position.x == doctest::Approx(30.0f));
    CHECK(e1->position.y == doctest::Approx(12.0f));
    CHECK(e1->position.z == doctest::Approx(-12.0f));
}

TEST_CASE("dust extract: the wind direction turns the stream") {
    Rig windy;  // no WindDirection: +x
    windy.add_dust();
    windy.add_wind(0.8f);
    const render::ParticleEmitter* w = dust_of(windy.frame());
    REQUIRE(w != nullptr);
    CHECK(w->direction.x > 0.9f);
    CHECK(w->direction.z == doctest::Approx(0.0f).epsilon(1e-6));

    Rig turned;  // 90 degrees: +z in the engine's (x, z) plane
    turned.add_dust();
    turned.add_wind(0.8f);
    turned.add_wind_direction(90.0f);
    const render::ParticleEmitter* t = dust_of(turned.frame());
    REQUIRE(t != nullptr);
    const math::vec2 v = wind_direction_vector(*turned.world.try_get<WindDirection>(turned.wd_e));
    const Weather& wx = *turned.world.try_get<Weather>(turned.weather_e);
    const DustLook l = dust_look(turned.dust(), 1.0f, effective_wind(wx, nullptr, kGustSway), v);
    CHECK(t->direction.x == doctest::Approx(l.direction.x));
    CHECK(t->direction.z == doctest::Approx(l.direction.z));
    CHECK(std::abs(t->direction.z) > 0.9f);
    CHECK(std::abs(t->direction.x) < 0.01f);

    Rig no_weather;  // a WindDirection alone is calm and +x; the manual floor keeps the dust on
    no_weather.add_dust();
    no_weather.dust().manual = 1.0f;
    no_weather.add_wind_direction(90.0f);
    const render::ParticleEmitter* n = dust_of(no_weather.frame());
    REQUIRE(n != nullptr);
    CHECK(n->direction.x > 0.9f);
}

TEST_CASE("dust extract: the Low tier halves the rate and the budget") {
    Rig r;
    r.add_dust();
    r.add_wind(0.8f);
    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    const Shot high = r.frame();
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    const Shot low = r.frame();
    REQUIRE(dust_of(high) != nullptr);
    REQUIRE(dust_of(low) != nullptr);
    CHECK(dust_of(low)->rate == doctest::Approx(0.5f * dust_of(high)->rate));
    CHECK(dust_of(low)->max_particles == dust_of(high)->max_particles / 2u);
    CHECK(low.stat == 1);
}

TEST_CASE("dust extract: leaf, firefly, pollen and dust emitters coexist") {
    Rig r;
    r.add_dust();
    r.add_wind(0.8f);
    Firefly ff;
    ff.manual = 1.0f;
    ff.night_only = 0;
    r.world.spawn_with(SaveKey{7}, ff);
    LeafFall l;
    l.manual = 1.0f;
    r.world.spawn_with(SaveKey{8}, l);
    Pollen p;
    p.manual = 1.0f;
    p.day_only = 0;
    r.world.spawn_with(SaveKey{9}, p);
    const Shot f = r.frame();
    REQUIRE(f.emitters.size() == 4);
    CHECK(f.stat == 4);
    bool leaf = false, fire = false, pollen = false;
    for (const render::ParticleEmitter& e : f.emitters) {
        leaf = leaf || e.id == kLeafId;
        fire = fire || e.id == kFireflyId;
        pollen = pollen || e.id == kPollenId;
    }
    CHECK(leaf);
    CHECK(fire);
    CHECK(pollen);
    REQUIRE(dust_of(f) != nullptr);
    CHECK_FALSE(dust_of(f)->additive);
}
