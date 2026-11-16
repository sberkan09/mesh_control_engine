// Extract feed of the night sky (M-NIGHTSKY 14e): Kit3DExtract fills render::NightSky from the Moon, the clock, the cloud
// coverage and the fog; no Moon (or disabled, no clock, an equirect sky texture) leaves it off and the Scene3D as before;
// the direction handed to the sky is the one the light uses; the sky pass's own sun disk is dropped only while the moon
// holds the light.

#include <doctest/doctest.h>

#include <bit>
#include <algorithm>
#include <cmath>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/cloud_layer.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/moon.h"
#include "mce/kit/weather.h"
#include "mce/kit/world_clock.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
bool same_vec(const math::vec3& a, const math::vec3& b) { return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z); }
bool same_night(const render::NightSky& a, const render::NightSky& b) {
    return same_vec(a.moon_dir, b.moon_dir) && same_bits(a.moon_cos_size, b.moon_cos_size) &&
           same_bits(a.moon_intensity, b.moon_intensity) && same_vec(a.moon_color, b.moon_color) &&
           same_bits(a.moon_phase, b.moon_phase) && same_bits(a.star_density, b.star_density) &&
           same_bits(a.star_brightness, b.star_brightness) && same_bits(a.star_twinkle, b.star_twinkle) &&
           same_bits(a.star_time, b.star_time) && same_bits(a.night_fade, b.night_fade) && a.enabled == b.enabled;
}
bool same_scene(const render::Scene3D& a, const render::Scene3D& b) {
    const render::DirectionalLight &la = a.light(), &lb = b.light();
    return same_vec(la.direction, lb.direction) && same_vec(la.color, lb.color) && same_bits(la.intensity, lb.intensity) &&
           same_vec(la.ambient_sky, lb.ambient_sky) && same_vec(la.ambient_ground, lb.ambient_ground) &&
           la.shadows == lb.shadows && same_bits(a.sky().sun_intensity, b.sky().sun_intensity) &&
           same_bits(a.sky().sun_size, b.sky().sun_size) && same_bits(a.fog().density, b.fog().density) &&
           same_bits(a.clouds().coverage, b.clouds().coverage) && same_night(a.night_sky(), b.night_sky());
}

struct Rig {
    render::MeshStore store;
    World world;
    Kit3DExtract extract;
    render::Scene3D scene;
    render::Camera3D cam;
    Entity moon_e;

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
        world.spawn_with(SaveKey{1}, WorldClock{});
        env().time_of_day = 0.0f;  // midnight
        env().sun_intensity = 0.05f;
        REQUIRE(extract.camera(world, 1.0f, cam));
    }
    void add_moon(const Moon& m = {}) { moon_e = world.spawn_with(SaveKey{2}, m); }
    void add_weather(WeatherKind k, f32 intensity) {
        Weather w;
        w.current = k;
        w.target = k;
        w.intensity = intensity;
        world.spawn_with(SaveKey{3}, w);
    }
    void add_clouds(const CloudLayer& l = {}) { world.spawn_with(SaveKey{4}, l); }
    Moon& moon() { return *world.try_get_mut<Moon>(moon_e); }
    Environment3D& env() { return world.resource<Environment3D>(); }
    const render::NightSky& frame() {
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
        return scene.night_sky();
    }
};

}  // namespace

TEST_CASE("night sky extract: no Moon leaves the sky off and the Scene3D untouched") {
    Rig plain;
    const render::NightSky& n = plain.frame();
    CHECK(n.star_density == 0.0f);
    CHECK(n.moon_intensity == 0.0f);
    for (const f32 h : {0.0f, 6.0f, 12.0f, 20.0f, -1.0f}) {
        Rig a, b;
        a.env().time_of_day = h;
        b.env().time_of_day = h;
        b.add_moon();
        b.moon().enabled = 0;  // present but off: the same scene as none
        a.frame();
        b.frame();
        CHECK(same_scene(a.scene, b.scene));
        CHECK(a.scene.night_sky().star_density == 0.0f);
        CHECK(a.scene.night_sky().moon_intensity == 0.0f);
    }
}

TEST_CASE("night sky extract: day fades the night sky out and midnight to full") {
    Rig r;
    r.add_moon();
    const render::NightSky night = r.frame();
    CHECK(night.night_fade == doctest::Approx(1.0f));
    CHECK(night.star_density == doctest::Approx(0.6f));
    CHECK(night.moon_intensity == doctest::Approx(6.0f));
    r.env().time_of_day = 12.0f;
    CHECK(r.frame().night_fade == doctest::Approx(0.0f));
    // dusk and dawn: monotone across the ramp
    f32 prev = 0.0f;
    bool mid = false;
    for (f32 h = 14.0f; h <= 22.0f; h += 0.05f) {
        r.env().time_of_day = h;
        const f32 f = r.frame().night_fade;
        CHECK(f >= prev - 1e-6f);  // the sun sets: the fade only grows
        prev = f;
        if (f > 0.05f && f < 0.95f) mid = true;
    }
    CHECK(mid);
}

TEST_CASE("night sky extract: the fields come from the Moon, the phase and the clock") {
    Rig r;
    Moon m;
    m.freeze_phase = 0.3f;
    m.disk_size = 0.998f;
    m.disk_intensity = 4.0f;
    m.light_color = {0.2f, 0.4f, 0.8f};
    m.star_density = 0.25f;
    m.star_brightness = 2.0f;
    m.star_twinkle = 0.7f;
    r.add_moon(m);
    r.extract.set_frame_dt(0.5f);
    r.frame();
    const render::NightSky& n = r.frame();  // two frames: time 1.0 s
    CHECK(n.moon_phase == doctest::Approx(0.3f));
    CHECK(n.moon_cos_size == doctest::Approx(0.998f));
    CHECK(n.moon_intensity == doctest::Approx(4.0f));
    CHECK(same_vec(n.moon_color, m.light_color));
    CHECK(n.star_density == doctest::Approx(0.25f));
    CHECK(n.star_brightness == doctest::Approx(2.0f));
    CHECK(n.star_twinkle == doctest::Approx(0.7f));
    CHECK(n.star_time == doctest::Approx(1.0f));
    CHECK(same_bits(n.star_time, r.scene.time()));  // the clock the scene ends up with
    const math::vec3 want = -moon_direction(m, 0.0f, moon_phase(m, 0, 0.0f));
    CHECK(same_vec(n.moon_dir, want));  // unit already; apply_quality normalises
    CHECK(n.enabled);
}

TEST_CASE("night sky extract: no clock, disabled and an equirect sky texture leave it off") {
    Rig r;
    r.add_moon();
    r.env().time_of_day = -1.0f;
    CHECK(r.frame().star_density == 0.0f);
    CHECK(r.scene.night_sky().moon_intensity == 0.0f);
    r.env().time_of_day = 0.0f;
    CHECK(r.frame().star_density > 0.0f);
    r.moon().enabled = 0;
    CHECK(r.frame().star_density == 0.0f);
    r.moon().enabled = 1;
    r.env().sky_texture.guid = asset::Guid{1, 2};  // an equirect map wins, as with the clouds
    CHECK(r.frame().star_density == 0.0f);
    CHECK(r.scene.night_sky().moon_intensity == 0.0f);
    r.env().sky_texture.guid = asset::Guid{};
    CHECK(r.frame().star_density > 0.0f);
}

TEST_CASE("night sky extract: cloud coverage and fog dim the night sky") {
    Rig clear;
    clear.add_moon();
    CHECK(clear.frame().night_fade == doctest::Approx(1.0f));

    Rig cloudy;  // fair coverage 0.35: fade 0.65
    cloudy.add_moon();
    cloudy.add_weather(WeatherKind::Clear, 0.0f);
    cloudy.add_clouds();
    CHECK(cloudy.frame().night_fade == doctest::Approx(0.65f));

    Rig overcast;  // overcast coverage 0.9: fade 0.1
    overcast.add_moon();
    overcast.add_weather(WeatherKind::Overcast, 1.0f);
    overcast.add_clouds();
    const f32 oc = overcast.frame().night_fade;
    CHECK(oc == doctest::Approx(0.1f));
    CHECK(oc < cloudy.scene.night_sky().night_fade);

    Rig solid;  // coverage 1: stars and moon gone
    solid.add_moon();
    solid.add_weather(WeatherKind::Overcast, 1.0f);
    CloudLayer full;
    full.overcast_coverage = 1.0f;
    solid.add_clouds(full);
    CHECK(solid.frame().night_fade == 0.0f);

    // the Low tier turns the clouds off, so they stop dimming
    cloudy.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    CHECK(cloudy.frame().night_fade == doctest::Approx(1.0f));

    Rig foggy;  // fog density d: unfogged = 1 - smoothstep(0, 0.05, d)
    foggy.add_moon();
    foggy.env().fog_density = 0.025f;
    CHECK(foggy.frame().night_fade == doctest::Approx(0.5f));
    foggy.env().fog_density = 0.0125f;
    const f32 light_fog = foggy.frame().night_fade;
    CHECK(light_fog > 0.5f);
    CHECK(light_fog < 1.0f);
    foggy.env().fog_density = 0.05f;
    CHECK(foggy.frame().night_fade == doctest::Approx(0.0f));
    foggy.env().fog_density = 0.2f;
    CHECK(foggy.frame().night_fade == doctest::Approx(0.0f));

    Rig both;  // the factors multiply
    both.add_moon();
    both.add_weather(WeatherKind::Clear, 0.0f);
    both.add_clouds();
    both.env().fog_density = 0.025f;
    CHECK(both.frame().night_fade == doctest::Approx(0.65f * 0.5f));
}

TEST_CASE("night sky extract: the moon direction handed to the sky is the one the light uses") {
    Rig r;
    r.add_moon();
    for (const f32 h : {0.0f, 2.0f, 21.0f, 23.5f}) {
        r.env().time_of_day = h;
        const render::NightSky& n = r.frame();
        const render::DirectionalLight& l = r.scene.light();
        if (!same_vec(l.color, r.moon().light_color)) continue;  // the sun holds the light at this hour
        CHECK(same_vec(n.moon_dir, -l.direction));
        CHECK(n.moon_dir.y > 0.0f);  // toward a moon that is up
    }
    // a moon that is up while the sun still holds the light: the sky still gets the moon's own direction
    r.env().time_of_day = 18.0f;
    r.env().sun_intensity = 2.0f;
    const render::NightSky& n = r.frame();
    const math::vec3 want = -moon_direction(r.moon(), 18.0f, moon_phase(r.moon(), 0, 18.0f));
    CHECK(same_vec(n.moon_dir, want));
}

TEST_CASE("night sky extract: the Low tier drops the stars and keeps the moon") {
    Rig r;
    r.add_moon();
    CHECK(r.frame().star_density > 0.0f);
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    const render::NightSky& n = r.frame();
    CHECK(n.star_density == 0.0f);
    CHECK(n.star_twinkle == 0.0f);
    CHECK(n.moon_intensity > 0.0f);
}

TEST_CASE("night sky extract: the sun disk is dropped only while the moon holds the light") {
    Rig plain;
    plain.env().sky_sun_intensity = 12.0f;
    Rig r;
    r.env().sky_sun_intensity = 12.0f;
    r.add_moon();
    // midnight, full moon: the moon holds the light, so no flat second disk on the phased moon
    plain.frame();
    r.frame();
    REQUIRE(same_vec(r.scene.light().color, r.moon().light_color));
    CHECK(plain.scene.sky().sun_intensity == 12.0f);
    CHECK(r.scene.sky().sun_intensity == 0.0f);
    CHECK(r.scene.sky().sun_size == plain.scene.sky().sun_size);
    CHECK(r.env().sky_sun_intensity == 12.0f);  // Environment3D untouched
    // a new moon never takes the light: the disk stays
    r.moon().freeze_phase = 0.0f;
    r.frame();
    CHECK(r.scene.sky().sun_intensity == 12.0f);
    r.moon().freeze_phase = -1.0f;
    // by day the whole scene is bit-identical to one without a Moon, hour by hour
    for (f32 h = 6.0f; h <= 17.0f; h += 0.25f) {
        plain.env().time_of_day = h;
        r.env().time_of_day = h;
        plain.env().sun_intensity = 2.0f;
        r.env().sun_intensity = 2.0f;
        plain.frame();
        r.frame();
        CHECK(same_bits(r.scene.sky().sun_intensity, 12.0f));
        CHECK(same_vec(r.scene.light().direction, plain.scene.light().direction));
        CHECK(same_bits(r.scene.light().intensity, plain.scene.light().intensity));
    }
}
