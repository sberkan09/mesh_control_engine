// Extract feed of the frozen water (M-FREEZE 20b): Kit3DExtract applies water_ice_look to the local render::WaterSurface
// (waves scaled, colours mixed toward the ice colour, depth scale lerped, roughness floored, reflectivity and foam scaled)
// before render::apply_quality. No WaterIce, enabled 0, strength 0 and an amount of 0 leave the carrier bit-identical;
// time, wavelength, wave_count, level_y and wind_dir are untouched; Water, Season and WorldClock are never written.

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>

#include "mce/convar.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/season.h"
#include "mce/kit/water.h"
#include "mce/kit/water_ice.h"
#include "mce/kit/weather.h"
#include "mce/kit/world_clock.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"
#include "mce/string_id.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
bool same_vec(const math::vec3& a, const math::vec3& b) { return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z); }
bool same_water(const render::WaterSurface& a, const render::WaterSurface& b) {
    return same_bits(a.level_y, b.level_y) && same_bits(a.half_extent, b.half_extent) && a.wave_count == b.wave_count &&
           same_bits(a.amplitude, b.amplitude) && same_bits(a.wavelength, b.wavelength) &&
           same_bits(a.steepness, b.steepness) && same_bits(a.wind_dir.x, b.wind_dir.x) &&
           same_bits(a.wind_dir.y, b.wind_dir.y) && same_bits(a.time, b.time) && same_bits(a.speed_scale, b.speed_scale) &&
           same_vec(a.shallow_color, b.shallow_color) && same_vec(a.deep_color, b.deep_color) &&
           same_bits(a.depth_scale_m, b.depth_scale_m) && same_bits(a.reflectivity, b.reflectivity) &&
           same_bits(a.roughness, b.roughness) && same_bits(a.foam_amount, b.foam_amount) &&
           same_bits(a.foam_width_m, b.foam_width_m) && a.enabled == b.enabled;
}

// The same carrier apart from `time`, which advances with the extract's frame count in one rig.
bool same_water_any_time(render::WaterSurface a, const render::WaterSurface& b) {
    a.time = b.time;
    return same_water(a, b);
}

template <class T>
u32 bytes_hash(const T& v) {
    u32 h = 2166136261u;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(&v);
    for (usize i = 0; i < sizeof(T); ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

struct Rig {
    render::MeshStore store;
    World world;
    Kit3DExtract extract;
    render::Scene3D scene;
    render::Camera3D cam;
    Entity water_e, ice_e, season_e, clock_e;

    explicit Rig(bool with_water = true) : scene(store, mem::heap()) {
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
        if (with_water) add_water();
    }
    void add_water() {
        Water w;
        w.reflectivity = 0.9f;
        w.roughness = 0.1f;
        w.wave_count = 5;
        w.level_y = 1.5f;
        w.wind_dir_deg = 30.0f;
        w.wind_follow = 0.0f;
        water_e = world.spawn_with(SaveKey{2}, w);
    }
    void add_ice(WaterIce i = {}) { ice_e = world.spawn_with(SaveKey{3}, i); }
    WaterIce& ice() { return *world.try_get_mut<WaterIce>(ice_e); }
    // A Season whose day-0 hour-0 phase is `phase`, 12 days a year.
    void add_season(f32 phase) {
        Season s;
        s.start_phase = phase;
        season_e = world.spawn_with(SaveKey{4}, s);
    }
    void add_clock(u32 day, f32 hour = 0.0f) {
        WorldClock c;
        c.day = day;
        c.hour = hour;
        clock_e = world.spawn_with(SaveKey{5}, c);
    }
    render::WaterSurface frame() {
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
        return scene.water();
    }
};

}  // namespace

TEST_CASE("water ice extract: no WaterIce, enabled 0, strength 0 and an amount of 0 are bit-identical") {
    Rig base;
    const render::WaterSurface a = base.frame();
    REQUIRE(a.enabled);

    Rig off;
    off.add_ice();
    off.ice().manual = 1.0f;
    off.ice().enabled = 0;
    CHECK(same_water(a, off.frame()));

    Rig zero;
    zero.add_ice();
    zero.ice().manual = 1.0f;
    zero.ice().strength = 0.0f;
    CHECK(same_water(a, zero.frame()));

    Rig none;  // no Season, manual 0: nothing drives the ice
    none.add_ice();
    CHECK(same_water(a, none.frame()));

    Rig summer;  // the centre of summer: winter weight 0
    summer.add_ice();
    summer.add_season(0.375f);
    summer.add_clock(0);
    CHECK(same_water(a, summer.frame()));
}

namespace {

// The expected carrier for an authored base carrier, by the helpers (independent of the extract's own code path).
void check_look(const render::WaterSurface& base, const render::WaterSurface& got, const WaterIce& ice, f32 amount) {
    const WaterIceLook l = water_ice_look(ice, amount);
    const math::vec3 c = water_ice_color(ice);
    const f32 m = l.ice_mix;
    CHECK(got.amplitude == doctest::Approx(base.amplitude * l.amplitude_scale));
    CHECK(got.steepness == doctest::Approx(base.steepness * l.steepness_scale));
    CHECK(got.speed_scale == doctest::Approx(base.speed_scale * l.speed_scale));
    CHECK(got.shallow_color.x == doctest::Approx(base.shallow_color.x + (c.x - base.shallow_color.x) * m));
    CHECK(got.shallow_color.y == doctest::Approx(base.shallow_color.y + (c.y - base.shallow_color.y) * m));
    CHECK(got.shallow_color.z == doctest::Approx(base.shallow_color.z + (c.z - base.shallow_color.z) * m));
    CHECK(got.deep_color.x == doctest::Approx(base.deep_color.x + (c.x - base.deep_color.x) * m));
    CHECK(got.deep_color.y == doctest::Approx(base.deep_color.y + (c.y - base.deep_color.y) * m));
    CHECK(got.deep_color.z == doctest::Approx(base.deep_color.z + (c.z - base.deep_color.z) * m));
    CHECK(got.depth_scale_m == doctest::Approx(base.depth_scale_m + (l.depth_scale_m - base.depth_scale_m) * m));
    CHECK(got.roughness == doctest::Approx(std::max(base.roughness, l.roughness_floor)));
    CHECK(got.reflectivity == doctest::Approx(base.reflectivity * l.reflectivity_scale));
    CHECK(got.foam_amount == doctest::Approx(base.foam_amount * l.foam_scale));
    // untouched
    CHECK(same_bits(got.wavelength, base.wavelength));
    CHECK(got.wave_count == base.wave_count);
    CHECK(same_bits(got.level_y, base.level_y));
    CHECK(same_bits(got.half_extent, base.half_extent));
    CHECK(same_bits(got.wind_dir.x, base.wind_dir.x));
    CHECK(same_bits(got.wind_dir.y, base.wind_dir.y));
    CHECK(same_bits(got.foam_width_m, base.foam_width_m));
    CHECK(got.enabled == base.enabled);
}

}  // namespace

TEST_CASE("water ice extract: a full freeze carries the helper's look") {
    Rig base;
    const render::WaterSurface a = base.frame();

    Rig r;
    r.add_ice();
    r.ice().manual = 1.0f;
    const render::WaterSurface f = r.frame();
    check_look(a, f, r.ice(), 1.0f);
    CHECK(same_bits(f.time, a.time));  // both are the first frame of a fresh rig
    CHECK(f.amplitude == 0.0f);
    CHECK(f.steepness == 0.0f);
    CHECK(f.speed_scale == 0.0f);
    CHECK(f.foam_amount == 0.0f);
    CHECK(f.depth_scale_m == doctest::Approx(0.4f));
    CHECK(f.reflectivity == doctest::Approx(0.9f * 0.55f));
    CHECK(f.roughness == doctest::Approx(0.35f));

    Rig half;  // a partial freeze mixes
    half.add_ice();
    half.ice().manual = 0.5f;
    half.ice().calm = 0.6f;
    check_look(a, half.frame(), half.ice(), 0.5f);
}

TEST_CASE("water ice extract: the Season and the day drive the amount, a day advance changes only the carrier") {
    Rig base;
    const render::WaterSurface a = base.frame();

    Rig r;
    r.add_ice();
    r.add_season(0.375f);  // summer at day 0; 12 days a year, winter centre 0.875 is day 6
    r.add_clock(0);
    CHECK(same_water(a, r.frame()));  // first frame

    const u32 water_before = bytes_hash(*r.world.try_get<Water>(r.water_e));
    const u32 season_before = bytes_hash(*r.world.try_get<Season>(r.season_e));
    const u32 ice_before = bytes_hash(*r.world.try_get<WaterIce>(r.ice_e));
    r.world.try_get_mut<WorldClock>(r.clock_e)->day = 6;
    const u32 clock_before = bytes_hash(*r.world.try_get<WorldClock>(r.clock_e));
    const u64 world_before = r.world.state_hash();
    const render::WaterSurface winter = r.frame();
    CHECK_FALSE(same_water_any_time(a, winter));
    const Season& s = *r.world.try_get<Season>(r.season_e);
    check_look(a, winter, r.ice(), water_ice_amount(r.ice(), &s, season_phase(s, 6, 0.0f)));
    CHECK(winter.amplitude == 0.0f);  // the winter centre is a full freeze

    // nothing was written
    CHECK(bytes_hash(*r.world.try_get<Water>(r.water_e)) == water_before);
    CHECK(bytes_hash(*r.world.try_get<Season>(r.season_e)) == season_before);
    CHECK(bytes_hash(*r.world.try_get<WaterIce>(r.ice_e)) == ice_before);
    CHECK(bytes_hash(*r.world.try_get<WorldClock>(r.clock_e)) == clock_before);
    CHECK(r.world.state_hash() == world_before);

    // thaw again: the carrier follows the clock with no other state
    r.world.try_get_mut<WorldClock>(r.clock_e)->day = 0;
    CHECK(same_water_any_time(a, r.frame()));
}

TEST_CASE("water ice extract: the hour moves the amount inside a day") {
    Rig r;
    r.add_ice();
    r.add_season(0.5f);  // autumn/winter border: the winter weight rises with the hour
    r.add_clock(4, 0.0f);
    const render::WaterSurface early = r.frame();
    r.world.try_get_mut<WorldClock>(r.clock_e)->hour = 18.0f;
    const render::WaterSurface late = r.frame();
    const Season& s = *r.world.try_get<Season>(r.season_e);
    CHECK(early.amplitude == doctest::Approx(0.4f * water_ice_look(r.ice(), water_ice_amount(r.ice(), &s, season_phase(s, 4, 0.0f))).amplitude_scale));
    CHECK(late.amplitude == doctest::Approx(0.4f * water_ice_look(r.ice(), water_ice_amount(r.ice(), &s, season_phase(s, 4, 18.0f))).amplitude_scale));
}

TEST_CASE("water ice extract: no WorldClock uses the start phase, no Season uses manual only") {
    Rig base;
    const render::WaterSurface a = base.frame();

    Rig winter;  // no clock: day 0 hour 0 at start_phase 0.875
    winter.add_ice();
    winter.add_season(0.875f);
    const render::WaterSurface w = winter.frame();
    CHECK(w.amplitude == 0.0f);
    const Season& s = *winter.world.try_get<Season>(winter.season_e);
    check_look(a, w, winter.ice(), water_ice_amount(winter.ice(), &s, season_phase(s, 0, 0.0f)));

    Rig manual;  // no Season: a winter clock does nothing, the manual floor does
    manual.add_ice();
    manual.add_clock(6);
    CHECK(same_water(a, manual.frame()));
    manual.ice().manual = 0.25f;
    check_look(a, manual.frame(), manual.ice(), 0.25f);
}

TEST_CASE("water ice extract: no Water still draws nothing") {
    Rig r(false);
    r.add_ice();
    r.ice().manual = 1.0f;
    const render::WaterSurface w = r.frame();
    const render::WaterSurface def;
    CHECK_FALSE(w.enabled);
    CHECK(same_water(w, def));
}

TEST_CASE("water ice extract: the Low tier and r.water 0 still win") {
    Rig r;
    r.add_ice();
    r.ice().manual = 0.5f;
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    const render::WaterSurface low = r.frame();
    CHECK(low.enabled);
    CHECK(low.amplitude == 0.0f);
    CHECK(low.foam_amount == 0.0f);

    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    const render::WaterSurface high = r.frame();
    CHECK(high.amplitude == doctest::Approx(0.4f * 0.5f));

    ConVar* cv = ConVar::find(StringId("r.water"));
    REQUIRE(cv != nullptr);
    cv->set_float(0.0f);
    const bool off = r.frame().enabled;
    cv->reset();
    CHECK_FALSE(off);
    CHECK(r.frame().enabled);
}
