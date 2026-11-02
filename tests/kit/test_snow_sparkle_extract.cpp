// Extract feed of the snow sparkle (M-SNOWSPARKLE 31b): Kit3DExtract builds one local additive render::ParticleEmitter
// from snow_sparkle_look and adds it to the scene (id 0xFFFF0008, in a flat box centred on the camera). No SnowSparkle,
// enabled 0, strength 0 and an amount of 0 (no snow cover and no manual floor, high sun, cloud, rain) add nothing (the
// emitter list and the stats stay bit-identical); SnowSparkle, SnowCover, Weather and every ParticleEmitter3D are never
// written.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/dusk_bat.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/snow_cover.h"
#include "mce/kit/snow_sparkle.h"
#include "mce/kit/weather.h"
#include "mce/render/atmosphere.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr u32 kSparkleId = 0xFFFF0008u;
constexpr u32 kDuskBatId = 0xFFFF0007u;
// A low morning sun, inside the plateau of the default sun gate; noon is far above the band.
constexpr f32 kLowSun = 6.5f;
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
    Entity sparkle_e, cover_e, weather_e;

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
    void add_sparkle(SnowSparkle s = {}) { sparkle_e = world.spawn_with(SaveKey{2}, s); }
    SnowSparkle& sparkle() { return *world.try_get_mut<SnowSparkle>(sparkle_e); }
    void add_cover(f32 cover, f32 strength = 1.0f) {
        SnowCover c;
        c.cover = cover;
        c.strength = strength;
        cover_e = world.spawn_with(SaveKey{3}, c);
    }
    void add_weather(WeatherKind kind, f32 intensity) {
        Weather w;
        w.current = kind;
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

// Full snow cover, a low sun, a clear sky.
void snowy_morning(Rig& r) {
    r.add_cover(1.0f);
    r.set_time(kLowSun);
    r.add_weather(WeatherKind::Clear, 0.0f);
}

const render::ParticleEmitter* sparkle_of(const Shot& f) {
    for (const render::ParticleEmitter& e : f.emitters)
        if (e.id == kSparkleId) return &e;
    return nullptr;
}

}  // namespace

TEST_CASE("snow sparkle extract: no SnowSparkle adds nothing") {
    Rig r;
    snowy_morning(r);
    const Shot a = r.frame();
    CHECK(a.emitters.size() == 0);
    CHECK(a.stat == 0);
}

TEST_CASE("snow sparkle extract: enabled 0 and strength 0 add nothing") {
    Rig off;
    off.add_sparkle();
    snowy_morning(off);
    off.sparkle().enabled = 0;
    CHECK(off.frame().emitters.size() == 0);
    CHECK(off.extract.stats().particle_emitters == 0);

    Rig zero;
    zero.add_sparkle();
    snowy_morning(zero);
    zero.sparkle().strength = 0.0f;
    zero.sparkle().manual = 1.0f;
    CHECK(zero.frame().emitters.size() == 0);
}

TEST_CASE("snow sparkle extract: snow cover, a low sun and a clear sky add exactly one additive emitter with the helper's look") {
    Rig r;
    r.add_sparkle();
    snowy_morning(r);
    const f32 elev = -render::sun_direction_at(kLowSun).y;
    REQUIRE(elev > 0.05f);
    REQUIRE(elev < 0.18f);
    const Shot f = r.frame();
    REQUIRE(f.emitters.size() == 1);
    CHECK(f.stat == 1);
    const render::ParticleEmitter& e = f.emitters[0];
    CHECK(e.id == kSparkleId);

    const f32 amount = snow_sparkle_amount(r.sparkle(), 1.0f, elev, 0.0f, 0.0f);
    CHECK(amount == doctest::Approx(1.0f));
    const SnowSparkleLook l = snow_sparkle_look(r.sparkle(), amount);
    CHECK(e.rate == doctest::Approx(l.rate));
    CHECK(e.rate == doctest::Approx(24.0f));
    CHECK(e.speed_max == doctest::Approx(l.speed_max));
    CHECK(e.direction.y == doctest::Approx(1.0f));
    CHECK(e.drag == doctest::Approx(l.drag));
    CHECK(e.size_start == doctest::Approx(0.03f));
    CHECK(e.size_end == doctest::Approx(0.03f));
    CHECK(e.lifetime_min == doctest::Approx(0.3f));
    CHECK(e.lifetime_max == doctest::Approx(0.6f));
    CHECK(e.color_start.x == doctest::Approx(1.0f));
    CHECK(e.color_start.w == doctest::Approx(1.0f));
    CHECK(e.color_end.w == 0.0f);
    CHECK(e.box.x == doctest::Approx(14.0f));
    CHECK(e.box.y == doctest::Approx(1.2f));
    CHECK(e.box.z == doctest::Approx(14.0f));
    CHECK(e.position.x == doctest::Approx(r.cam.position.x));
    CHECK(e.position.z == doctest::Approx(r.cam.position.z));
    CHECK(e.max_particles == 96u);
    CHECK(e.seed == 1u);
    CHECK(e.additive);
    CHECK_FALSE(e.collide);
    CHECK(e.emitting);
    CHECK(e.burst == 0u);

    // authoring flows through; max_particles is clamped to 8..512
    r.sparkle().max_particles = 1;
    CHECK(sparkle_of(r.frame())->max_particles == 8u);
    r.sparkle().max_particles = 100000;
    CHECK(sparkle_of(r.frame())->max_particles == 512u);
    r.sparkle().seed = 77;
    r.sparkle().strength = 0.5f;
    const Shot g = r.frame();
    REQUIRE(g.emitters.size() == 1);
    CHECK(g.emitters[0].rate == doctest::Approx(12.0f));
    CHECK(g.emitters[0].seed == 77u);

    // the cover value is cover * SnowCover.strength
    Rig half;
    half.add_sparkle();
    half.add_cover(1.0f, 0.0f);  // strength 0: no cover at all
    half.set_time(kLowSun);
    CHECK(half.frame().emitters.size() == 0);

    // the emitter follows the camera
    r.cam.position = {30.0f, 4.0f, -12.0f};
    const render::ParticleEmitter* e1 = sparkle_of(r.frame());
    REQUIRE(e1 != nullptr);
    CHECK(e1->position.x == doctest::Approx(30.0f));
    CHECK(e1->position.z == doctest::Approx(-12.0f));
}

TEST_CASE("snow sparkle extract: no SnowCover only the manual floor makes an amount, and a high sun or no clock none") {
    Rig bare;
    bare.add_sparkle();
    bare.set_time(kLowSun);
    CHECK(bare.frame().emitters.size() == 0);  // no SnowCover: cover 0

    Rig manual;
    manual.add_sparkle();
    manual.sparkle().manual = 0.5f;
    manual.set_time(kNoon);  // the manual floor ignores the sun gate
    const Shot m = manual.frame();
    REQUIRE(m.emitters.size() == 1);
    CHECK(sparkle_of(m)->rate == doctest::Approx(12.0f));

    Rig noclock;  // time_of_day -1: sun elevation 0, so only the floor works
    noclock.add_sparkle();
    noclock.add_cover(1.0f);
    CHECK(noclock.frame().emitters.size() == 0);
    noclock.sparkle().manual = 0.25f;
    const Shot n = noclock.frame();
    REQUIRE(n.emitters.size() == 1);
    CHECK(sparkle_of(n)->rate == doctest::Approx(6.0f));

    Rig high;  // snow but a high midday sun: closed sun gate
    high.add_sparkle();
    high.add_cover(1.0f);
    high.set_time(kNoon);
    CHECK(high.frame().emitters.size() == 0);
}

TEST_CASE("snow sparkle extract: cloud and rain remove it") {
    Rig over;
    over.add_sparkle();
    over.add_cover(1.0f);
    over.set_time(kLowSun);
    over.add_weather(WeatherKind::Overcast, 1.0f);
    CHECK(over.frame().emitters.size() == 0);
    over.world.try_get_mut<Weather>(over.weather_e)->intensity = 0.5f;
    const Shot half = over.frame();
    REQUIRE(half.emitters.size() == 1);
    CHECK(sparkle_of(half)->rate == doctest::Approx(12.0f));

    Rig rain;
    rain.add_sparkle();
    rain.add_cover(1.0f);
    rain.set_time(kLowSun);
    rain.add_weather(WeatherKind::Rain, 1.0f);
    CHECK(rain.frame().emitters.size() == 0);
    rain.world.try_get_mut<Weather>(rain.weather_e)->current = WeatherKind::Clear;
    CHECK(rain.frame().emitters.size() == 1);

    Rig storm;  // the manual floor does not survive rain either
    storm.add_sparkle();
    storm.sparkle().manual = 1.0f;
    storm.add_weather(WeatherKind::Storm, 1.0f);
    CHECK(storm.frame().emitters.size() == 0);
}

TEST_CASE("snow sparkle extract: nothing is written") {
    Rig r;
    r.add_sparkle();
    snowy_morning(r);
    const SnowSparkle before = r.sparkle();
    const f32 cover = r.world.try_get<SnowCover>(r.cover_e)->cover;
    (void)r.frame();
    (void)r.frame();
    const SnowSparkle& after = r.sparkle();
    CHECK(after.enabled == before.enabled);
    CHECK(after.strength == before.strength);
    CHECK(after.manual == before.manual);
    CHECK(after.rate == before.rate);
    CHECK(after.max_particles == before.max_particles);
    CHECK(r.world.try_get<SnowCover>(r.cover_e)->cover == cover);
    CHECK(r.world.try_get<Weather>(r.weather_e)->intensity == 0.0f);
}

TEST_CASE("snow sparkle extract: the id never collides with other emitters") {
    Rig r;
    r.add_sparkle();
    snowy_morning(r);
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
        if (e.id == kSparkleId) {
            ++count;
            continue;
        }
        bool known = false;
        for (const Entity s : spawned) known = known || e.id == s.index + 1;
        CHECK(known);
    }
    CHECK(count == 1);
    CHECK(kSparkleId != kDuskBatId);
    for (const Entity s : spawned) CHECK(s.index + 1 != kSparkleId);
}

TEST_CASE("snow sparkle extract: the Low tier halves the rate and the budget") {
    Rig r;
    r.add_sparkle();
    snowy_morning(r);
    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    const Shot high = r.frame();
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    const Shot low = r.frame();
    REQUIRE(sparkle_of(high) != nullptr);
    REQUIRE(sparkle_of(low) != nullptr);
    CHECK(sparkle_of(low)->rate == doctest::Approx(0.5f * sparkle_of(high)->rate));
    CHECK(sparkle_of(low)->max_particles == sparkle_of(high)->max_particles / 2u);
    CHECK(low.stat == 1);
}

TEST_CASE("snow sparkle extract: a particle_scale of 0 (r.particles 0) adds none, a fraction scales the rate and the budget") {
    Rig r;
    r.add_sparkle();
    snowy_morning(r);
    render::QualitySettings q = render::quality_settings(render::QualityTier::Medium);
    r.extract.set_quality(q);
    const Shot full = r.frame();
    REQUIRE(sparkle_of(full) != nullptr);
    q.particle_scale = 0.25f;
    r.extract.set_quality(q);
    const Shot quarter = r.frame();
    REQUIRE(sparkle_of(quarter) != nullptr);
    CHECK(sparkle_of(quarter)->rate == doctest::Approx(0.25f * sparkle_of(full)->rate));
    CHECK(sparkle_of(quarter)->max_particles == sparkle_of(full)->max_particles / 4u);
    q.particle_scale = 0.0f;
    r.extract.set_quality(q);
    const Shot none = r.frame();
    CHECK(sparkle_of(none) == nullptr);
    CHECK(none.stat == 0);
}
