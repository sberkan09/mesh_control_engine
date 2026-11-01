// Extract feed of the summer butterflies (M-BUTTERFLY 27b): Kit3DExtract builds one local alpha-blended
// render::ParticleEmitter from butterfly_look and adds it to the scene (id 0xFFFF0006, in a low box centred on the
// camera). No Butterfly, enabled 0, strength 0 and an amount of 0 (winter, night, strong wind, rain) add nothing (the
// emitter list and the stats stay bit-identical); Butterfly, Season, WorldClock, Weather and every ParticleEmitter3D are
// never written.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/butterfly.h"
#include "mce/kit/dust.h"
#include "mce/kit/firefly.h"
#include "mce/kit/flock.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/leaf_fall.h"
#include "mce/kit/pollen.h"
#include "mce/kit/season.h"
#include "mce/kit/weather.h"
#include "mce/kit/world_clock.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr u32 kButterflyId = 0xFFFF0006u;
constexpr u32 kFlockId = 0xFFFF0005u;
constexpr u32 kDustId = 0xFFFF0004u;
constexpr u32 kPollenId = 0xFFFF0003u;
constexpr u32 kFireflyId = 0xFFFF0002u;
constexpr u32 kLeafId = 0xFFFF0001u;
constexpr f32 kNoon = 12.0f;

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
    Entity bf_e, season_e, clock_e, weather_e;

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
    void add_butterfly(Butterfly b = {}) { bf_e = world.spawn_with(SaveKey{2}, b); }
    Butterfly& butterfly() { return *world.try_get_mut<Butterfly>(bf_e); }
    void add_season(f32 phase) {
        Season s;
        s.start_phase = phase;
        season_e = world.spawn_with(SaveKey{3}, s);
    }
    void add_clock(u32 day, f32 hour = 0.0f) {
        WorldClock c;
        c.day = day;
        c.hour = hour;
        clock_e = world.spawn_with(SaveKey{4}, c);
    }
    void add_weather(WeatherKind kind, f32 wind, f32 intensity) {
        Weather w;
        w.current = kind;
        w.wind = wind;
        w.intensity = intensity;
        weather_e = world.spawn_with(SaveKey{5}, w);
    }
    void set_time(f32 hours) { world.resource<Environment3D>().time_of_day = hours; }
    Shot frame() {
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
        Shot f;
        for (const render::ParticleEmitter& e : scene.particle_emitters()) f.emitters.push_back(e);
        f.stat = extract.stats().particle_emitters;
        return f;
    }
};

// A Season at the summer centre on day 0, midday, a calm dry sky.
void summer_day(Rig& r) {
    r.add_season(0.375f);
    r.add_clock(0);
    r.set_time(kNoon);
    r.add_weather(WeatherKind::Clear, 0.1f, 0.0f);
}

const render::ParticleEmitter* butterfly_of(const Shot& f) {
    for (const render::ParticleEmitter& e : f.emitters)
        if (e.id == kButterflyId) return &e;
    return nullptr;
}

}  // namespace

TEST_CASE("butterfly extract: no Butterfly adds nothing") {
    Rig base;
    summer_day(base);
    const Shot a = base.frame();
    CHECK(a.emitters.size() == 0);
    CHECK(a.stat == 0);
}

TEST_CASE("butterfly extract: enabled 0 and strength 0 add nothing") {
    Rig off;
    off.add_butterfly();
    summer_day(off);
    off.butterfly().enabled = 0;
    CHECK(off.frame().emitters.size() == 0);
    CHECK(off.extract.stats().particle_emitters == 0);

    Rig zero;
    zero.add_butterfly();
    summer_day(zero);
    zero.butterfly().strength = 0.0f;
    CHECK(zero.frame().emitters.size() == 0);
}

TEST_CASE("butterfly extract: a calm summer day adds exactly one alpha-blended emitter with the helper's look") {
    Rig r;
    r.add_butterfly();
    summer_day(r);
    const Shot f = r.frame();
    REQUIRE(f.emitters.size() == 1);
    CHECK(f.stat == 1);
    const render::ParticleEmitter& e = f.emitters[0];
    CHECK(e.id == kButterflyId);

    const f32 amount = butterfly_amount(r.butterfly(), r.world.try_get<Season>(r.season_e), 0.375f, 0.0f, 0.1f, 0.0f);
    CHECK(amount == doctest::Approx(1.0f));
    const ButterflyLook l = butterfly_look(r.butterfly(), amount);
    CHECK(e.rate == doctest::Approx(l.rate));
    CHECK(e.rate == doctest::Approx(5.0f));
    CHECK(e.speed_min == doctest::Approx(l.speed_min));
    CHECK(e.speed_max == doctest::Approx(l.speed_max));
    CHECK(e.direction.y == doctest::Approx(1.0f));
    CHECK(e.spread == doctest::Approx(l.spread));
    CHECK(e.gravity.y == doctest::Approx(0.2f));
    CHECK(e.drag == doctest::Approx(l.drag));
    CHECK(e.size_start == doctest::Approx(0.12f));
    CHECK(e.size_end == doctest::Approx(0.12f));
    CHECK(e.lifetime_min == doctest::Approx(l.lifetime_min));
    CHECK(e.lifetime_max == doctest::Approx(l.lifetime_max));
    CHECK(e.color_start.x == doctest::Approx(1.0f));
    CHECK(e.color_start.y == doctest::Approx(0.75f));
    CHECK(e.color_start.w == doctest::Approx(0.9f));
    CHECK(e.color_end.w == 0.0f);
    CHECK(e.box.x == doctest::Approx(18.0f));
    CHECK(e.box.y == doctest::Approx(3.0f));
    CHECK(e.box.z == doctest::Approx(18.0f));
    CHECK(e.position.x == doctest::Approx(r.cam.position.x));
    CHECK(e.position.z == doctest::Approx(r.cam.position.z));
    CHECK(e.max_particles == 48u);
    CHECK(e.seed == 1u);
    CHECK_FALSE(e.additive);
    CHECK_FALSE(e.collide);
    CHECK(e.emitting);
    CHECK(e.burst == 0u);

    // authoring flows through; max_particles is clamped to 8..512
    r.butterfly().max_particles = 1;
    CHECK(butterfly_of(r.frame())->max_particles == 8u);
    r.butterfly().max_particles = 100000;  // above the component Range; the extract clamps what it reads
    CHECK(butterfly_of(r.frame())->max_particles == 512u);
    r.butterfly().seed = 77;
    r.butterfly().strength = 0.5f;
    const Shot g = r.frame();
    REQUIRE(g.emitters.size() == 1);
    CHECK(g.emitters[0].rate == doctest::Approx(2.5f));
    CHECK(g.emitters[0].seed == 77u);

    // the emitter follows the camera
    r.cam.position = {30.0f, 4.0f, -12.0f};
    const render::ParticleEmitter* e1 = butterfly_of(r.frame());
    REQUIRE(e1 != nullptr);
    CHECK(e1->position.x == doctest::Approx(30.0f));
    CHECK(e1->position.z == doctest::Approx(-12.0f));

    Rig manual;  // the manual floor works without a Season, a clock or a Weather
    manual.add_butterfly();
    manual.butterfly().manual = 0.5f;
    manual.set_time(kNoon);
    const Shot m = manual.frame();
    REQUIRE(m.emitters.size() == 1);
    CHECK(butterfly_of(m)->rate == doctest::Approx(2.5f));
}

TEST_CASE("butterfly extract: winter and night add nothing") {
    Rig winter;  // phase 0.875 is far outside the summer window
    winter.add_butterfly();
    winter.add_season(0.875f);
    winter.add_clock(0);
    winter.set_time(kNoon);
    winter.add_weather(WeatherKind::Clear, 0.1f, 0.0f);
    CHECK(winter.frame().emitters.size() == 0);

    Rig night;  // summer, but the sun is down
    night.add_butterfly();
    summer_day(night);
    night.set_time(0.0f);
    CHECK(night.frame().emitters.size() == 0);
}

TEST_CASE("butterfly extract: a strong wind closes the butterflies, a light one thins them") {
    Rig storm;
    storm.add_butterfly();
    storm.add_season(0.375f);
    storm.add_clock(0);
    storm.set_time(kNoon);
    storm.add_weather(WeatherKind::Clear, 1.0f, 0.0f);
    CHECK(storm.frame().emitters.size() == 0);

    Rig breeze;  // between wind_calm and wind_still
    breeze.add_butterfly();
    breeze.add_season(0.375f);
    breeze.add_clock(0);
    breeze.set_time(kNoon);
    breeze.add_weather(WeatherKind::Clear, 0.5f, 0.0f);
    const Shot b = breeze.frame();
    REQUIRE(b.emitters.size() == 1);
    CHECK(butterfly_of(b)->rate < 5.0f);
    CHECK(butterfly_of(b)->rate > 0.0f);
}

TEST_CASE("butterfly extract: rain closes the butterflies, only Rain and Storm count") {
    Rig rain;
    rain.add_butterfly();
    rain.add_season(0.375f);
    rain.add_clock(0);
    rain.set_time(kNoon);
    rain.add_weather(WeatherKind::Rain, 0.1f, 1.0f);
    CHECK(rain.frame().emitters.size() == 0);

    Rig light;  // half rain halves the amount
    light.add_butterfly();
    light.add_season(0.375f);
    light.add_clock(0);
    light.set_time(kNoon);
    light.add_weather(WeatherKind::Rain, 0.1f, 0.5f);
    const Shot fl = light.frame();
    REQUIRE(fl.emitters.size() == 1);
    CHECK(butterfly_of(fl)->rate == doctest::Approx(2.5f));

    Rig snow;  // another kind with an intensity is not rain
    snow.add_butterfly();
    snow.add_season(0.375f);
    snow.add_clock(0);
    snow.set_time(kNoon);
    snow.add_weather(WeatherKind::Snow, 0.1f, 1.0f);
    CHECK(snow.frame().emitters.size() == 1);
}

TEST_CASE("butterfly extract: the id never collides with other emitters") {
    Rig r;
    r.add_butterfly();
    summer_day(r);
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
        if (e.id == kButterflyId) {
            ++count;
            continue;
        }
        bool known = false;
        for (const Entity s : spawned) known = known || e.id == s.index + 1;
        CHECK(known);
    }
    CHECK(count == 1);
    CHECK(kButterflyId != kFlockId);
    CHECK(kButterflyId != kDustId);
    CHECK(kButterflyId != kPollenId);
    CHECK(kButterflyId != kFireflyId);
    CHECK(kButterflyId != kLeafId);
    for (const Entity s : spawned) CHECK(s.index + 1 != kButterflyId);
}

TEST_CASE("butterfly extract: the Low tier halves the rate and the budget") {
    Rig r;
    r.add_butterfly();
    summer_day(r);
    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    const Shot high = r.frame();
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    const Shot low = r.frame();
    REQUIRE(butterfly_of(high) != nullptr);
    REQUIRE(butterfly_of(low) != nullptr);
    CHECK(butterfly_of(low)->rate == doctest::Approx(0.5f * butterfly_of(high)->rate));
    CHECK(butterfly_of(low)->max_particles == butterfly_of(high)->max_particles / 2u);
    CHECK(low.stat == 1);
}
