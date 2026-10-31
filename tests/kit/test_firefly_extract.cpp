// Extract feed of the fireflies (M-FIREFLIES 23b): Kit3DExtract builds one local additive render::ParticleEmitter from
// firefly_look and adds it to the scene (id 0xFFFF0002, following the camera). No Firefly, enabled 0, strength 0 and an
// amount of 0 (day, rain) add nothing (the emitter list and the stats stay bit-identical); Firefly, Season, WorldClock,
// Weather and every ParticleEmitter3D are never written.

#include <doctest/doctest.h>

#include <algorithm>

#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/firefly.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/leaf_fall.h"
#include "mce/kit/season.h"
#include "mce/kit/weather.h"
#include "mce/kit/world_clock.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

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
    Entity ff_e, season_e, clock_e, weather_e;

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
    void add_firefly(Firefly f = {}) { ff_e = world.spawn_with(SaveKey{2}, f); }
    Firefly& firefly() { return *world.try_get_mut<Firefly>(ff_e); }
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
    void set_time(f32 hours) {
        world.resource<Environment3D>().time_of_day = hours;
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

// A Season at the summer centre on day 0, at midnight.
void summer_night(Rig& r) {
    r.add_season(0.375f);
    r.add_clock(0);
    r.set_time(kMidnight);
}

const render::ParticleEmitter* firefly_of(const Shot& f) {
    for (const render::ParticleEmitter& e : f.emitters)
        if (e.id == kFireflyId) return &e;
    return nullptr;
}

}  // namespace

TEST_CASE("firefly extract: no Firefly adds nothing") {
    Rig base;
    base.set_time(kMidnight);
    const Shot a = base.frame();
    CHECK(a.emitters.size() == 0);
    CHECK(a.stat == 0);
}

TEST_CASE("firefly extract: day, rain, enabled 0, strength 0 and no driver add nothing") {
    Rig day;  // night_only: the day closes the gate
    day.add_season(0.375f);
    day.add_clock(0);
    day.set_time(kNoon);
    day.add_firefly();
    CHECK(day.frame().emitters.size() == 0);
    CHECK(day.extract.stats().particle_emitters == 0);

    Rig noclock;  // no time of day: no night fade, night_only shows nothing
    noclock.add_season(0.375f);
    noclock.add_clock(0);
    noclock.add_firefly();
    CHECK(noclock.frame().emitters.size() == 0);

    Rig rain;
    summer_night(rain);
    rain.add_rain(1.0f);
    rain.add_firefly();
    CHECK(rain.frame().emitters.size() == 0);

    Rig off;
    summer_night(off);
    off.add_firefly();
    off.firefly().enabled = 0;
    CHECK(off.frame().emitters.size() == 0);

    Rig zero;
    summer_night(zero);
    zero.add_firefly();
    zero.firefly().strength = 0.0f;
    CHECK(zero.frame().emitters.size() == 0);

    Rig winter;  // outside the summer window
    winter.add_season(0.875f);
    winter.add_clock(0);
    winter.set_time(kMidnight);
    winter.add_firefly();
    CHECK(winter.frame().emitters.size() == 0);

    Rig nothing;  // no Season, manual 0
    nothing.set_time(kMidnight);
    nothing.add_firefly();
    CHECK(nothing.frame().emitters.size() == 0);
}

TEST_CASE("firefly extract: a summer night adds exactly one additive emitter with the helper's look") {
    Rig r;
    summer_night(r);
    r.add_firefly();
    const Shot f = r.frame();
    REQUIRE(f.emitters.size() == 1);
    CHECK(f.stat == 1);
    const render::ParticleEmitter& e = f.emitters[0];
    CHECK(e.id == kFireflyId);

    const Season& s = *r.world.try_get<Season>(r.season_e);
    const f32 amount = firefly_amount(r.firefly(), &s, season_phase(s, 0, 0.0f), 1.0f, 0.0f, 0.0f);
    CHECK(amount == doctest::Approx(1.0f));
    const FireflyLook l = firefly_look(r.firefly(), amount);
    CHECK(e.rate == doctest::Approx(l.rate));
    CHECK(e.rate == doctest::Approx(18.0f));
    CHECK(e.speed_min == doctest::Approx(l.speed_min));
    CHECK(e.speed_max == doctest::Approx(l.speed_max));
    CHECK(e.direction.y == doctest::Approx(l.direction.y));
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
    CHECK(e.box.x == doctest::Approx(14.0f));
    CHECK(e.box.y == 0.0f);
    CHECK(e.box.z == doctest::Approx(14.0f));
    CHECK(e.max_particles == 256u);
    CHECK(e.seed == 1u);
    CHECK(e.additive);
    CHECK_FALSE(e.collide);
    CHECK(e.emitting);
    CHECK(e.burst == 0u);

    // authoring flows through
    r.firefly().rate = 100.0f;
    r.firefly().max_particles = 512;
    r.firefly().seed = 77;
    r.firefly().strength = 0.5f;
    const Shot g = r.frame();
    REQUIRE(g.emitters.size() == 1);
    CHECK(g.emitters[0].rate == doctest::Approx(50.0f));
    CHECK(g.emitters[0].max_particles == 512u);
    CHECK(g.emitters[0].seed == 77u);
}

TEST_CASE("firefly extract: the id never collides and the emitter follows the camera") {
    Rig r;
    summer_night(r);
    r.add_firefly();
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
        if (e.id == kFireflyId) {
            ++count;
            continue;
        }
        bool known = false;
        for (const Entity s : spawned) known = known || e.id == s.index + 1;
        CHECK(known);
    }
    CHECK(count == 1);
    CHECK(kFireflyId != kLeafId);
    for (const Entity s : spawned) CHECK(s.index + 1 != kFireflyId);

    const render::ParticleEmitter* e0 = firefly_of(f);
    REQUIRE(e0 != nullptr);
    CHECK(e0->position.x == doctest::Approx(r.cam.position.x));
    CHECK(e0->position.y == doctest::Approx(r.cam.position.y + 3.0f));
    CHECK(e0->position.z == doctest::Approx(r.cam.position.z));

    r.cam.position = {30.0f, 4.0f, -12.0f};
    r.firefly().height_m = 6.0f;
    const render::ParticleEmitter* e1 = nullptr;
    const Shot m = r.frame();
    e1 = firefly_of(m);
    REQUIRE(e1 != nullptr);
    CHECK(e1->position.x == doctest::Approx(30.0f));
    CHECK(e1->position.y == doctest::Approx(10.0f));
    CHECK(e1->position.z == doctest::Approx(-12.0f));
}

TEST_CASE("firefly extract: the manual floor works without a Season") {
    Rig r;
    r.set_time(kMidnight);
    r.add_firefly();
    r.firefly().manual = 0.5f;
    const Shot f = r.frame();
    REQUIRE(f.emitters.size() == 1);
    CHECK(firefly_of(f)->rate == doctest::Approx(9.0f));

    Rig by_day;  // night_only 0: shows by day too
    by_day.set_time(kNoon);
    by_day.add_firefly();
    by_day.firefly().manual = 1.0f;
    by_day.firefly().night_only = 0;
    CHECK(by_day.frame().emitters.size() == 1);
}

TEST_CASE("firefly extract: the Low tier halves the rate and the budget") {
    Rig r;
    summer_night(r);
    r.add_firefly();
    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    const Shot high = r.frame();
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    const Shot low = r.frame();
    REQUIRE(firefly_of(high) != nullptr);
    REQUIRE(firefly_of(low) != nullptr);
    CHECK(firefly_of(low)->rate == doctest::Approx(0.5f * firefly_of(high)->rate));
    CHECK(firefly_of(low)->max_particles == firefly_of(high)->max_particles / 2u);
    CHECK(low.stat == 1);
}

TEST_CASE("firefly extract: leaf and firefly emitters coexist") {
    Rig r;
    r.add_season(0.375f);  // summer: the leaves need autumn, so give the leaves their manual floor
    r.add_clock(0);
    r.set_time(kMidnight);
    r.add_firefly();
    LeafFall l;
    l.manual = 1.0f;
    r.world.spawn_with(SaveKey{7}, l);
    const Shot f = r.frame();
    REQUIRE(f.emitters.size() == 2);
    CHECK(f.stat == 2);
    CHECK(firefly_of(f) != nullptr);
    bool leaf = false;
    for (const render::ParticleEmitter& e : f.emitters) leaf = leaf || e.id == kLeafId;
    CHECK(leaf);
    CHECK_FALSE(firefly_of(f)->additive == false);
}
