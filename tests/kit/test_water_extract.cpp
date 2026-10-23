// Extract feed of the water surface (M-WATER 15b): Kit3DExtract fills render::WaterSurface from the first Water, the
// WindDirection and the extract's frame time; no Water (or disabled) leaves the Scene3D as before; Environment3D, Fog,
// Sky and the light are never touched.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>

#include "mce/convar.h"
#include "mce/ecs/save.h"
#include "mce/string_id.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/water.h"
#include "mce/kit/weather.h"
#include "mce/kit/wind_direction.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"
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
bool same_scene(const render::Scene3D& a, const render::Scene3D& b) {
    const render::DirectionalLight &la = a.light(), &lb = b.light();
    return same_vec(la.direction, lb.direction) && same_vec(la.color, lb.color) && same_bits(la.intensity, lb.intensity) &&
           same_vec(la.ambient_sky, lb.ambient_sky) && same_vec(la.ambient_ground, lb.ambient_ground) &&
           la.shadows == lb.shadows && same_bits(a.sky().sun_intensity, b.sky().sun_intensity) &&
           same_bits(a.sky().sun_size, b.sky().sun_size) && same_vec(a.sky().horizon, b.sky().horizon) &&
           same_bits(a.fog().density, b.fog().density) && same_vec(a.fog().color, b.fog().color) &&
           same_bits(a.clouds().coverage, b.clouds().coverage) && same_bits(a.night_sky().star_density, b.night_sky().star_density) &&
           same_bits(a.time(), b.time()) && same_water(a.water(), b.water());
}

struct Rig {
    render::MeshStore store;
    World world;
    Kit3DExtract extract;
    render::Scene3D scene;
    render::Camera3D cam;
    Entity water_e;

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
    void add_water(const Water& w = {}) { water_e = world.spawn_with(SaveKey{2}, w); }
    void add_wind(f32 degrees) {
        WindDirection d;
        d.degrees = degrees;
        d.angle_q = static_cast<u32>(degrees * 65536.0f);
        world.spawn_with(SaveKey{3}, d);
    }
    Water& water() { return *world.try_get_mut<Water>(water_e); }
    Environment3D& env() { return world.resource<Environment3D>(); }
    const render::WaterSurface& frame() {
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
        return scene.water();
    }
};

}  // namespace

TEST_CASE("water extract: no Water, disabled or a bad level leaves the Scene3D as before") {
    Rig a;
    a.frame();
    CHECK_FALSE(a.scene.water().enabled);
    const render::WaterSurface def;
    CHECK(same_water(a.scene.water(), def));

    Rig off;
    off.add_water();
    off.water().enabled = 0;
    off.frame();
    CHECK(same_scene(a.scene, off.scene));

    Rig bad;
    bad.add_water();
    bad.water().level_y = std::nanf("");
    bad.frame();
    CHECK(same_scene(a.scene, bad.scene));
}

TEST_CASE("water extract: Environment3D, Fog, Sky and the light are untouched by a Water") {
    Rig a, b;
    b.add_water();
    const Environment3D before = b.env();
    a.frame();
    b.frame();
    CHECK(b.scene.water().enabled);
    CHECK(same_vec(a.scene.light().direction, b.scene.light().direction));
    CHECK(same_vec(a.scene.light().color, b.scene.light().color));
    CHECK(same_bits(a.scene.light().intensity, b.scene.light().intensity));
    CHECK(same_vec(a.scene.light().ambient_sky, b.scene.light().ambient_sky));
    CHECK(same_bits(a.scene.sky().sun_intensity, b.scene.sky().sun_intensity));
    CHECK(same_vec(a.scene.sky().horizon, b.scene.sky().horizon));
    CHECK(same_bits(a.scene.fog().density, b.scene.fog().density));
    CHECK(same_vec(a.scene.fog().color, b.scene.fog().color));
    const Environment3D& after = b.env();
    CHECK(same_bits(after.time_of_day, before.time_of_day));
    CHECK(same_bits(after.sun_intensity, before.sun_intensity));
    CHECK(same_bits(after.fog_density, before.fog_density));
    CHECK(same_bits(after.exposure, before.exposure));
}

TEST_CASE("water extract: the authoring is passed through") {
    Rig r;
    Water w;
    w.level_y = 3.5f;
    w.half_extent = 200.0f;
    w.wave_count = 6;
    w.amplitude = 1.25f;
    w.wavelength = 30.0f;
    w.steepness = 0.3f;
    w.speed_scale = 2.0f;
    w.shallow_color = {0.2f, 0.3f, 0.4f};
    w.deep_color = {0.0f, 0.1f, 0.2f};
    w.depth_scale_m = 12.0f;
    w.reflectivity = 0.5f;
    w.roughness = 0.2f;
    w.foam_amount = 0.7f;
    w.foam_width_m = 2.0f;
    r.add_water(w);
    const render::WaterSurface& s = r.frame();
    CHECK(s.enabled);
    CHECK(s.level_y == doctest::Approx(3.5f));
    CHECK(s.half_extent == doctest::Approx(200.0f));
    CHECK(s.wave_count == 6);
    CHECK(s.amplitude == doctest::Approx(1.25f));
    CHECK(s.wavelength == doctest::Approx(30.0f));
    CHECK(s.steepness == doctest::Approx(0.3f));
    CHECK(s.speed_scale == doctest::Approx(2.0f));
    CHECK(s.shallow_color.y == doctest::Approx(0.3f));
    CHECK(s.deep_color.z == doctest::Approx(0.2f));
    CHECK(s.depth_scale_m == doctest::Approx(12.0f));
    CHECK(s.reflectivity == doctest::Approx(0.5f));
    CHECK(s.roughness == doctest::Approx(0.2f));
    CHECK(s.foam_amount == doctest::Approx(0.7f));
    CHECK(s.foam_width_m == doctest::Approx(2.0f));
}

TEST_CASE("water extract: wind follow uses the WindDirection angle, else the authored one") {
    Rig r;
    Water w;
    w.wind_dir_deg = 90.0f;
    w.wind_follow = 1.0f;
    r.add_water(w);
    r.add_wind(180.0f);
    const render::WaterSurface followed = r.frame();
    CHECK(followed.wind_dir.x == doctest::Approx(-1.0f).epsilon(1e-5));
    CHECK(followed.wind_dir.y == doctest::Approx(0.0f).epsilon(1e-5));

    r.water().wind_follow = 0.0f;
    const render::WaterSurface authored = r.frame();
    CHECK(authored.wind_dir.x == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(authored.wind_dir.y == doctest::Approx(1.0f).epsilon(1e-5));

    Rig none;  // follow on, but the scene has no WindDirection: the authored direction
    Water w2;
    w2.wind_dir_deg = 90.0f;
    none.add_water(w2);
    CHECK(none.frame().wind_dir.y == doctest::Approx(1.0f).epsilon(1e-5));
}

TEST_CASE("water extract: time is the extract's frame time, wrapped") {
    Rig r;
    r.add_water();
    r.extract.set_frame_dt(0.25f);
    const render::WaterSurface& first = r.frame();
    CHECK(first.time == doctest::Approx(0.25f));
    CHECK(same_bits(first.time, r.scene.time()));  // the value set_time got in the same extract
    r.frame();
    CHECK(r.scene.water().time == doctest::Approx(0.5f));
    CHECK(same_bits(r.scene.water().time, r.scene.time()));
    // far past the period: the carrier holds the wrapped value, the scene clock the raw one
    for (int i = 0; i < 400; ++i) r.frame();
    const f32 expect = water_wrap_time(static_cast<f64>(r.scene.time()));
    CHECK(same_bits(r.scene.water().time, expect));
    CHECK(r.scene.water().time < static_cast<f32>(kWaterPeriodSeconds));
    CHECK(r.scene.time() > static_cast<f32>(kWaterPeriodSeconds));
}

TEST_CASE("water extract: the Low tier flattens the plane, r.water 0 removes it") {
    Rig r;
    r.add_water();
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    const render::WaterSurface low = r.frame();
    CHECK(low.enabled);
    CHECK(low.amplitude == 0.0f);
    CHECK(low.foam_amount == 0.0f);

    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    const render::WaterSurface high = r.frame();
    CHECK(high.amplitude == doctest::Approx(0.4f));
    CHECK(high.foam_amount == doctest::Approx(0.5f));

    ConVar* cv = ConVar::find(StringId("r.water"));
    REQUIRE(cv != nullptr);
    cv->set_float(0.0f);
    const bool off = r.frame().enabled;
    cv->reset();
    CHECK_FALSE(off);
    CHECK(r.frame().enabled);
}
