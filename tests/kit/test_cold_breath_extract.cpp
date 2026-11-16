// Extract feed of the cold breath (M-COLDBREATH 34b): Kit3DExtract builds one local alpha-blended render::ParticleEmitter
// from cold_breath_look and adds it to the scene (id 0xFFFF000A, a point at the camera plus forward * mouth_forward minus
// up * mouth_down). No ColdBreath, enabled 0, strength 0 and an amount of 0 (outside the winter window, no snow cover, wind,
// no manual floor) add nothing (the emitter list and the stats stay bit-identical); ColdBreath, Season, WorldClock, Weather
// and SnowCover are never written.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/cold_breath.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/season.h"
#include "mce/kit/snow_cover.h"
#include "mce/kit/weather.h"
#include "mce/kit/world_clock.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr u32 kColdBreathId = 0xFFFF000Au;
constexpr u32 kMidgeId = 0xFFFF0009u;

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
    Entity breath_e, season_e, clock_e, weather_e, cover_e;

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
    void add_breath(ColdBreath c = {}) { breath_e = world.spawn_with(SaveKey{2}, c); }
    ColdBreath& breath() { return *world.try_get_mut<ColdBreath>(breath_e); }
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
    void add_weather(f32 wind) {
        Weather w;
        w.current = WeatherKind::Clear;
        w.wind = wind;
        w.intensity = 0.0f;
        weather_e = world.spawn_with(SaveKey{5}, w);
    }
    void add_cover(f32 cover) {
        SnowCover c;
        c.cover = cover;
        cover_e = world.spawn_with(SaveKey{6}, c);
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

// A Season at the winter centre on day 0, a clock, a calm sky, bare ground.
void winter_calm(Rig& r) {
    r.add_season(0.875f);
    r.add_clock(0);
    r.add_weather(0.1f);
}

const render::ParticleEmitter* breath_of(const Shot& f) {
    for (const render::ParticleEmitter& e : f.emitters)
        if (e.id == kColdBreathId) return &e;
    return nullptr;
}

}  // namespace

TEST_CASE("cold breath extract: no ColdBreath adds nothing and leaves other emitters unchanged") {
    Rig base;
    winter_calm(base);
    const Shot a = base.frame();
    CHECK(a.emitters.size() == 0);
    CHECK(a.stat == 0);
    CHECK(breath_of(a) == nullptr);

    Rig other;
    winter_calm(other);
    ParticleEmitter3D p;
    other.world.spawn_with(LocalTransform{}, p);
    const Shot b = other.frame();
    REQUIRE(b.emitters.size() == 1);
    CHECK(b.emitters[0].id != kColdBreathId);
    Rig with;
    winter_calm(with);
    with.world.spawn_with(LocalTransform{}, p);
    with.add_breath();
    const Shot c = with.frame();
    REQUIRE(c.emitters.size() == 2);
    CHECK(c.emitters[0].id == b.emitters[0].id);
    CHECK(c.emitters[0].rate == b.emitters[0].rate);
    CHECK(c.emitters[0].max_particles == b.emitters[0].max_particles);
    CHECK(c.emitters[0].position.x == b.emitters[0].position.x);
    CHECK(c.emitters[0].position.y == b.emitters[0].position.y);
    CHECK(c.emitters[1].id == kColdBreathId);
}

TEST_CASE("cold breath extract: enabled 0 and strength 0 add nothing") {
    Rig off;
    off.add_breath();
    winter_calm(off);
    off.breath().enabled = 0;
    CHECK(off.frame().emitters.size() == 0);
    CHECK(off.extract.stats().particle_emitters == 0);

    Rig zero;
    zero.add_breath();
    winter_calm(zero);
    zero.breath().strength = 0.0f;
    zero.breath().manual = 1.0f;
    CHECK(zero.frame().emitters.size() == 0);
}

TEST_CASE("cold breath extract: the manual floor adds one alpha-blended point emitter at the mouth") {
    Rig r;
    r.add_breath();
    r.breath().manual = 1.0f;  // no Season, no cover, no Weather: only the floor
    const Shot f = r.frame();
    REQUIRE(f.emitters.size() == 1);
    CHECK(f.stat == 1);
    const render::ParticleEmitter& e = f.emitters[0];
    CHECK(e.id == kColdBreathId);

    const ColdBreathLook l = cold_breath_look(r.breath(), 1.0f);
    REQUIRE(l.active);
    const math::vec3 fwd = r.scene.view().forward;
    CHECK(e.position.x == doctest::Approx(r.cam.position.x + fwd.x * l.mouth_forward));
    CHECK(e.position.y == doctest::Approx(r.cam.position.y + fwd.y * l.mouth_forward - l.mouth_down));
    CHECK(e.position.z == doctest::Approx(r.cam.position.z + fwd.z * l.mouth_forward));
    CHECK(e.box.x < 0.05f);
    CHECK(e.box.y < 0.05f);
    CHECK(e.box.z < 0.05f);
    // the direction follows the view's forward (the lift only tilts it up)
    const math::vec3 d = e.direction;
    const f32 len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    CHECK(len > 0.0f);
    CHECK(d.x / len * fwd.x + d.z / len * fwd.z > 0.9f);
    CHECK(e.direction.y == doctest::Approx(fwd.y + l.lift));
    CHECK(e.rate == doctest::Approx(l.rate));
    CHECK(e.speed_min == doctest::Approx(l.speed_min));
    CHECK(e.speed_max == doctest::Approx(l.speed_max));
    CHECK(e.drag == doctest::Approx(l.drag));
    CHECK(e.size_start == doctest::Approx(l.size_start));
    CHECK(e.size_end == doctest::Approx(l.size_end));
    CHECK(e.size_end > e.size_start);
    CHECK(e.lifetime_min == doctest::Approx(l.lifetime_min));
    CHECK(e.lifetime_max == doctest::Approx(l.lifetime_max));
    CHECK(e.color_start.w == doctest::Approx(l.color_start.w));
    CHECK(e.color_end.w == 0.0f);
    CHECK(e.max_particles == 24u);
    CHECK(e.seed == 1u);
    CHECK_FALSE(e.additive);
    CHECK_FALSE(e.collide);
    CHECK(e.emitting);
    CHECK(e.burst == 0u);

    // authoring flows through; max_particles is clamped to 4..128
    r.breath().max_particles = 1;
    CHECK(breath_of(r.frame())->max_particles == 4u);
    r.breath().max_particles = 100000;
    CHECK(breath_of(r.frame())->max_particles == 128u);
    r.breath().seed = 77;
    CHECK(breath_of(r.frame())->seed == 77u);
    r.breath().mouth_forward_m = 1.0f;
    r.breath().mouth_down_m = 0.5f;
    r.cam.position = {30.0f, 4.0f, -12.0f};
    const Shot moved_shot = r.frame();  // keep the frame alive: the emitter pointer points into it
    const render::ParticleEmitter* moved = breath_of(moved_shot);
    REQUIRE(moved != nullptr);
    CHECK(moved->position.x == doctest::Approx(30.0f + r.scene.view().forward.x));
    CHECK(moved->position.y == doctest::Approx(4.0f + r.scene.view().forward.y - 0.5f));
    CHECK(moved->position.z == doctest::Approx(-12.0f + r.scene.view().forward.z));
}

TEST_CASE("cold breath extract: winter and snow cover each switch it on, a summer bare scene does not") {
    Rig summer;
    summer.add_breath();
    summer.add_season(0.375f);
    summer.add_clock(0);
    summer.add_weather(0.1f);
    CHECK(summer.frame().emitters.size() == 0);

    Rig winter;
    winter.add_breath();
    winter_calm(winter);
    const Shot w = winter.frame();
    REQUIRE(w.emitters.size() == 1);
    CHECK(breath_of(w)->rate > 0.0f);

    Rig later;  // the clock day moves the phase out of the window (half a year on, year_days 12)
    later.add_breath();
    later.add_season(0.875f);
    later.add_clock(6);
    later.add_weather(0.1f);
    CHECK(later.frame().emitters.size() == 0);

    Rig snow;  // summer, but the ground is white
    snow.add_breath();
    snow.add_season(0.375f);
    snow.add_clock(0);
    snow.add_weather(0.1f);
    snow.add_cover(1.0f);
    const Shot s = snow.frame();
    REQUIRE(s.emitters.size() == 1);
    CHECK(breath_of(s)->rate > 0.0f);

    Rig scaled;  // cover times the SnowCover strength: strength 0 is a bare ground
    scaled.add_breath();
    scaled.add_season(0.375f);
    scaled.add_clock(0);
    scaled.add_weather(0.1f);
    scaled.add_cover(1.0f);
    scaled.world.try_get_mut<SnowCover>(scaled.cover_e)->strength = 0.0f;
    CHECK(scaled.frame().emitters.size() == 0);

    Rig bare;  // no Season, no Weather, no cover, no floor
    bare.add_breath();
    CHECK(bare.frame().emitters.size() == 0);
}

TEST_CASE("cold breath extract: wind past wind_still scatters the breath") {
    Rig gale;
    gale.add_breath();
    gale.add_season(0.875f);
    gale.add_clock(0);
    gale.add_weather(1.0f);
    CHECK(gale.frame().emitters.size() == 0);

    Rig breeze;  // between wind_calm and wind_still
    breeze.add_breath();
    breeze.add_season(0.875f);
    breeze.add_clock(0);
    breeze.add_weather(0.7f);
    Rig calm;
    calm.add_breath();
    winter_calm(calm);
    const Shot b = breeze.frame();
    const Shot c = calm.frame();
    REQUIRE(breath_of(b) != nullptr);
    REQUIRE(breath_of(c) != nullptr);
    CHECK(breath_of(b)->rate < breath_of(c)->rate);

    Rig floor_gale;  // the manual floor does not survive the wind gate
    floor_gale.add_breath();
    floor_gale.breath().manual = 1.0f;
    floor_gale.add_weather(1.0f);
    CHECK(floor_gale.frame().emitters.size() == 0);
}

TEST_CASE("cold breath extract: a degenerate zero forward gives no NaN") {
    Rig r;
    r.add_breath();
    r.breath().manual = 1.0f;
    r.cam.orientation = math::quat{0.0f, 0.0f, 0.0f, 0.0f};
    const Shot f = r.frame();
    REQUIRE(f.emitters.size() == 1);
    const render::ParticleEmitter& e = f.emitters[0];
    CHECK(std::isfinite(e.position.x));
    CHECK(std::isfinite(e.position.y));
    CHECK(std::isfinite(e.position.z));
    CHECK(std::isfinite(e.direction.x));
    CHECK(std::isfinite(e.direction.y));
    CHECK(std::isfinite(e.direction.z));
}

TEST_CASE("cold breath extract: nothing is written") {
    Rig r;
    r.add_breath();
    winter_calm(r);
    r.add_cover(0.4f);
    const ColdBreath before = r.breath();
    const Season s_before = *r.world.try_get<Season>(r.season_e);
    (void)r.frame();
    (void)r.frame();
    const ColdBreath& after = r.breath();
    CHECK(after.enabled == before.enabled);
    CHECK(after.strength == before.strength);
    CHECK(after.manual == before.manual);
    CHECK(after.rate == before.rate);
    CHECK(after.max_particles == before.max_particles);
    CHECK(r.world.try_get<Season>(r.season_e)->start_phase == s_before.start_phase);
    CHECK(r.world.try_get<WorldClock>(r.clock_e)->day == 0u);
    CHECK(r.world.try_get<Weather>(r.weather_e)->intensity == 0.0f);
    CHECK(r.world.try_get<SnowCover>(r.cover_e)->cover == 0.4f);
}

TEST_CASE("cold breath extract: the id never collides with other emitters") {
    Rig r;
    r.add_breath();
    winter_calm(r);
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
        if (e.id == kColdBreathId) {
            ++count;
            continue;
        }
        bool known = false;
        for (const Entity s : spawned) known = known || e.id == s.index + 1;
        CHECK(known);
    }
    CHECK(count == 1);
    CHECK(kColdBreathId != kMidgeId);
    for (const Entity s : spawned) CHECK(s.index + 1 != kColdBreathId);
}

TEST_CASE("cold breath extract: the Low tier halves the rate and the budget") {
    Rig r;
    r.add_breath();
    r.breath().max_particles = 64;
    winter_calm(r);
    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    const Shot high = r.frame();
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    const Shot low = r.frame();
    REQUIRE(breath_of(high) != nullptr);
    REQUIRE(breath_of(low) != nullptr);
    CHECK(breath_of(low)->rate == doctest::Approx(0.5f * breath_of(high)->rate));
    CHECK(breath_of(low)->max_particles == breath_of(high)->max_particles / 2u);
    CHECK(low.stat == 1);
}

TEST_CASE("cold breath extract: a particle_scale of 0 (r.particles 0) adds none, a fraction scales the rate and the budget") {
    Rig r;
    r.add_breath();
    r.breath().max_particles = 64;
    winter_calm(r);
    render::QualitySettings q = render::quality_settings(render::QualityTier::Medium);
    r.extract.set_quality(q);
    const Shot full = r.frame();
    REQUIRE(breath_of(full) != nullptr);
    q.particle_scale = 0.25f;
    r.extract.set_quality(q);
    const Shot quarter = r.frame();
    REQUIRE(breath_of(quarter) != nullptr);
    CHECK(breath_of(quarter)->rate == doctest::Approx(0.25f * breath_of(full)->rate));
    CHECK(breath_of(quarter)->max_particles == breath_of(full)->max_particles / 4u);
    q.particle_scale = 0.0f;
    r.extract.set_quality(q);
    const Shot none = r.frame();
    CHECK(breath_of(none) == nullptr);
    CHECK(none.stat == 0);
}

TEST_CASE("cold breath extract: two extracts of the same scene are equal") {
    Rig r;
    r.add_breath();
    winter_calm(r);
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
    CHECK(x.direction.y == y.direction.y);
    CHECK(x.color_start.w == y.color_start.w);
    CHECK(x.additive == y.additive);
}
