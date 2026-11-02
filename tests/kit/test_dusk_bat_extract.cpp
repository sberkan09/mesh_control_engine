// Extract feed of the dusk bats (M-BAT 30b): Kit3DExtract builds one local alpha-blended
// render::ParticleEmitter from dusk_bat_look and adds it to the scene (id 0xFFFF0007, in a box centred on the
// camera). No DuskBat, enabled 0, strength 0 and an amount of 0 (winter, midday, deep night, strong wind, rain) add nothing (the
// emitter list and the stats stay bit-identical); DuskBat, Season, WorldClock, Weather and every ParticleEmitter3D are
// never written.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/butterfly.h"
#include "mce/kit/dusk_bat.h"
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

constexpr u32 kDuskBatId = 0xFFFF0007u;
constexpr u32 kButterflyId = 0xFFFF0006u;
constexpr u32 kFlockId = 0xFFFF0005u;
constexpr u32 kDustId = 0xFFFF0004u;
constexpr u32 kPollenId = 0xFFFF0003u;
constexpr u32 kFireflyId = 0xFFFF0002u;
constexpr u32 kLeafId = 0xFFFF0001u;
constexpr f32 kNoon = 12.0f;
// 18:00 puts the sun on the horizon: elevation 0, so the night fade is 0.5, inside the dusk band.
constexpr f32 kDusk = 18.0f;

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
    Entity bat_e, season_e, clock_e, weather_e;

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
    void add_bat(DuskBat b = {}) { bat_e = world.spawn_with(SaveKey{2}, b); }
    DuskBat& bat() { return *world.try_get_mut<DuskBat>(bat_e); }
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

// A Season at the summer centre on day 0, dusk, a calm dry sky.
void summer_evening(Rig& r) {
    r.add_season(0.375f);
    r.add_clock(0);
    r.set_time(kDusk);
    r.add_weather(WeatherKind::Clear, 0.1f, 0.0f);
}

const render::ParticleEmitter* bat_of(const Shot& f) {
    for (const render::ParticleEmitter& e : f.emitters)
        if (e.id == kDuskBatId) return &e;
    return nullptr;
}

}  // namespace

TEST_CASE("dusk bat extract: no DuskBat adds nothing") {
    Rig base;
    summer_evening(base);
    const Shot a = base.frame();
    CHECK(a.emitters.size() == 0);
    CHECK(a.stat == 0);
}

TEST_CASE("dusk bat extract: enabled 0 and strength 0 add nothing") {
    Rig off;
    off.add_bat();
    summer_evening(off);
    off.bat().enabled = 0;
    CHECK(off.frame().emitters.size() == 0);
    CHECK(off.extract.stats().particle_emitters == 0);

    Rig zero;
    zero.add_bat();
    summer_evening(zero);
    zero.bat().strength = 0.0f;
    CHECK(zero.frame().emitters.size() == 0);
}

TEST_CASE("dusk bat extract: a summer dusk adds exactly one alpha-blended emitter with the helper's look") {
    Rig r;
    r.add_bat();
    summer_evening(r);
    const Shot f = r.frame();
    REQUIRE(f.emitters.size() == 1);
    CHECK(f.stat == 1);
    const render::ParticleEmitter& e = f.emitters[0];
    CHECK(e.id == kDuskBatId);

    const f32 amount = dusk_bat_amount(r.bat(), r.world.try_get<Season>(r.season_e), 0.375f, 0.5f, 0.1f, 0.0f);
    CHECK(amount == doctest::Approx(1.0f));
    const DuskBatLook l = dusk_bat_look(r.bat(), amount);
    CHECK(e.rate == doctest::Approx(l.rate));
    CHECK(e.rate == doctest::Approx(4.0f));
    CHECK(e.speed_min == doctest::Approx(l.speed_min));
    CHECK(e.speed_max == doctest::Approx(l.speed_max));
    CHECK(e.direction.y == doctest::Approx(1.0f));
    CHECK(e.spread == doctest::Approx(l.spread));
    CHECK(e.gravity.y == doctest::Approx(-0.2f));
    CHECK(e.drag == doctest::Approx(l.drag));
    CHECK(e.size_start == doctest::Approx(0.1f));
    CHECK(e.size_end == doctest::Approx(0.1f));
    CHECK(e.lifetime_min == doctest::Approx(l.lifetime_min));
    CHECK(e.lifetime_max == doctest::Approx(l.lifetime_max));
    CHECK(e.color_start.x == doctest::Approx(0.08f));
    CHECK(e.color_start.y == doctest::Approx(0.07f));
    CHECK(e.color_start.w == doctest::Approx(0.85f));
    CHECK(e.color_end.w == 0.0f);
    CHECK(e.box.x == doctest::Approx(24.0f));
    CHECK(e.box.y == doctest::Approx(6.0f));
    CHECK(e.box.z == doctest::Approx(24.0f));
    CHECK(e.position.x == doctest::Approx(r.cam.position.x));
    CHECK(e.position.z == doctest::Approx(r.cam.position.z));
    CHECK(e.max_particles == 32u);
    CHECK(e.seed == 1u);
    CHECK_FALSE(e.additive);
    CHECK_FALSE(e.collide);
    CHECK(e.emitting);
    CHECK(e.burst == 0u);

    // authoring flows through; max_particles is clamped to 8..512
    r.bat().max_particles = 1;
    CHECK(bat_of(r.frame())->max_particles == 8u);
    r.bat().max_particles = 100000;  // above the component Range; the extract clamps what it reads
    CHECK(bat_of(r.frame())->max_particles == 512u);
    r.bat().seed = 77;
    r.bat().strength = 0.5f;
    const Shot g = r.frame();
    REQUIRE(g.emitters.size() == 1);
    CHECK(g.emitters[0].rate == doctest::Approx(2.0f));
    CHECK(g.emitters[0].seed == 77u);

    // the emitter follows the camera
    r.cam.position = {30.0f, 4.0f, -12.0f};
    const render::ParticleEmitter* e1 = bat_of(r.frame());
    REQUIRE(e1 != nullptr);
    CHECK(e1->position.x == doctest::Approx(30.0f));
    CHECK(e1->position.z == doctest::Approx(-12.0f));

    Rig manual;  // the manual floor works without a Season, a clock or a Weather
    manual.add_bat();
    manual.bat().manual = 0.5f;
    manual.set_time(kDusk);
    const Shot m = manual.frame();
    REQUIRE(m.emitters.size() == 1);
    CHECK(bat_of(m)->rate == doctest::Approx(2.0f));
}

TEST_CASE("dusk bat extract: midday and deep night add nothing") {
    Rig day;  // night fade 0: too bright
    day.add_bat();
    summer_evening(day);
    day.set_time(kNoon);
    CHECK(day.frame().emitters.size() == 0);

    Rig night;  // night fade 1: too dark
    night.add_bat();
    summer_evening(night);
    night.set_time(0.0f);
    CHECK(night.frame().emitters.size() == 0);

    Rig winter;  // phase 0.875 is far outside the warm window
    winter.add_bat();
    winter.add_season(0.875f);
    winter.add_clock(0);
    winter.set_time(kDusk);
    winter.add_weather(WeatherKind::Clear, 0.1f, 0.0f);
    CHECK(winter.frame().emitters.size() == 0);
}

TEST_CASE("dusk bat extract: strong wind and rain add none") {
    Rig storm;
    storm.add_bat();
    storm.add_season(0.375f);
    storm.add_clock(0);
    storm.set_time(kDusk);
    storm.add_weather(WeatherKind::Clear, 1.0f, 0.0f);
    CHECK(storm.frame().emitters.size() == 0);

    Rig rain;
    rain.add_bat();
    rain.add_season(0.375f);
    rain.add_clock(0);
    rain.set_time(kDusk);
    rain.add_weather(WeatherKind::Rain, 0.1f, 1.0f);
    CHECK(rain.frame().emitters.size() == 0);

    Rig light;  // half rain halves the amount
    light.add_bat();
    light.add_season(0.375f);
    light.add_clock(0);
    light.set_time(kDusk);
    light.add_weather(WeatherKind::Rain, 0.1f, 0.5f);
    const Shot fl = light.frame();
    REQUIRE(fl.emitters.size() == 1);
    CHECK(bat_of(fl)->rate == doctest::Approx(2.0f));
}

TEST_CASE("dusk bat extract: nothing is written") {
    Rig r;
    r.add_bat();
    summer_evening(r);
    const DuskBat bat0 = r.bat();
    const Season season0 = *r.world.try_get<Season>(r.season_e);
    const WorldClock clock0 = *r.world.try_get<WorldClock>(r.clock_e);
    const Weather weather0 = *r.world.try_get<Weather>(r.weather_e);
    for (u32 i = 0; i < 3; ++i) REQUIRE(r.frame().emitters.size() == 1);
    const DuskBat* bat1 = r.world.try_get<DuskBat>(r.bat_e);
    REQUIRE(bat1 != nullptr);
    CHECK(bat1->strength == bat0.strength);
    CHECK(bat1->manual == bat0.manual);
    CHECK(bat1->enabled == bat0.enabled);
    CHECK(bat1->max_particles == bat0.max_particles);
    CHECK(bat1->seed == bat0.seed);
    CHECK(r.world.try_get<Season>(r.season_e)->start_phase == season0.start_phase);
    CHECK(r.world.try_get<WorldClock>(r.clock_e)->day == clock0.day);
    CHECK(r.world.try_get<WorldClock>(r.clock_e)->hour == clock0.hour);
    CHECK(r.world.try_get<Weather>(r.weather_e)->wind == weather0.wind);
    CHECK(r.world.try_get<Weather>(r.weather_e)->intensity == weather0.intensity);
    CHECK(r.world.try_get<Weather>(r.weather_e)->current == weather0.current);
}

TEST_CASE("dusk bat extract: the id never collides with other emitters") {
    Rig r;
    r.add_bat();
    summer_evening(r);
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
        if (e.id == kDuskBatId) {
            ++count;
            continue;
        }
        bool known = false;
        for (const Entity s : spawned) known = known || e.id == s.index + 1;
        CHECK(known);
    }
    CHECK(count == 1);
    CHECK(kDuskBatId != kButterflyId);
    CHECK(kDuskBatId != kFlockId);
    CHECK(kDuskBatId != kDustId);
    CHECK(kDuskBatId != kPollenId);
    CHECK(kDuskBatId != kFireflyId);
    CHECK(kDuskBatId != kLeafId);
    for (const Entity s : spawned) CHECK(s.index + 1 != kDuskBatId);
}

TEST_CASE("dusk bat extract: the Low tier halves the rate and the budget") {
    Rig r;
    r.add_bat();
    summer_evening(r);
    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    const Shot high = r.frame();
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    const Shot low = r.frame();
    REQUIRE(bat_of(high) != nullptr);
    REQUIRE(bat_of(low) != nullptr);
    CHECK(bat_of(low)->rate == doctest::Approx(0.5f * bat_of(high)->rate));
    CHECK(bat_of(low)->max_particles == bat_of(high)->max_particles / 2u);
    CHECK(low.stat == 1);
}

TEST_CASE("dusk bat extract: a hand-set particle_scale on a Medium tier scales the rate and the budget, 0 adds none") {
    Rig r;
    r.add_bat();
    summer_evening(r);
    render::QualitySettings q = render::quality_settings(render::QualityTier::Medium);
    r.extract.set_quality(q);
    const Shot full = r.frame();
    REQUIRE(bat_of(full) != nullptr);
    q.particle_scale = 0.25f;
    r.extract.set_quality(q);
    const Shot quarter = r.frame();
    REQUIRE(bat_of(quarter) != nullptr);
    CHECK(bat_of(quarter)->rate == doctest::Approx(0.25f * bat_of(full)->rate));
    CHECK(bat_of(quarter)->max_particles == bat_of(full)->max_particles / 4u);
    q.particle_scale = 0.0f;
    r.extract.set_quality(q);
    const Shot none = r.frame();
    CHECK(bat_of(none) == nullptr);
}
