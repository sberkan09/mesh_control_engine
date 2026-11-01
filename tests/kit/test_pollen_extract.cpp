// Extract feed of the pollen (M-POLLEN 24b): Kit3DExtract builds one local alpha-blended render::ParticleEmitter from
// pollen_look and adds it to the scene (id 0xFFFF0003, in a box following the camera). No Pollen, enabled 0, strength 0
// and an amount of 0 (night with day_only, rain, outside spring) add nothing (the emitter list and the stats stay
// bit-identical); Pollen, Season, WorldClock, Weather, WindDirection and every ParticleEmitter3D are never written.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/firefly.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/leaf_fall.h"
#include "mce/kit/pollen.h"
#include "mce/kit/season.h"
#include "mce/kit/weather.h"
#include "mce/kit/wind_direction.h"
#include "mce/kit/wind_gust.h"
#include "mce/kit/world_clock.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr u32 kPollenId = 0xFFFF0003u;
constexpr u32 kFireflyId = 0xFFFF0002u;
constexpr u32 kLeafId = 0xFFFF0001u;
constexpr f32 kMidnight = 0.0f;
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
    Entity po_e, season_e, clock_e, weather_e, wd_e;

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
    void add_pollen(Pollen p = {}) { po_e = world.spawn_with(SaveKey{2}, p); }
    Pollen& pollen() { return *world.try_get_mut<Pollen>(po_e); }
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
    void add_wind(f32 wind) {
        Weather w;
        w.wind = wind;
        weather_e = world.spawn_with(SaveKey{5}, w);
    }
    void add_wind_direction(f32 degrees) {
        WindDirection d;
        d.degrees = degrees;
        d.angle_q = static_cast<u32>(degrees * 65536.0f);  // engine-written by the system; there is no tick here
        wd_e = world.spawn_with(SaveKey{6}, d);
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

// A Season at the spring centre on day 0, at noon.
void spring_day(Rig& r) {
    r.add_season(0.125f);
    r.add_clock(0);
    r.set_time(kNoon);
}

const render::ParticleEmitter* pollen_of(const Shot& f) {
    for (const render::ParticleEmitter& e : f.emitters)
        if (e.id == kPollenId) return &e;
    return nullptr;
}

}  // namespace

TEST_CASE("pollen extract: no Pollen adds nothing") {
    Rig base;
    base.set_time(kNoon);
    const Shot a = base.frame();
    CHECK(a.emitters.size() == 0);
    CHECK(a.stat == 0);
}

TEST_CASE("pollen extract: night, rain, enabled 0, strength 0, other seasons and no driver add nothing") {
    Rig night;  // day_only: the night closes the gate
    night.add_season(0.125f);
    night.add_clock(0);
    night.set_time(kMidnight);
    night.add_pollen();
    CHECK(night.frame().emitters.size() == 0);
    CHECK(night.extract.stats().particle_emitters == 0);

    Rig rain;
    spring_day(rain);
    rain.add_rain(1.0f);
    rain.add_pollen();
    CHECK(rain.frame().emitters.size() == 0);

    Rig off;
    spring_day(off);
    off.add_pollen();
    off.pollen().enabled = 0;
    CHECK(off.frame().emitters.size() == 0);

    Rig zero;
    spring_day(zero);
    zero.add_pollen();
    zero.pollen().strength = 0.0f;
    CHECK(zero.frame().emitters.size() == 0);

    Rig autumn;  // outside the spring window
    autumn.add_season(0.625f);
    autumn.add_clock(0);
    autumn.set_time(kNoon);
    autumn.add_pollen();
    CHECK(autumn.frame().emitters.size() == 0);

    Rig nothing;  // no Season, manual 0
    nothing.set_time(kNoon);
    nothing.add_pollen();
    CHECK(nothing.frame().emitters.size() == 0);
}

TEST_CASE("pollen extract: a spring day adds exactly one alpha-blended emitter with the helper's look") {
    Rig r;
    spring_day(r);
    r.add_pollen();
    const Shot f = r.frame();
    REQUIRE(f.emitters.size() == 1);
    CHECK(f.stat == 1);
    const render::ParticleEmitter& e = f.emitters[0];
    CHECK(e.id == kPollenId);

    const Season& s = *r.world.try_get<Season>(r.season_e);
    const f32 amount = pollen_amount(r.pollen(), &s, season_phase(s, 0, 0.0f), 0.0f, 0.0f);
    CHECK(amount == doctest::Approx(1.0f));
    const PollenLook l = pollen_look(r.pollen(), amount, 0.0f, math::vec2{1.0f, 0.0f});
    CHECK(e.rate == doctest::Approx(l.rate));
    CHECK(e.rate == doctest::Approx(14.0f));
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
    CHECK(e.color_start.y == doctest::Approx(l.color_start.y));
    CHECK(e.color_start.z == doctest::Approx(l.color_start.z));
    CHECK(e.color_start.w == doctest::Approx(l.color_start.w));
    CHECK(e.color_end.w == 0.0f);
    CHECK(e.box.x == doctest::Approx(18.0f));
    CHECK(e.box.y == 0.0f);
    CHECK(e.box.z == doctest::Approx(18.0f));
    CHECK(e.max_particles == 512u);
    CHECK(e.seed == 1u);
    CHECK_FALSE(e.additive);
    CHECK_FALSE(e.collide);
    CHECK(e.emitting);
    CHECK(e.burst == 0u);

    // authoring flows through; max_particles is clamped to 16..4096
    r.pollen().rate = 100.0f;
    r.pollen().max_particles = 1000;
    r.pollen().seed = 77;
    r.pollen().strength = 0.5f;
    const Shot g = r.frame();
    REQUIRE(g.emitters.size() == 1);
    CHECK(g.emitters[0].rate == doctest::Approx(50.0f));
    CHECK(g.emitters[0].max_particles == 1000u);
    CHECK(g.emitters[0].seed == 77u);
    r.pollen().max_particles = 4;
    CHECK(pollen_of(r.frame())->max_particles == 16u);
    r.pollen().max_particles = 100000;
    CHECK(pollen_of(r.frame())->max_particles == 4096u);
}

TEST_CASE("pollen extract: the id never collides and the emitter follows the camera") {
    Rig r;
    spring_day(r);
    r.add_pollen();
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
        if (e.id == kPollenId) {
            ++count;
            continue;
        }
        bool known = false;
        for (const Entity s : spawned) known = known || e.id == s.index + 1;
        CHECK(known);
    }
    CHECK(count == 1);
    CHECK(kPollenId != kLeafId);
    CHECK(kPollenId != kFireflyId);
    for (const Entity s : spawned) CHECK(s.index + 1 != kPollenId);

    const render::ParticleEmitter* e0 = pollen_of(f);
    REQUIRE(e0 != nullptr);
    CHECK(e0->position.x == doctest::Approx(r.cam.position.x));
    CHECK(e0->position.y == doctest::Approx(r.cam.position.y + 5.0f));
    CHECK(e0->position.z == doctest::Approx(r.cam.position.z));

    r.cam.position = {30.0f, 4.0f, -12.0f};
    r.pollen().height_m = 8.0f;
    const Shot m = r.frame();
    const render::ParticleEmitter* e1 = pollen_of(m);
    REQUIRE(e1 != nullptr);
    CHECK(e1->position.x == doctest::Approx(30.0f));
    CHECK(e1->position.y == doctest::Approx(12.0f));
    CHECK(e1->position.z == doctest::Approx(-12.0f));
}

TEST_CASE("pollen extract: the wind direction turns the drift and wind 0 drops it") {
    Rig calm;
    spring_day(calm);
    calm.add_pollen();
    calm.add_wind(0.0f);
    const Shot fc = calm.frame();
    const render::ParticleEmitter* c = pollen_of(fc);
    REQUIRE(c != nullptr);
    CHECK(c->direction.x == 0.0f);
    CHECK(c->direction.z == 0.0f);
    CHECK(c->direction.y == doctest::Approx(-1.0f));

    Rig windy;  // no WindDirection: +x
    spring_day(windy);
    windy.add_pollen();
    windy.add_wind(0.8f);
    const Shot fw = windy.frame();
    const render::ParticleEmitter* w = pollen_of(fw);
    REQUIRE(w != nullptr);
    CHECK(w->direction.x > 0.1f);
    CHECK(w->direction.z == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(w->speed_max > c->speed_max);

    Rig turned;  // 90 degrees: +z in the engine's (x, z) plane
    spring_day(turned);
    turned.add_pollen();
    turned.add_wind(0.8f);
    turned.add_wind_direction(90.0f);
    const Shot ft = turned.frame();
    const render::ParticleEmitter* t = pollen_of(ft);
    REQUIRE(t != nullptr);
    const math::vec2 v = wind_direction_vector(*turned.world.try_get<WindDirection>(turned.wd_e));
    const Weather& wx = *turned.world.try_get<Weather>(turned.weather_e);
    const PollenLook l = pollen_look(turned.pollen(), 1.0f, effective_wind(wx, nullptr, kGustSway), v);
    CHECK(t->direction.x == doctest::Approx(l.direction.x));
    CHECK(t->direction.z == doctest::Approx(l.direction.z));
    CHECK(std::abs(t->direction.z) > 0.1f);
    CHECK(std::abs(t->direction.x) < 0.01f);

    Rig no_weather;  // a WindDirection alone changes nothing: calm
    spring_day(no_weather);
    no_weather.add_pollen();
    no_weather.add_wind_direction(90.0f);
    const render::ParticleEmitter* n = pollen_of(no_weather.frame());
    REQUIRE(n != nullptr);
    CHECK(n->direction.x == 0.0f);
    CHECK(n->direction.z == 0.0f);

    Rig no_follow;
    spring_day(no_follow);
    no_follow.add_pollen();
    no_follow.pollen().wind_follow = 0.0f;
    no_follow.add_wind(0.8f);
    const render::ParticleEmitter* nf = pollen_of(no_follow.frame());
    REQUIRE(nf != nullptr);
    CHECK(nf->direction.x == 0.0f);
    CHECK(nf->direction.z == 0.0f);
}

TEST_CASE("pollen extract: the manual floor works without a Season") {
    Rig r;
    r.set_time(kNoon);
    r.add_pollen();
    r.pollen().manual = 0.5f;
    const Shot f = r.frame();
    REQUIRE(f.emitters.size() == 1);
    CHECK(pollen_of(f)->rate == doctest::Approx(7.0f));

    Rig by_night;  // day_only 0: shows by night too
    by_night.set_time(kMidnight);
    by_night.add_pollen();
    by_night.pollen().manual = 1.0f;
    by_night.pollen().day_only = 0;
    CHECK(by_night.frame().emitters.size() == 1);
}

TEST_CASE("pollen extract: the Low tier halves the rate and the budget") {
    Rig r;
    spring_day(r);
    r.add_pollen();
    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    const Shot high = r.frame();
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    const Shot low = r.frame();
    REQUIRE(pollen_of(high) != nullptr);
    REQUIRE(pollen_of(low) != nullptr);
    CHECK(pollen_of(low)->rate == doctest::Approx(0.5f * pollen_of(high)->rate));
    CHECK(pollen_of(low)->max_particles == pollen_of(high)->max_particles / 2u);
    CHECK(low.stat == 1);
}

TEST_CASE("pollen extract: leaf, firefly and pollen emitters coexist") {
    Rig r;
    r.add_season(0.125f);
    r.add_clock(0);
    r.set_time(kNoon);
    r.add_pollen();
    Firefly ff;
    ff.manual = 1.0f;
    ff.night_only = 0;
    r.world.spawn_with(SaveKey{7}, ff);
    LeafFall l;
    l.manual = 1.0f;
    r.world.spawn_with(SaveKey{8}, l);
    const Shot f = r.frame();
    REQUIRE(f.emitters.size() == 3);
    CHECK(f.stat == 3);
    bool leaf = false, fire = false;
    for (const render::ParticleEmitter& e : f.emitters) {
        leaf = leaf || e.id == kLeafId;
        fire = fire || e.id == kFireflyId;
    }
    CHECK(leaf);
    CHECK(fire);
    REQUIRE(pollen_of(f) != nullptr);
    CHECK_FALSE(pollen_of(f)->additive);
}
