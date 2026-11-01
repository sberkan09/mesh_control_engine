// Extract feed of the bird flocks (M-BIRDFLOCK 26b): Kit3DExtract builds one local alpha-blended render::ParticleEmitter
// from flock_look and adds it to the scene (id 0xFFFF0005, in a box above the camera following it). No Flock, enabled 0,
// strength 0 and an amount of 0 (midday, night, rain, out of season) add nothing (the emitter list and the stats stay
// bit-identical); Flock, Season, WorldClock, Weather and every ParticleEmitter3D are never written. The sun height comes
// from the day clock, or from the environment's fixed sun_direction when time_of_day < 0.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
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

constexpr u32 kFlockId = 0xFFFF0005u;
constexpr u32 kDustId = 0xFFFF0004u;
constexpr u32 kPollenId = 0xFFFF0003u;
constexpr u32 kFireflyId = 0xFFFF0002u;
constexpr u32 kLeafId = 0xFFFF0001u;
constexpr f32 kNoon = 12.0f;
constexpr f32 kSunrise = 6.0f;  // sun_direction_at(6).y == 0: the horizon, the twilight gate is fully open

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
    Entity fl_e, season_e, clock_e, weather_e;

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
    void add_flock(Flock f = {}) { fl_e = world.spawn_with(SaveKey{2}, f); }
    Flock& flock() { return *world.try_get_mut<Flock>(fl_e); }
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
    void add_rain(f32 intensity) {
        Weather w;
        w.current = WeatherKind::Rain;
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

// A Season at the spring centre on day 0, the sun on the horizon.
void spring_dawn(Rig& r) {
    r.add_season(0.125f);
    r.add_clock(0);
    r.set_time(kSunrise);
}

const render::ParticleEmitter* flock_of(const Shot& f) {
    for (const render::ParticleEmitter& e : f.emitters)
        if (e.id == kFlockId) return &e;
    return nullptr;
}

}  // namespace

TEST_CASE("flock extract: no Flock adds nothing") {
    Rig base;
    spring_dawn(base);
    const Shot a = base.frame();
    CHECK(a.emitters.size() == 0);
    CHECK(a.stat == 0);
}

TEST_CASE("flock extract: midday, night, off-season, enabled 0 and strength 0 add nothing") {
    Rig noon;  // the twilight gate is closed by day
    noon.add_flock();
    noon.add_season(0.125f);
    noon.add_clock(0);
    noon.set_time(kNoon);
    CHECK(noon.frame().emitters.size() == 0);
    CHECK(noon.extract.stats().particle_emitters == 0);

    Rig night;  // and at midnight
    night.add_flock();
    night.add_season(0.125f);
    night.add_clock(0);
    night.set_time(0.0f);
    CHECK(night.frame().emitters.size() == 0);

    Rig summer;  // phase 0.375 is between both migrations
    summer.add_flock();
    summer.add_season(0.375f);
    summer.add_clock(0);
    summer.set_time(kSunrise);
    CHECK(summer.frame().emitters.size() == 0);

    Rig off;
    off.add_flock();
    spring_dawn(off);
    off.flock().enabled = 0;
    CHECK(off.frame().emitters.size() == 0);

    Rig zero;
    zero.add_flock();
    spring_dawn(zero);
    zero.flock().strength = 0.0f;
    CHECK(zero.frame().emitters.size() == 0);
}

TEST_CASE("flock extract: twilight in a migration season adds exactly one alpha-blended emitter with the helper's look") {
    Rig r;
    r.add_flock();
    spring_dawn(r);
    const Shot f = r.frame();
    REQUIRE(f.emitters.size() == 1);
    CHECK(f.stat == 1);
    const render::ParticleEmitter& e = f.emitters[0];
    CHECK(e.id == kFlockId);

    const f32 amount = flock_amount(r.flock(), r.world.try_get<Season>(r.season_e), 0.125f, 0.0f, 0.0f);
    CHECK(amount == doctest::Approx(1.0f));
    const FlockLook l = flock_look(r.flock(), amount);
    CHECK(e.rate == doctest::Approx(l.rate));
    CHECK(e.rate == doctest::Approx(3.0f));
    CHECK(e.speed_min == doctest::Approx(l.speed_min));
    CHECK(e.speed_max == doctest::Approx(l.speed_max));
    CHECK(e.direction.x == doctest::Approx(l.direction.x));
    CHECK(e.direction.y == doctest::Approx(0.0f));
    CHECK(e.direction.z == doctest::Approx(l.direction.z));
    CHECK(e.spread == doctest::Approx(l.spread));
    CHECK(e.gravity.y == doctest::Approx(0.0f));
    CHECK(e.drag == doctest::Approx(l.drag));
    CHECK(e.size_start == doctest::Approx(l.size));
    CHECK(e.size_end == doctest::Approx(l.size));
    CHECK(e.lifetime_min == doctest::Approx(l.lifetime_min));
    CHECK(e.lifetime_max == doctest::Approx(l.lifetime_max));
    CHECK(e.color_start.x == doctest::Approx(l.color_start.x));
    CHECK(e.color_start.w == doctest::Approx(0.85f));
    CHECK(e.color_end.w == 0.0f);
    CHECK(e.box.x == doctest::Approx(60.0f));
    CHECK(e.box.y == 0.0f);
    CHECK(e.box.z == doctest::Approx(60.0f));
    CHECK(e.max_particles == 64u);
    CHECK(e.seed == 1u);
    CHECK_FALSE(e.additive);
    CHECK_FALSE(e.collide);
    CHECK(e.emitting);
    CHECK(e.burst == 0u);

    // authoring flows through; max_particles is clamped to 16..1024
    r.flock().max_particles = 8;
    CHECK(flock_of(r.frame())->max_particles == 16u);
    r.flock().max_particles = 100000;  // above the component Range; the extract clamps what it reads
    CHECK(flock_of(r.frame())->max_particles == 1024u);
    r.flock().seed = 77;
    r.flock().strength = 0.5f;
    const Shot g = r.frame();
    REQUIRE(g.emitters.size() == 1);
    CHECK(g.emitters[0].rate == doctest::Approx(1.5f));
    CHECK(g.emitters[0].seed == 77u);

    Rig manual;  // the manual floor works without a Season
    manual.add_flock();
    manual.flock().manual = 0.5f;
    manual.set_time(kSunrise);
    const Shot m = manual.frame();
    REQUIRE(m.emitters.size() == 1);
    CHECK(flock_of(m)->rate == doctest::Approx(1.5f));
}

TEST_CASE("flock extract: no clock (time_of_day < 0) reads the environment's sun_direction") {
    Rig day;  // the default sun_direction is high: the gate is closed
    day.add_flock();
    day.add_season(0.125f);
    CHECK(day.world.resource<Environment3D>().time_of_day < 0.0f);
    CHECK(day.frame().emitters.size() == 0);

    Rig horizon;  // a sun on the horizon opens it, whichever way the vector points
    horizon.add_flock();
    horizon.add_season(0.125f);
    horizon.world.resource<Environment3D>().sun_direction = math::vec3{-1.0f, 0.0f, 0.0f};
    const Shot f = horizon.frame();
    REQUIRE(f.emitters.size() == 1);
    CHECK(flock_of(f)->rate == doctest::Approx(3.0f));
}

TEST_CASE("flock extract: rain closes the flock, only Rain and Storm count") {
    Rig rain;
    rain.add_flock();
    spring_dawn(rain);
    rain.add_rain(1.0f);
    CHECK(rain.frame().emitters.size() == 0);

    Rig light;  // half rain halves the amount
    light.add_flock();
    spring_dawn(light);
    light.add_rain(0.5f);
    const Shot fl = light.frame();
    REQUIRE(fl.emitters.size() == 1);
    CHECK(flock_of(fl)->rate == doctest::Approx(1.5f));

    Rig snow;  // another kind with an intensity is not rain
    snow.add_flock();
    spring_dawn(snow);
    snow.add_rain(1.0f);
    Weather* w = snow.world.try_get_mut<Weather>(snow.weather_e);
    REQUIRE(w != nullptr);
    w->current = WeatherKind::Snow;
    CHECK(snow.frame().emitters.size() == 1);
}

TEST_CASE("flock extract: the id never collides and the emitter follows the camera") {
    Rig r;
    r.add_flock();
    spring_dawn(r);
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
        if (e.id == kFlockId) {
            ++count;
            continue;
        }
        bool known = false;
        for (const Entity s : spawned) known = known || e.id == s.index + 1;
        CHECK(known);
    }
    CHECK(count == 1);
    CHECK(kFlockId != kDustId);
    CHECK(kFlockId != kPollenId);
    CHECK(kFlockId != kFireflyId);
    CHECK(kFlockId != kLeafId);
    for (const Entity s : spawned) CHECK(s.index + 1 != kFlockId);

    const render::ParticleEmitter* e0 = flock_of(f);
    REQUIRE(e0 != nullptr);
    CHECK(e0->position.x == doctest::Approx(r.cam.position.x));
    CHECK(e0->position.y == doctest::Approx(r.cam.position.y + 45.0f));
    CHECK(e0->position.z == doctest::Approx(r.cam.position.z));

    r.cam.position = {30.0f, 4.0f, -12.0f};
    r.flock().height_m = 20.0f;
    const render::ParticleEmitter* e1 = flock_of(r.frame());
    REQUIRE(e1 != nullptr);
    CHECK(e1->position.x == doctest::Approx(30.0f));
    CHECK(e1->position.y == doctest::Approx(24.0f));
    CHECK(e1->position.z == doctest::Approx(-12.0f));
}

TEST_CASE("flock extract: the heading turns the stream on the ground plane") {
    Rig east;  // heading 90 is +x
    east.add_flock();
    east.flock().heading_deg = 90.0f;
    spring_dawn(east);
    const render::ParticleEmitter* e = flock_of(east.frame());
    REQUIRE(e != nullptr);
    CHECK(e->direction.x > 0.99f);
    CHECK(std::abs(e->direction.z) < 0.01f);
    CHECK(e->direction.y == 0.0f);

    Rig north;  // heading 0 is +z
    north.add_flock();
    north.flock().heading_deg = 0.0f;
    spring_dawn(north);
    const render::ParticleEmitter* n = flock_of(north.frame());
    REQUIRE(n != nullptr);
    CHECK(n->direction.z > 0.99f);
    CHECK(std::abs(n->direction.x) < 0.01f);
}

TEST_CASE("flock extract: the Low tier halves the rate and the budget") {
    Rig r;
    r.add_flock();
    spring_dawn(r);
    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    const Shot high = r.frame();
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    const Shot low = r.frame();
    REQUIRE(flock_of(high) != nullptr);
    REQUIRE(flock_of(low) != nullptr);
    CHECK(flock_of(low)->rate == doctest::Approx(0.5f * flock_of(high)->rate));
    CHECK(flock_of(low)->max_particles == flock_of(high)->max_particles / 2u);
    CHECK(low.stat == 1);
}
