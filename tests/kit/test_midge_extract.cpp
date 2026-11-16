// Extract feed of the midges (M-MIDGES 33b): Kit3DExtract builds one local alpha-blended render::ParticleEmitter from
// midge_look and adds it to the scene (id 0xFFFF0009, in a box around the camera whose floor sits at the assumed ground).
// No Midge, enabled 0, strength 0 and an amount of 0 (outside the warm-season window or the twilight, wind, rain, no manual
// floor) add nothing (the emitter list and the stats stay bit-identical); Midge, Season, WorldClock, Weather and every
// ParticleEmitter3D are never written.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/midge.h"
#include "mce/kit/season.h"
#include "mce/kit/weather.h"
#include "mce/kit/world_clock.h"
#include "mce/render/atmosphere.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr u32 kMidgeId = 0xFFFF0009u;
constexpr u32 kSnowSparkleId = 0xFFFF0008u;
constexpr f32 kNoon = 12.0f;
constexpr f32 kMidnight = 0.0f;

// The evening hour (12..24) at which the sun elevation (the sine of the angle) is closest to `target`.
f32 evening_hour(f32 target) {
    f32 best = 12.0f;
    f32 best_d = 1e9f;
    for (f32 h = 12.0f; h <= 24.0f; h += 0.01f) {
        const f32 d = std::fabs(-render::sun_direction_at(h).y - target);
        if (d < best_d) {
            best_d = d;
            best = h;
        }
    }
    return best;
}

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
    Entity midge_e, season_e, clock_e, weather_e;

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
    void add_midge(Midge m = {}) { midge_e = world.spawn_with(SaveKey{2}, m); }
    Midge& midge() { return *world.try_get_mut<Midge>(midge_e); }
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

// A Season at the summer centre on day 0, a clock, the sun at the horizon, a calm dry sky.
void summer_dusk(Rig& r) {
    r.add_season(0.375f);
    r.add_clock(0);
    r.set_time(evening_hour(0.0f));
    r.add_weather(WeatherKind::Clear, 0.1f, 0.0f);
}

const render::ParticleEmitter* midge_of(const Shot& f) {
    for (const render::ParticleEmitter& e : f.emitters)
        if (e.id == kMidgeId) return &e;
    return nullptr;
}

}  // namespace

TEST_CASE("midge extract: no Midge adds nothing and leaves other emitters unchanged") {
    Rig base;
    summer_dusk(base);
    const Shot a = base.frame();
    CHECK(a.emitters.size() == 0);
    CHECK(a.stat == 0);
    CHECK(midge_of(a) == nullptr);

    Rig other;  // a ParticleEmitter3D alone gives the same emitter as without a Midge elsewhere
    summer_dusk(other);
    ParticleEmitter3D p;
    other.world.spawn_with(LocalTransform{}, p);
    const Shot b = other.frame();
    REQUIRE(b.emitters.size() == 1);
    CHECK(b.emitters[0].id != kMidgeId);
    Rig with;
    summer_dusk(with);
    with.world.spawn_with(LocalTransform{}, p);
    with.add_midge();
    const Shot c = with.frame();
    REQUIRE(c.emitters.size() == 2);
    CHECK(c.emitters[0].id == b.emitters[0].id);
    CHECK(c.emitters[0].rate == b.emitters[0].rate);
    CHECK(c.emitters[0].max_particles == b.emitters[0].max_particles);
    CHECK(c.emitters[0].position.x == b.emitters[0].position.x);
    CHECK(c.emitters[0].position.y == b.emitters[0].position.y);
    CHECK(c.emitters[1].id == kMidgeId);
}

TEST_CASE("midge extract: enabled 0 and strength 0 add nothing") {
    Rig off;
    off.add_midge();
    summer_dusk(off);
    off.midge().enabled = 0;
    CHECK(off.frame().emitters.size() == 0);
    CHECK(off.extract.stats().particle_emitters == 0);

    Rig zero;
    zero.add_midge();
    summer_dusk(zero);
    zero.midge().strength = 0.0f;
    zero.midge().manual = 1.0f;
    CHECK(zero.frame().emitters.size() == 0);
}

TEST_CASE("midge extract: the summer dusk adds exactly one alpha-blended emitter with the helper's look") {
    Rig r;
    r.add_midge();
    summer_dusk(r);
    const f32 elev = -render::sun_direction_at(evening_hour(0.0f)).y;
    REQUIRE(std::fabs(elev) < 0.02f);
    const Shot f = r.frame();
    REQUIRE(f.emitters.size() == 1);
    CHECK(f.stat == 1);
    const render::ParticleEmitter& e = f.emitters[0];
    CHECK(e.id == kMidgeId);

    const f32 amount = midge_amount(r.midge(), r.world.try_get<Season>(r.season_e), 0.375f, elev, 0.1f, 0.0f);
    CHECK(amount > 0.9f);
    const MidgeLook l = midge_look(r.midge(), amount);
    REQUIRE(l.active);
    CHECK(e.rate == doctest::Approx(l.rate));
    CHECK(e.speed_min == doctest::Approx(l.speed_min));
    CHECK(e.speed_max == doctest::Approx(l.speed_max));
    CHECK(e.direction.y == doctest::Approx(1.0f));
    CHECK(e.spread == doctest::Approx(l.spread));
    CHECK(e.drag == doctest::Approx(l.drag));
    CHECK(e.size_start == doctest::Approx(l.size));
    CHECK(e.size_end == doctest::Approx(l.size));
    CHECK(e.lifetime_min == doctest::Approx(l.lifetime_min));
    CHECK(e.lifetime_max == doctest::Approx(l.lifetime_max));
    CHECK(e.color_start.w == doctest::Approx(l.color_start.w));
    CHECK(e.color_end.w == 0.0f);
    CHECK(e.box.x == doctest::Approx(8.0f));
    CHECK(e.box.y == doctest::Approx(2.5f));
    CHECK(e.box.z == doctest::Approx(8.0f));
    CHECK(e.position.x == doctest::Approx(r.cam.position.x));
    CHECK(e.position.z == doctest::Approx(r.cam.position.z));
    CHECK(e.position.y - e.box.y == doctest::Approx(r.cam.position.y - 1.6f));
    CHECK(e.max_particles == 192u);
    CHECK(e.seed == 1u);
    CHECK_FALSE(e.additive);
    CHECK_FALSE(e.collide);
    CHECK(e.emitting);
    CHECK(e.burst == 0u);

    // authoring flows through; max_particles is clamped to 8..512
    r.midge().max_particles = 1;
    CHECK(midge_of(r.frame())->max_particles == 8u);
    r.midge().max_particles = 100000;
    CHECK(midge_of(r.frame())->max_particles == 512u);
    r.midge().seed = 77;
    const Shot g = r.frame();
    REQUIRE(g.emitters.size() == 1);
    CHECK(g.emitters[0].seed == 77u);

    // the emitter follows the camera, the floor stays at camera - eye
    r.cam.position = {30.0f, 4.0f, -12.0f};
    const Shot e1_shot = r.frame();  // keep the frame alive: the emitter pointer points into it
    const render::ParticleEmitter* e1 = midge_of(e1_shot);
    REQUIRE(e1 != nullptr);
    CHECK(e1->position.x == doctest::Approx(30.0f));
    CHECK(e1->position.z == doctest::Approx(-12.0f));
    CHECK(e1->position.y - e1->box.y == doctest::Approx(4.0f - 1.6f));
}

TEST_CASE("midge extract: the manual floor shows midges all year without a Season or a clock") {
    Rig manual;
    manual.add_midge();
    manual.midge().manual = 0.5f;
    manual.set_time(kNoon);  // the manual floor ignores the window and the twilight gate
    const Shot m = manual.frame();
    REQUIRE(m.emitters.size() == 1);
    CHECK(midge_of(m)->rate == doctest::Approx(15.0f));
    CHECK(midge_of(m)->position.y - midge_of(m)->box.y == doctest::Approx(manual.cam.position.y - 1.6f));
    CHECK_FALSE(midge_of(m)->additive);

    Rig bare;  // no Season: no window, so without the floor nothing
    bare.add_midge();
    bare.set_time(evening_hour(0.0f));
    CHECK(bare.frame().emitters.size() == 0);
}

TEST_CASE("midge extract: the window and the twilight gate decide") {
    Rig winter;  // phase 0.875 is the middle of winter, far from the summer centre
    winter.add_midge();
    winter.add_season(0.875f);
    winter.add_clock(0);
    winter.set_time(evening_hour(0.0f));
    winter.add_weather(WeatherKind::Clear, 0.1f, 0.0f);
    CHECK(winter.frame().emitters.size() == 0);

    Rig noon;  // a high sun closes the twilight gate
    noon.add_midge();
    noon.add_season(0.375f);
    noon.add_clock(0);
    noon.set_time(kNoon);
    noon.add_weather(WeatherKind::Clear, 0.1f, 0.0f);
    CHECK(noon.frame().emitters.size() == 0);

    Rig night;  // deep night closes it too (the elevation is kept signed)
    night.add_midge();
    night.add_season(0.375f);
    night.add_clock(0);
    night.set_time(kMidnight);
    night.add_weather(WeatherKind::Clear, 0.1f, 0.0f);
    REQUIRE(-render::sun_direction_at(kMidnight).y < -0.12f);
    CHECK(night.frame().emitters.size() == 0);

    Rig noclock;  // time_of_day -1: sun elevation 0, the horizon, so the gate is open
    noclock.add_midge();
    noclock.add_season(0.375f);
    noclock.add_clock(0);
    noclock.add_weather(WeatherKind::Clear, 0.1f, 0.0f);
    CHECK(noclock.frame().emitters.size() == 1);

    Rig later;  // the clock day moves the phase out of the window (half a year on, year_days 12)
    later.add_midge();
    later.add_season(0.375f);
    later.add_clock(6);
    later.set_time(evening_hour(0.0f));
    later.add_weather(WeatherKind::Clear, 0.1f, 0.0f);
    CHECK(later.frame().emitters.size() == 0);
}

TEST_CASE("midge extract: wind and rain gate it, only Rain and Storm count") {
    Rig storm;
    storm.add_midge();
    storm.add_season(0.375f);
    storm.add_clock(0);
    storm.set_time(evening_hour(0.0f));
    storm.add_weather(WeatherKind::Clear, 1.0f, 0.0f);  // a gale: nobody flies
    CHECK(storm.frame().emitters.size() == 0);

    Rig breeze;  // between wind_calm and wind_still
    breeze.add_midge();
    breeze.add_season(0.375f);
    breeze.add_clock(0);
    breeze.set_time(evening_hour(0.0f));
    breeze.add_weather(WeatherKind::Clear, 0.4f, 0.0f);
    const Shot b = breeze.frame();
    REQUIRE(b.emitters.size() == 1);
    CHECK(midge_of(b)->rate > 0.0f);
    CHECK(midge_of(b)->rate < 30.0f);

    Rig rain;
    rain.add_midge();
    rain.add_season(0.375f);
    rain.add_clock(0);
    rain.set_time(evening_hour(0.0f));
    rain.add_weather(WeatherKind::Rain, 0.1f, 1.0f);
    CHECK(rain.frame().emitters.size() == 0);
    rain.world.try_get_mut<Weather>(rain.weather_e)->current = WeatherKind::Clear;
    CHECK(rain.frame().emitters.size() == 1);

    Rig snow;  // Snow is not rain
    snow.add_midge();
    snow.add_season(0.375f);
    snow.add_clock(0);
    snow.set_time(evening_hour(0.0f));
    snow.add_weather(WeatherKind::Snow, 0.1f, 1.0f);
    CHECK(snow.frame().emitters.size() == 1);

    Rig floor_rain;  // the manual floor does not survive rain
    floor_rain.add_midge();
    floor_rain.midge().manual = 1.0f;
    floor_rain.add_weather(WeatherKind::Storm, 0.1f, 1.0f);
    CHECK(floor_rain.frame().emitters.size() == 0);
}

TEST_CASE("midge extract: nothing is written") {
    Rig r;
    r.add_midge();
    summer_dusk(r);
    const Midge before = r.midge();
    const Season s_before = *r.world.try_get<Season>(r.season_e);
    (void)r.frame();
    (void)r.frame();
    const Midge& after = r.midge();
    CHECK(after.enabled == before.enabled);
    CHECK(after.strength == before.strength);
    CHECK(after.manual == before.manual);
    CHECK(after.rate == before.rate);
    CHECK(after.max_particles == before.max_particles);
    CHECK(r.world.try_get<Season>(r.season_e)->start_phase == s_before.start_phase);
    CHECK(r.world.try_get<WorldClock>(r.clock_e)->day == 0u);
    CHECK(r.world.try_get<Weather>(r.weather_e)->intensity == 0.0f);
}

TEST_CASE("midge extract: the id never collides with other emitters") {
    Rig r;
    r.add_midge();
    summer_dusk(r);
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
        if (e.id == kMidgeId) {
            ++count;
            continue;
        }
        bool known = false;
        for (const Entity s : spawned) known = known || e.id == s.index + 1;
        CHECK(known);
    }
    CHECK(count == 1);
    CHECK(kMidgeId != kSnowSparkleId);
    for (const Entity s : spawned) CHECK(s.index + 1 != kMidgeId);
}

TEST_CASE("midge extract: the Low tier halves the rate and the budget") {
    Rig r;
    r.add_midge();
    summer_dusk(r);
    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    const Shot high = r.frame();
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    const Shot low = r.frame();
    REQUIRE(midge_of(high) != nullptr);
    REQUIRE(midge_of(low) != nullptr);
    CHECK(midge_of(low)->rate == doctest::Approx(0.5f * midge_of(high)->rate));
    CHECK(midge_of(low)->max_particles == midge_of(high)->max_particles / 2u);
    CHECK(low.stat == 1);
}

TEST_CASE("midge extract: a particle_scale of 0 (r.particles 0) adds none, a fraction scales the rate and the budget") {
    Rig r;
    r.add_midge();
    summer_dusk(r);
    render::QualitySettings q = render::quality_settings(render::QualityTier::Medium);
    r.extract.set_quality(q);
    const Shot full = r.frame();
    REQUIRE(midge_of(full) != nullptr);
    q.particle_scale = 0.25f;
    r.extract.set_quality(q);
    const Shot quarter = r.frame();
    REQUIRE(midge_of(quarter) != nullptr);
    CHECK(midge_of(quarter)->rate == doctest::Approx(0.25f * midge_of(full)->rate));
    CHECK(midge_of(quarter)->max_particles == midge_of(full)->max_particles / 4u);
    q.particle_scale = 0.0f;
    r.extract.set_quality(q);
    const Shot none = r.frame();
    CHECK(midge_of(none) == nullptr);
    CHECK(none.stat == 0);
}

TEST_CASE("midge extract: two extracts of the same scene are equal") {
    Rig r;
    r.add_midge();
    summer_dusk(r);
    const Shot a = r.frame();
    const Shot b = r.frame();
    REQUIRE(a.emitters.size() == 1);
    REQUIRE(b.emitters.size() == 1);
    CHECK(a.stat == b.stat);
    const render::ParticleEmitter& x = a.emitters[0];
    const render::ParticleEmitter& y = b.emitters[0];
    CHECK(x.id == y.id);
    CHECK(x.rate == y.rate);
    CHECK(x.max_particles == y.max_particles);
    CHECK(x.seed == y.seed);
    CHECK(x.position.x == y.position.x);
    CHECK(x.position.y == y.position.y);
    CHECK(x.position.z == y.position.z);
    CHECK(x.color_start.w == y.color_start.w);
    CHECK(x.additive == y.additive);
}
