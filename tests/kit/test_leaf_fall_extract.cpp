// Extract feed of the autumn leaves (M-LEAFFALL 21b): Kit3DExtract builds one local render::ParticleEmitter from
// leaf_fall_look and adds it to the scene (id 0xFFFF0001, following the camera). No LeafFall, enabled 0, strength 0 and
// an amount of 0 add nothing (the emitter list and the stats stay bit-identical); LeafFall, Season, WorldClock, Weather,
// WindDirection and every ParticleEmitter3D are never written.

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>

#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/leaf_fall.h"
#include "mce/kit/season.h"
#include "mce/kit/weather.h"
#include "mce/kit/wind_direction.h"
#include "mce/kit/world_clock.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr u32 kLeafId = 0xFFFF0001u;

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }

template <class T>
u32 bytes_hash(const T& v) {
    u32 h = 2166136261u;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(&v);
    for (usize i = 0; i < sizeof(T); ++i) h = (h ^ p[i]) * 16777619u;
    return h;
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
    Entity leaf_e, season_e, clock_e, weather_e, wd_e;

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
    void add_leaf(LeafFall l = {}) { leaf_e = world.spawn_with(SaveKey{2}, l); }
    LeafFall& leaf() { return *world.try_get_mut<LeafFall>(leaf_e); }
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
        w.wind = wind;
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

// A Season at the autumn centre on day 0.
void autumn(Rig& r) {
    r.add_season(0.625f);
    r.add_clock(0);
}

const render::ParticleEmitter* leaf_of(const Shot& f) {
    for (const render::ParticleEmitter& e : f.emitters)
        if (e.id == kLeafId) return &e;
    return nullptr;
}

bool same_emitter(const render::ParticleEmitter& a, const render::ParticleEmitter& b) {
    return a.id == b.id && a.max_particles == b.max_particles && a.seed == b.seed && same_bits(a.position.x, b.position.x) &&
           same_bits(a.position.y, b.position.y) && same_bits(a.position.z, b.position.z) && same_bits(a.rate, b.rate) &&
           same_bits(a.direction.x, b.direction.x) && same_bits(a.direction.y, b.direction.y) &&
           same_bits(a.direction.z, b.direction.z) && same_bits(a.color_start.w, b.color_start.w) &&
           same_bits(a.color_start.x, b.color_start.x) && same_bits(a.speed_min, b.speed_min);
}

}  // namespace

TEST_CASE("leaf_fall extract: no LeafFall, enabled 0, strength 0 and an amount of 0 add nothing") {
    Rig base;
    const Shot a = base.frame();
    CHECK(a.emitters.size() == 0);
    CHECK(a.stat == 0);

    Rig off;
    autumn(off);
    off.add_leaf();
    off.leaf().enabled = 0;
    CHECK(off.frame().emitters.size() == 0);
    CHECK(off.extract.stats().particle_emitters == 0);

    Rig zero;
    autumn(zero);
    zero.add_leaf();
    zero.leaf().strength = 0.0f;
    CHECK(zero.frame().emitters.size() == 0);

    Rig no_amount;  // no Season, manual 0: nothing drives the leaves
    no_amount.add_leaf();
    CHECK(no_amount.frame().emitters.size() == 0);

    Rig winter;  // the centre of winter is outside the autumn window
    winter.add_season(0.875f);
    winter.add_clock(0);
    winter.add_leaf();
    CHECK(winter.frame().emitters.size() == 0);
    CHECK(winter.extract.stats().particle_emitters == 0);

    Rig no_rate;  // an amount above 0 but a rate of 0 still yields an (idle) emitter, the amount alone gates it
    autumn(no_rate);
    no_rate.add_leaf();
    no_rate.leaf().rate = 0.0f;
    CHECK(no_rate.frame().emitters.size() == 1);
}

TEST_CASE("leaf_fall extract: an autumn day adds exactly one emitter with the helper's look") {
    Rig r;
    autumn(r);
    r.add_leaf();
    const Shot f = r.frame();
    REQUIRE(f.emitters.size() == 1);
    CHECK(f.stat == 1);
    const render::ParticleEmitter& e = f.emitters[0];
    CHECK(e.id == kLeafId);

    const Season& s = *r.world.try_get<Season>(r.season_e);
    const f32 phase = season_phase(s, 0, 0.0f);
    const f32 amount = leaf_fall_amount(r.leaf(), &s, phase);
    CHECK(amount == doctest::Approx(1.0f));
    const LeafFallLook l =
        leaf_fall_look(r.leaf(), amount, 0.0f, math::vec2{1.0f, 0.0f}, season_foliage_tint(s, phase));
    CHECK(e.rate == doctest::Approx(l.rate));
    CHECK(e.rate == doctest::Approx(24.0f));
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
    CHECK(e.box.x == doctest::Approx(14.0f));
    CHECK(e.box.y == 0.0f);
    CHECK(e.box.z == doctest::Approx(14.0f));
    CHECK(e.max_particles == 1024u);
    CHECK(e.seed == 1u);
    CHECK_FALSE(e.collide);
    CHECK_FALSE(e.additive);
    CHECK(e.emitting);
    CHECK(e.burst == 0u);
    CHECK(e.texture == ~0u);
    // the terminal fall speed is gravity / drag
    CHECK(-e.gravity.y / e.drag == doctest::Approx(0.9f));

    // authoring flows through
    r.leaf().rate = 100.0f;
    r.leaf().max_particles = 2048;
    r.leaf().seed = 77;
    r.leaf().strength = 0.5f;
    const Shot g = r.frame();
    REQUIRE(g.emitters.size() == 1);
    CHECK(g.emitters[0].rate == doctest::Approx(50.0f));
    CHECK(g.emitters[0].max_particles == 2048u);
    CHECK(g.emitters[0].seed == 77u);
}

TEST_CASE("leaf_fall extract: the id never collides with a ParticleEmitter3D and the emitter follows the camera") {
    Rig r;
    autumn(r);
    r.add_leaf();
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
    u32 leaf_count = 0;
    for (const render::ParticleEmitter& e : f.emitters) {
        if (e.id == kLeafId) {
            ++leaf_count;
            continue;
        }
        bool known = false;
        for (const Entity s : spawned) known = known || e.id == s.index + 1;
        CHECK(known);
    }
    CHECK(leaf_count == 1);
    for (const Entity s : spawned) CHECK(s.index + 1 != kLeafId);

    // above the camera by height_m, then moved with the camera
    const render::ParticleEmitter* e0 = leaf_of(f);
    REQUIRE(e0 != nullptr);
    CHECK(e0->position.x == doctest::Approx(r.cam.position.x));
    CHECK(e0->position.y == doctest::Approx(r.cam.position.y + 7.0f));
    CHECK(e0->position.z == doctest::Approx(r.cam.position.z));

    r.cam.position = {30.0f, 4.0f, -12.0f};
    r.leaf().height_m = 10.0f;
    const Shot m = r.frame();
    const render::ParticleEmitter* e1 = leaf_of(m);
    REQUIRE(e1 != nullptr);
    CHECK(e1->position.x == doctest::Approx(30.0f));
    CHECK(e1->position.y == doctest::Approx(14.0f));
    CHECK(e1->position.z == doctest::Approx(-12.0f));
}

TEST_CASE("leaf_fall extract: the wind drives the drift and its direction turns it") {
    Rig calm;
    autumn(calm);
    calm.add_leaf();
    calm.add_weather(0.0f);
    const render::ParticleEmitter* c = nullptr;
    const Shot fc = calm.frame();
    c = leaf_of(fc);
    REQUIRE(c != nullptr);
    CHECK(c->direction.x == 0.0f);
    CHECK(c->direction.z == 0.0f);
    CHECK(c->direction.y == doctest::Approx(-1.0f));

    Rig windy;
    autumn(windy);
    windy.add_leaf();
    windy.add_weather(0.8f);  // no WindDirection: +x
    const Shot fw = windy.frame();
    const render::ParticleEmitter* w = leaf_of(fw);
    REQUIRE(w != nullptr);
    CHECK(w->direction.x > 0.3f);
    CHECK(w->direction.z == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(w->speed_max > c->speed_max);

    Rig turned;  // 90 degrees counter-clockwise from above: +z in the engine's (x, z) plane
    autumn(turned);
    turned.add_leaf();
    turned.add_weather(0.8f);
    turned.add_wind_direction(90.0f);
    const Shot ft = turned.frame();
    const render::ParticleEmitter* t = leaf_of(ft);
    REQUIRE(t != nullptr);
    const WindDirection& wd = *turned.world.try_get<WindDirection>(turned.wd_e);
    const math::vec2 v = wind_direction_vector(wd);
    const LeafFallLook l = leaf_fall_look(turned.leaf(), 1.0f, 0.8f, v, season_foliage_tint(turned.world.try_get<Season>(turned.season_e), season_phase(*turned.world.try_get<Season>(turned.season_e), 0, 0.0f)));
    CHECK(t->direction.x == doctest::Approx(l.direction.x));
    CHECK(t->direction.z == doctest::Approx(l.direction.z));
    CHECK(std::abs(t->direction.z) > 0.3f);
    CHECK(std::abs(t->direction.x) < 0.01f);

    Rig no_weather;  // a WindDirection alone changes nothing: calm
    autumn(no_weather);
    no_weather.add_leaf();
    no_weather.add_wind_direction(90.0f);
    const Shot fn = no_weather.frame();
    CHECK(same_emitter(*leaf_of(fn), *c));

    Rig no_follow;
    autumn(no_follow);
    no_follow.add_leaf();
    no_follow.leaf().wind_follow = 0.0f;
    no_follow.add_weather(0.8f);
    const Shot fnf = no_follow.frame();
    CHECK(same_emitter(*leaf_of(fnf), *c));
}

TEST_CASE("leaf_fall extract: the colour follows the Season tint") {
    Rig autumn_rig;
    autumn(autumn_rig);
    autumn_rig.add_leaf();
    const Shot fa = autumn_rig.frame();
    const render::ParticleEmitter* a = leaf_of(fa);
    REQUIRE(a != nullptr);
    const Season& s = *autumn_rig.world.try_get<Season>(autumn_rig.season_e);
    const math::vec3 tint = season_foliage_tint(s, season_phase(s, 0, 0.0f));
    const LeafFall& lf = autumn_rig.leaf();
    CHECK(a->color_start.x == doctest::Approx(lf.leaf_color.x * tint.x));
    CHECK(a->color_start.y == doctest::Approx(lf.leaf_color.y * tint.y));
    CHECK(a->color_start.z == doctest::Approx(lf.leaf_color.z * tint.z));

    Rig no_follow;  // tint_follow 0: the authored colour
    autumn(no_follow);
    no_follow.add_leaf();
    no_follow.leaf().tint_follow = 0.0f;
    const Shot fn = no_follow.frame();
    const render::ParticleEmitter* n = leaf_of(fn);
    REQUIRE(n != nullptr);
    CHECK(n->color_start.x == doctest::Approx(0.95f));
    CHECK(n->color_start.y == doctest::Approx(0.5f));
    CHECK(n->color_start.z == doctest::Approx(0.15f));

    Rig manual;  // no Season: manual only, and the colour is untinted
    manual.add_leaf();
    manual.leaf().manual = 0.5f;
    const Shot fm = manual.frame();
    const render::ParticleEmitter* m = leaf_of(fm);
    REQUIRE(m != nullptr);
    CHECK(m->rate == doctest::Approx(12.0f));
    CHECK(m->color_start.x == doctest::Approx(0.95f));
    CHECK(m->color_start.y == doctest::Approx(0.5f));
    CHECK(m->color_start.z == doctest::Approx(0.15f));
}

TEST_CASE("leaf_fall extract: no WorldClock uses the start phase and a day advance changes only the emitter") {
    Rig no_clock;  // day 0 hour 0 at start_phase 0.625: full autumn
    no_clock.add_season(0.625f);
    no_clock.add_leaf();
    const Shot f0 = no_clock.frame();
    REQUIRE(f0.emitters.size() == 1);
    CHECK(f0.emitters[0].rate == doctest::Approx(24.0f));

    Rig r;
    r.add_season(0.625f);
    r.add_clock(0);
    r.add_leaf();
    r.add_weather(0.3f);
    r.add_wind_direction(45.0f);
    ParticleEmitter3D pe;
    const Entity pe_e = r.world.spawn_with(LocalTransform{}, pe);
    const Shot full = r.frame();
    REQUIRE(leaf_of(full) != nullptr);

    const u32 leaf_before = bytes_hash(*r.world.try_get<LeafFall>(r.leaf_e));
    const u32 season_before = bytes_hash(*r.world.try_get<Season>(r.season_e));
    const u32 weather_before = bytes_hash(*r.world.try_get<Weather>(r.weather_e));
    const u32 wd_before = bytes_hash(*r.world.try_get<WindDirection>(r.wd_e));
    const u32 pe_before = bytes_hash(*r.world.try_get<ParticleEmitter3D>(pe_e));
    r.world.try_get_mut<WorldClock>(r.clock_e)->day = 6;  // 12 days a year: phase 0.125, deep winter
    const u32 clock_before = bytes_hash(*r.world.try_get<WorldClock>(r.clock_e));
    const u64 world_before = r.world.state_hash();
    const Shot winter = r.frame();
    CHECK(leaf_of(winter) == nullptr);
    CHECK(winter.emitters.size() == 1);  // only the ParticleEmitter3D remains

    CHECK(bytes_hash(*r.world.try_get<LeafFall>(r.leaf_e)) == leaf_before);
    CHECK(bytes_hash(*r.world.try_get<Season>(r.season_e)) == season_before);
    CHECK(bytes_hash(*r.world.try_get<Weather>(r.weather_e)) == weather_before);
    CHECK(bytes_hash(*r.world.try_get<WindDirection>(r.wd_e)) == wd_before);
    CHECK(bytes_hash(*r.world.try_get<ParticleEmitter3D>(pe_e)) == pe_before);
    CHECK(bytes_hash(*r.world.try_get<WorldClock>(r.clock_e)) == clock_before);
    CHECK(r.world.state_hash() == world_before);

    r.world.try_get_mut<WorldClock>(r.clock_e)->day = 3;  // 12 days a year: phase 0.875
    const Shot mid = r.frame();
    const render::ParticleEmitter* me = leaf_of(mid);
    // whatever the amount is there, the emitter carries the helper's rate (or is absent at 0)
    const Season& s = *r.world.try_get<Season>(r.season_e);
    const f32 amount = leaf_fall_amount(r.leaf(), &s, season_phase(s, 3, 0.0f));
    if (amount > 0.0f) {
        REQUIRE(me != nullptr);
        CHECK(me->rate == doctest::Approx(24.0f * amount));
    } else {
        CHECK(me == nullptr);
    }

    // back to autumn: the same emitter as the first frame, no other state
    r.world.try_get_mut<WorldClock>(r.clock_e)->day = 0;
    const Shot again = r.frame();
    REQUIRE(leaf_of(again) != nullptr);
    CHECK(same_emitter(*leaf_of(again), *leaf_of(full)));
}

TEST_CASE("leaf_fall extract: the Low tier halves the rate and the budget") {
    Rig r;
    autumn(r);
    r.add_leaf();
    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    const Shot high = r.frame();
    r.extract.set_quality(render::quality_settings(render::QualityTier::Medium));
    const Shot medium = r.frame();
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    const Shot low = r.frame();
    REQUIRE(leaf_of(high) != nullptr);
    REQUIRE(leaf_of(medium) != nullptr);
    REQUIRE(leaf_of(low) != nullptr);
    CHECK(same_emitter(*leaf_of(high), *leaf_of(medium)));
    CHECK(leaf_of(low)->rate == doctest::Approx(0.5f * leaf_of(high)->rate));
    CHECK(leaf_of(low)->max_particles == leaf_of(high)->max_particles / 2u);
    CHECK(low.stat == 1);

    // nothing to scale when there are no leaves
    Rig none;
    none.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    CHECK(none.frame().emitters.size() == 0);
}
