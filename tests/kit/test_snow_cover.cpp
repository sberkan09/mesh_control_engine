// SnowCover (M-SNOWCOVER 6c): the kit_snow_cover system advances an integer snow_q out of 2^24 (rises by the snow
// intensity, melts otherwise, no wall clock or RNG), cover = snow_q / 2^24 stays in 0..1, the state is saved and a
// save mid-snow resumes on the same state hash, and Kit3DExtract builds render::SnowCover only when the scene has both
// a Weather and a SnowCover (cover = SnowCover.cover * strength).

#include <doctest/doctest.h>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/snow_cover.h"
#include "mce/kit/weather.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr u64 kMax = 1ull << 24;

u64 mul_div(u64 a, u64 b, u64 c) { return (a / c) * b + (a % c) * b / c; }

struct Rig {
    render::MeshStore store;
    World world;
    Kit3DExtract extract;
    u64 t = 0;
    Rig(bool with_weather, bool with_snow, WeatherKind kind = WeatherKind::Snow, f32 strength = 1.0f) {
        world.register_components<SaveKey>();
        install_kit3d(world, nullptr);
        install_weather(world, nullptr);
        extract.init(store, nullptr);
        world.spawn_with(LocalTransform{}, Camera3DComponent{});
        if (with_weather) {
            Weather w;
            w.target = kind;
            w.transition_seconds = 1.0f;
            world.spawn_with(SaveKey{1}, w);
        }
        if (with_snow) {
            SnowCover sc;
            sc.strength = strength;
            sc.build_seconds = 10.0f;
            sc.melt_seconds = 5.0f;
            sc.coverage = 0.6f;
            sc.noise_scale_m = 5.0f;
            sc.wind_bias = 0.25f;
            world.spawn_with(SaveKey{2}, sc);
        }
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, t++);
    }
    void set_target(WeatherKind k) {
        world.query<Weather&>().each([&](Weather& w) { w.target = k; });
    }
    SnowCover snow() {
        SnowCover s;
        world.query<const SnowCover&>().each([&](const SnowCover& c) { s = c; });
        return s;
    }
    Weather weather() {
        Weather w;
        world.query<const Weather&>().each([&](const Weather& c) { w = c; });
        return w;
    }
    render::SnowCover carrier() {
        render::Camera3D cam;
        REQUIRE(extract.camera(world, 1.0f, cam));
        render::Scene3D s(store, mem::heap());
        s.begin(cam, {320, 180});
        extract.extract(world, s, 1.0f);
        return s.snow_cover();
    }
};

}  // namespace

TEST_CASE("snow_cover: snow_q is integer-exact against the Weather fade") {
    Rig r(true, true);
    u64 q = 0;
    for (u32 i = 0; i < 900; ++i) {
        r.run(1);
        const Weather w = r.weather();
        if (w.current == WeatherKind::Snow && w.fade_den > 0) {
            // build_seconds 10 at 60 Hz = 600 ticks
            q += mul_div(kMax * w.fade, 1, static_cast<u64>(w.fade_den) * 600);
            if (q > kMax) q = kMax;
        }
        REQUIRE(r.snow().snow_q == q);
    }
    const SnowCover s = r.snow();
    CHECK(s.snow_q == kMax);  // 15 s of snow, 10 s to build
    CHECK(s.cover == 1.0f);
}

TEST_CASE("snow_cover: cover rises under snow, melts after, and stays in 0..1") {
    Rig r(true, true);
    f32 prev = 0.0f;
    for (u32 i = 0; i < 480; ++i) {  // 8 s of snow
        r.run(1);
        const f32 c = r.snow().cover;
        CHECK(c >= 0.0f);
        CHECK(c <= 1.0f);
        CHECK(c >= prev);
        prev = c;
    }
    REQUIRE(prev > 0.3f);
    r.set_target(WeatherKind::Clear);
    r.run(61);  // the weather fades out (it still builds a little meanwhile); then current is Clear
    REQUIRE(r.weather().current == WeatherKind::Clear);
    prev = r.snow().cover;
    for (u32 i = 0; i < 900; ++i) {
        r.run(1);
        const f32 c = r.snow().cover;
        CHECK(c >= 0.0f);
        CHECK(c <= 1.0f);
        CHECK(c <= prev);  // only melts once the snow has gone
        prev = c;
    }
    CHECK(r.snow().snow_q == 0);
    CHECK(r.snow().cover == 0.0f);
}

TEST_CASE("snow_cover: a melt is 1 / (melt_seconds / dt) per tick") {
    Rig r(true, true);
    r.run(1200);
    REQUIRE(r.snow().snow_q == kMax);
    r.set_target(WeatherKind::Clear);
    r.run(61);  // fade (60 ticks) completes, current is Clear
    REQUIRE(r.weather().current == WeatherKind::Clear);
    const u32 before = r.snow().snow_q;
    r.run(1);
    CHECK(before - r.snow().snow_q == kMax / 300);  // melt_seconds 5 s = 300 ticks
}

TEST_CASE("snow_cover: a clear scene leaves the cover at 0") {
    Rig r(true, true, WeatherKind::Clear);
    r.run(600);
    CHECK(r.snow().snow_q == 0);
    CHECK(r.snow().cover == 0.0f);
    Rig rain(true, true, WeatherKind::Rain);
    rain.run(600);
    CHECK(rain.snow().snow_q == 0);
    Rig noweather(false, true);
    noweather.run(600);
    CHECK(noweather.snow().snow_q == 0);
}

TEST_CASE("snow_cover: save and load mid-snow resumes with the same state hash") {
    Array<byte> bytes;
    u64 hash_a = 0;
    {
        Rig a(true, true);
        a.run(300);
        REQUIRE(a.snow().snow_q > 0);
        REQUIRE(a.snow().snow_q < kMax);
        save::write_save(a.world, {}, bytes);
        hash_a = a.world.state_hash();
        a.run(200);
        hash_a = a.world.state_hash();
    }
    Rig b(true, true);
    b.run(77);  // a different state, replaced by the save
    REQUIRE(save::read_save(b.world, bytes.span()));
    b.t = 300;
    b.run(200);
    CHECK(b.world.state_hash() == hash_a);
    CHECK(b.snow().snow_q == [&] {
        Rig c(true, true);
        c.run(500);
        return c.snow().snow_q;
    }());
}

TEST_CASE("snow_cover: the extract builds the carrier only with both Weather and SnowCover") {
    Rig both(true, true, WeatherKind::Snow, 0.5f);
    CHECK(both.carrier().cover == 0.0f);  // nothing has fallen yet
    both.run(300);
    const f32 expect = both.snow().cover * 0.5f;
    REQUIRE(expect > 0.05f);
    const render::SnowCover c = both.carrier();
    CHECK(c.cover == doctest::Approx(expect));
    CHECK(c.coverage == doctest::Approx(0.6f));
    CHECK(c.noise_scale == doctest::Approx(5.0f));
    CHECK(c.wind_bias == doctest::Approx(0.25f));
    CHECK(c.noise_enabled);

    Rig only_snow(false, true);
    only_snow.run(300);
    CHECK(only_snow.carrier().cover == 0.0f);

    Rig only_weather(true, false);
    only_weather.run(300);
    CHECK(only_weather.carrier().cover == 0.0f);
    Rig neither(false, false);
    neither.run(300);
    CHECK(neither.carrier().cover == 0.0f);
    CHECK(only_weather.world.state_hash() != 0);

    Rig zero(true, true, WeatherKind::Snow, 0.0f);
    zero.run(300);
    CHECK(zero.carrier().cover == 0.0f);
}

TEST_CASE("snow_cover: the Low tier drops the noise and keeps the cover") {
    Rig r(true, true);
    r.run(300);
    CHECK(r.carrier().noise_enabled);
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    const render::SnowCover c = r.carrier();
    CHECK_FALSE(c.noise_enabled);
    CHECK(c.cover == doctest::Approx(r.snow().cover));
}

TEST_CASE("snow_cover: Weather and a scene without SnowCover keep the same state hash as before the component") {
    Rig a(true, false), b(true, false);
    a.run(600);
    b.run(600);
    CHECK(a.world.state_hash() == b.world.state_hash());
    CHECK(a.carrier().cover == 0.0f);
}
