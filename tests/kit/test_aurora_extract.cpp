// Extract feed of the aurora (M-AURORA 16b): Kit3DExtract fills render::AuroraSky from the first Aurora, the night fade
// the night-sky feed uses, the cloud coverage and the extract's frame time; no Aurora (or enabled 0, a closed gate, no
// night, an equirect sky texture) leaves the carrier default and the Scene3D as before; Weather, WorldClock, Moon,
// Water, CloudLayer and Environment3D are never written.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>

#include "mce/containers/array.h"
#include "mce/convar.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/aurora.h"
#include "mce/kit/cloud_layer.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/moon.h"
#include "mce/kit/water.h"
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
bool same_aurora(const render::AuroraSky& a, const render::AuroraSky& b) {
    return same_bits(a.level, b.level) && same_bits(a.base_height, b.base_height) && same_bits(a.height_span, b.height_span) &&
           same_bits(a.azimuth.x, b.azimuth.x) && same_bits(a.azimuth.y, b.azimuth.y) &&
           same_bits(a.half_width_cos, b.half_width_cos) && a.curtain_count == b.curtain_count && same_bits(a.time, b.time) &&
           same_bits(a.speed, b.speed) && same_vec(a.low_color, b.low_color) && same_vec(a.high_color, b.high_color) &&
           a.enabled == b.enabled;
}
bool same_scene(const render::Scene3D& a, const render::Scene3D& b) {
    const render::DirectionalLight &la = a.light(), &lb = b.light();
    return same_vec(la.direction, lb.direction) && same_vec(la.color, lb.color) && same_bits(la.intensity, lb.intensity) &&
           same_vec(la.ambient_sky, lb.ambient_sky) && same_vec(la.ambient_ground, lb.ambient_ground) &&
           la.shadows == lb.shadows && same_bits(a.sky().sun_intensity, b.sky().sun_intensity) &&
           same_bits(a.sky().sun_size, b.sky().sun_size) && same_vec(a.sky().horizon, b.sky().horizon) &&
           same_bits(a.fog().density, b.fog().density) && same_vec(a.fog().color, b.fog().color) &&
           same_bits(a.clouds().coverage, b.clouds().coverage) &&
           same_bits(a.night_sky().night_fade, b.night_sky().night_fade) &&
           same_bits(a.night_sky().star_density, b.night_sky().star_density) && same_bits(a.water().level_y, b.water().level_y) &&
           a.water().enabled == b.water().enabled && same_bits(a.time(), b.time()) && same_aurora(a.aurora(), b.aurora());
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
    Entity aurora_e;

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
        REQUIRE(extract.camera(world, 1.0f, cam));
    }
    void add_aurora(const Aurora& a = {}) { aurora_e = world.spawn_with(SaveKey{2}, a); }
    void add_weather(WeatherKind k, f32 intensity) {
        Weather w;
        w.current = k;
        w.target = k;
        w.intensity = intensity;
        world.spawn_with(SaveKey{3}, w);
    }
    void add_clouds(const CloudLayer& l = {}) { world.spawn_with(SaveKey{4}, l); }
    Aurora& aurora() { return *world.try_get_mut<Aurora>(aurora_e); }
    Environment3D& env() { return world.resource<Environment3D>(); }
    const render::AuroraSky& frame() {
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
        return scene.aurora();
    }
};

}  // namespace

TEST_CASE("aurora extract: no Aurora gives the default carrier and the Scene3D as before") {
    Rig a;
    a.frame();
    const render::AuroraSky def;
    CHECK(same_aurora(a.scene.aurora(), def));
    CHECK(a.scene.aurora().level == 0.0f);

    Rig off;
    off.add_aurora();
    off.aurora().enabled = 0;
    off.frame();
    CHECK(same_scene(a.scene, off.scene));
    CHECK(same_aurora(off.scene.aurora(), def));

    Rig nan;
    nan.add_aurora();
    nan.aurora().azimuth_deg = std::nanf("");
    nan.frame();
    CHECK(same_scene(a.scene, nan.scene));
}

TEST_CASE("aurora extract: an Aurora changes only the aurora carrier of the Scene3D") {
    Rig a, b;
    b.add_aurora();
    a.frame();
    b.frame();
    CHECK(b.scene.aurora().level > 0.0f);
    CHECK(b.scene.aurora().enabled);
    // everything else compares equal once the carriers are aligned
    b.scene.set_aurora(a.scene.aurora());
    CHECK(same_scene(a.scene, b.scene));
}

TEST_CASE("aurora extract: day gives level 0 with night_only, midnight the helper's level") {
    Rig r;
    r.add_aurora();
    const Aurora a = r.aurora();
    CHECK(r.frame().level == doctest::Approx(aurora_level(a, 1.0f, 0.0f)));
    CHECK(r.scene.aurora().level == doctest::Approx(1.0f));  // default latitude 68 is past the gate
    r.env().time_of_day = 12.0f;
    CHECK(r.frame().level == 0.0f);
    r.aurora().night_only = 0;  // forced: shows by day too
    CHECK(r.frame().level == doctest::Approx(aurora_level(r.aurora(), 0.0f, 0.0f)));
    r.aurora().night_only = 1;
    // dusk: the fade is the one the night sky uses
    r.env().time_of_day = 18.5f;
    const f32 dusk = r.frame().level;
    r.add_aurora();  // (second Aurora is ignored: the first in ECS order is read)
    CHECK(r.frame().level == doctest::Approx(dusk));
}

TEST_CASE("aurora extract: the night fade is the night sky's dark term, not its fog or cloud terms") {
    Rig r;
    r.add_aurora();
    Moon m;
    r.world.spawn_with(SaveKey{5}, m);
    for (f32 h : {16.0f, 18.0f, 19.0f, 20.0f, 22.0f}) {
        r.env().time_of_day = h;
        r.frame();
        // the night sky's night_fade is dark * clear * unfogged; with no clouds and no fog it is the dark term itself
        CHECK(r.scene.aurora().level == doctest::Approx(aurora_level(r.aurora(), r.scene.night_sky().night_fade, 0.0f)));
    }
}

TEST_CASE("aurora extract: no clock means no night fade") {
    Rig r;
    r.add_aurora();
    r.env().time_of_day = -1.0f;
    CHECK(r.frame().level == 0.0f);  // night_only 1
    r.aurora().night_only = 0;
    CHECK(r.frame().level == doctest::Approx(1.0f));
}

TEST_CASE("aurora extract: overcast lowers the level by the cloud coverage the clouds block set") {
    Rig clear;
    clear.add_aurora();
    const f32 full = clear.frame().level;

    Rig r;
    r.add_aurora();
    r.add_weather(WeatherKind::Overcast, 1.0f);
    r.add_clouds();
    const f32 covered = r.frame().level;
    const f32 cov = r.scene.clouds().coverage;
    REQUIRE(cov > 0.0f);
    CHECK(covered < full);
    CHECK(covered == doctest::Approx(aurora_level(r.aurora(), 1.0f, cov)));
}

TEST_CASE("aurora extract: a closed latitude gate sets nothing") {
    Rig a, b;
    b.add_aurora();
    b.aurora().latitude_deg = 30.0f;
    a.frame();
    b.frame();
    CHECK(same_scene(a.scene, b.scene));
    b.aurora().latitude_deg = -30.0f;
    a.frame();  // the scene clock advances once per frame: keep both at the same frame
    b.frame();
    CHECK(same_scene(a.scene, b.scene));
    b.aurora().latitude_deg = -80.0f;  // the sign only mirrors: south is open too
    CHECK(b.frame().level > 0.0f);
    Rig z;
    z.add_aurora();
    z.aurora().strength = 0.0f;
    CHECK(z.frame().level == 0.0f);
}

TEST_CASE("aurora extract: an equirect sky texture suppresses the aurora") {
    Rig r;
    r.add_aurora();
    CHECK(r.frame().level > 0.0f);
    r.env().sky_texture.guid = asset::Guid{1, 2};
    const render::AuroraSky& s = r.frame();
    const render::AuroraSky def;
    CHECK(same_aurora(s, def));  // also clears last frame's carrier
}

TEST_CASE("aurora extract: time is the extract's frame time, wrapped") {
    Rig r;
    r.add_aurora();
    r.extract.set_frame_dt(0.25f);
    const render::AuroraSky& first = r.frame();
    CHECK(first.time == doctest::Approx(0.25f));
    CHECK(same_bits(first.time, r.scene.time()));  // the value set_time got in the same extract
    r.frame();
    CHECK(r.scene.aurora().time == doctest::Approx(0.5f));
    CHECK(same_bits(r.scene.aurora().time, r.scene.time()));
    for (int i = 0; i < 400; ++i) r.frame();  // past the 64 s period
    const f32 expect = aurora_wrap_time(static_cast<f64>(r.scene.time()));
    CHECK(same_bits(r.scene.aurora().time, expect));
    CHECK(r.scene.aurora().time < static_cast<f32>(kAuroraPeriodSeconds));
    CHECK(r.scene.time() > static_cast<f32>(kAuroraPeriodSeconds));
}

TEST_CASE("aurora extract: the Low tier and r.aurora 0 zero the level") {
    Rig r;
    r.add_aurora();
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    CHECK(r.frame().level == 0.0f);
    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    CHECK(r.frame().level > 0.0f);

    ConVar* cv = ConVar::find(StringId("r.aurora"));
    REQUIRE(cv != nullptr);
    cv->set_float(0.0f);
    const f32 off = r.frame().level;
    cv->reset();
    CHECK(off == 0.0f);
    CHECK(r.frame().level > 0.0f);
}

TEST_CASE("aurora extract: the authoring is passed through; azimuth 0 = +z, counter-clockwise from above") {
    Rig r;
    Aurora a;
    a.base_height = 0.25f;
    a.height_span = 0.5f;
    a.curtain_count = 5;
    a.speed = 2.0f;
    a.low_color = {0.2f, 0.3f, 0.4f};
    a.high_color = {0.5f, 0.6f, 0.7f};
    a.strength = 2.0f;
    a.azimuth_deg = 0.0f;
    a.width_deg = 60.0f;
    r.add_aurora(a);
    const render::AuroraSky s0 = r.frame();
    CHECK(s0.enabled);
    CHECK(s0.level == doctest::Approx(2.0f));
    CHECK(s0.base_height == doctest::Approx(0.25f));
    CHECK(s0.height_span == doctest::Approx(0.5f));
    CHECK(s0.curtain_count == 5);
    CHECK(s0.speed == doctest::Approx(2.0f));
    CHECK(s0.low_color.y == doctest::Approx(0.3f));
    CHECK(s0.high_color.z == doctest::Approx(0.7f));
    // (x, z) = (sin, cos) of azimuth_deg: 0 -> +z, 90 -> +x, 180 -> -z, 270 -> -x
    CHECK(s0.azimuth.x == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(s0.azimuth.y == doctest::Approx(1.0f).epsilon(1e-5));
    CHECK(s0.half_width_cos == doctest::Approx(std::cos(30.0 * 3.14159265358979323846 / 180.0)).epsilon(1e-5));
    r.aurora().azimuth_deg = 90.0f;
    CHECK(r.frame().azimuth.x == doctest::Approx(1.0f).epsilon(1e-5));
    CHECK(r.scene.aurora().azimuth.y == doctest::Approx(0.0f).epsilon(1e-5));
    r.aurora().azimuth_deg = 180.0f;
    CHECK(r.frame().azimuth.y == doctest::Approx(-1.0f).epsilon(1e-5));
    r.aurora().azimuth_deg = 270.0f;
    CHECK(r.frame().azimuth.x == doctest::Approx(-1.0f).epsilon(1e-5));
    r.aurora().width_deg = 180.0f;  // a half circle: cos(90 deg) = 0
    CHECK(r.frame().half_width_cos == doctest::Approx(0.0f).epsilon(1e-5));
    r.aurora().width_deg = 10.0f;
    CHECK(r.frame().half_width_cos == doctest::Approx(std::cos(5.0 * 3.14159265358979323846 / 180.0)).epsilon(1e-5));
}

TEST_CASE("aurora extract: Weather, WorldClock, Moon, Water, CloudLayer and Environment3D are never written") {
    const auto build = [](Rig& r, bool with_aurora) {
        r.add_weather(WeatherKind::Overcast, 0.5f);
        r.add_clouds();
        r.world.spawn_with(SaveKey{5}, Moon{});
        r.world.spawn_with(SaveKey{6}, Water{});
        if (with_aurora) r.add_aurora();
    };
    const auto hashes = [](Rig& r) {
        u32 h[5] = {};
        r.world.query<const Weather&>().each([&](const Weather& c) { h[0] = bytes_hash(c); });
        r.world.query<const WorldClock&>().each([&](const WorldClock& c) { h[1] = bytes_hash(c); });
        r.world.query<const Moon&>().each([&](const Moon& c) { h[2] = bytes_hash(c); });
        r.world.query<const Water&>().each([&](const Water& c) { h[3] = bytes_hash(c); });
        r.world.query<const CloudLayer&>().each([&](const CloudLayer& c) { h[4] = bytes_hash(c); });
        u32 x = 2166136261u;
        for (const u32 v : h) x = (x ^ v) * 16777619u;
        return x;
    };
    Rig a, b;
    build(a, false);
    build(b, true);
    const u32 env_before = bytes_hash(b.env());
    for (int i = 0; i < 3; ++i) {
        a.frame();
        b.frame();
    }
    CHECK(b.scene.aurora().level > 0.0f);
    CHECK(hashes(a) == hashes(b));
    CHECK(bytes_hash(b.env()) == env_before);
    CHECK(bytes_hash(a.env()) == bytes_hash(b.env()));
}
