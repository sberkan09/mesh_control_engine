// WetSurfaces (M-WETSURFACE 5c): Kit3DExtract hands the scene a wetness only when it has both a Weather and a
// WetSurfaces (wetness = Weather.wetness * strength); scenes without either stay dry with an unchanged state hash,
// strength 0 is dry, the Low tier drops the puddles, and the component is saved and hash-stable.

#include <doctest/doctest.h>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/weather.h"
#include "mce/kit/wet_surfaces.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

struct Rig {
    render::MeshStore store;
    World world;
    Kit3DExtract extract;
    u64 t = 0;
    explicit Rig(bool with_weather, bool with_wet, f32 strength = 1.0f) {
        world.register_components<SaveKey>();
        install_kit3d(world, nullptr);
        install_weather(world, nullptr);
        extract.init(store, nullptr);
        world.spawn_with(LocalTransform{}, Camera3DComponent{});
        if (with_weather) {
            Weather w;
            w.target = WeatherKind::Rain;
            w.transition_seconds = 1.0f;
            world.spawn_with(SaveKey{1}, w);
        }
        if (with_wet) {
            WetSurfaces ws;
            ws.strength = strength;
            ws.darken = 0.4f;
            ws.wet_roughness = 0.2f;
            ws.puddles = 0.3f;
            ws.puddle_scale_m = 6.0f;
            world.spawn_with(SaveKey{2}, ws);
        }
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, t++);
    }
    render::WetSurfaces wet() {
        render::Camera3D cam;
        REQUIRE(extract.camera(world, 1.0f, cam));
        render::Scene3D s(store, mem::heap());
        s.begin(cam, {320, 180});
        extract.extract(world, s, 1.0f);
        return s.wet_surfaces();
    }
    f32 weather_wetness() {
        f32 w = -1.0f;
        world.query<const Weather&>().each([&](const Weather& c) { w = c.wetness; });
        return w;
    }
};

}  // namespace

TEST_CASE("wet_surfaces: Weather without WetSurfaces and neither give the same dry scene and hash") {
    Rig a(true, false), b(false, false);
    a.run(1200);  // 20 s of rain: Weather.wetness is well above 0
    b.run(1200);
    REQUIRE(a.weather_wetness() > 0.1f);
    const u64 ha = a.world.state_hash();
    const render::WetSurfaces wa = a.wet(), wb = b.wet();
    CHECK(wa.wetness == 0.0f);
    CHECK(wb.wetness == 0.0f);
    CHECK(wa.darken == wb.darken);
    CHECK(wa.puddles == wb.puddles);
    CHECK(wa.puddle_scale == wb.puddle_scale);
    CHECK(a.world.state_hash() == ha);  // extraction does not touch the world
}

TEST_CASE("wet_surfaces: wetness follows Weather.wetness x strength") {
    Rig r(true, true, 0.5f);
    CHECK(r.wet().wetness == 0.0f);  // dry before it rains
    r.run(1200);
    const f32 expect = r.weather_wetness() * 0.5f;
    REQUIRE(expect > 0.05f);
    const render::WetSurfaces w = r.wet();
    CHECK(w.wetness == doctest::Approx(expect));
    CHECK(w.darken == doctest::Approx(0.4f));
    CHECK(w.roughness == doctest::Approx(0.2f));
    CHECK(w.puddles == doctest::Approx(0.3f));
    CHECK(w.puddle_scale == doctest::Approx(6.0f));
    CHECK(w.puddles_enabled);
}

TEST_CASE("wet_surfaces: WetSurfaces without Weather stays dry; strength 0 is dry") {
    Rig a(false, true);
    a.run(600);
    CHECK(a.wet().wetness == 0.0f);
    Rig b(true, true, 0.0f);
    b.run(1200);
    REQUIRE(b.weather_wetness() > 0.1f);
    CHECK(b.wet().wetness == 0.0f);
}

TEST_CASE("wet_surfaces: the Low tier drops the puddles and keeps the wetness") {
    Rig r(true, true);
    r.run(1200);
    CHECK(r.wet().puddles_enabled);
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    const render::WetSurfaces w = r.wet();
    CHECK_FALSE(w.puddles_enabled);
    CHECK(w.wetness == doctest::Approx(r.weather_wetness()));
}

TEST_CASE("wet_surfaces: saved and loaded, and the state hash is stable") {
    Array<byte> bytes;
    u64 hash = 0;
    {
        Rig a(true, true, 0.75f);
        a.run(300);
        hash = a.world.state_hash();
        CHECK(a.world.state_hash() == hash);
        save::write_save(a.world, {}, bytes);
    }
    Rig b(true, true, 0.1f);  // different authored value, replaced by the save
    b.run(300);
    REQUIRE(save::read_save(b.world, bytes.span()));
    f32 strength = 0.0f;
    b.world.query<const WetSurfaces&>().each([&](const WetSurfaces& c) { strength = c.strength; });
    CHECK(strength == 0.75f);
    CHECK(b.world.state_hash() == hash);
}
