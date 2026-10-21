// M-WEATHER 4: samples/12_weather. The sample's own assets are cooked and played with the stock systems and the scene
// script, as mce_player does. The weather has 10 s transitions (600 ticks) and the script calls weather.set('rain') at
// tick 60, 'storm' at 600, 'snow' at 1500 and 'clear' at 2100, so (all ticks are the tick number given to
// World::tick; the Weather system runs before the script of the same tick):
//   rain   fades in from tick 61 (intensity (t - 60) / 600), is 0.9 at tick 600 when the storm is called;
//   the rain fades out (539 .. 0 at tick 1139), the storm is in effect from tick 1140 and has reached 361 / 600 when
//   snow is called at 1500; it fades out and the snow is in effect from tick 1861, 240 / 600 = 0.4 at tick 2100 when
//   clear is called; the snow is gone (current clear) at tick 2340.
// Pinned: the sun at tick 0 and the clear scene bit-equal to a control without the forecast, the intensity bits and the
// emitter (rate, gravity) in rain, storm and snow, the storm darker than clear, the lightning strikes of the pinned
// seed (only inside the storm), the environment back to exactly the control's after the run, and the same run twice
// has the same state hash. (The player-level run is samples/12_weather/weather_test.cmake.)

#include <doctest/doctest.h>

#include <bit>
#include <filesystem>
#include <string>

#include "asset/cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/ecs/transform.h"
#include "mce/kit/day_cycle.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/precipitation.h"
#include "mce/kit/stock.h"
#include "mce/kit/cloud_layer.h"
#include "mce/kit/rain_ripples.h"
#include "mce/kit/rain_splash.h"
#include "mce/kit/snow_cover.h"
#include "mce/kit/thunder_delay.h"
#include "mce/kit/weather.h"
#include "mce/kit/wind_direction.h"
#include "mce/kit/wet_surfaces.h"
#include "mce/kit/wind_sway.h"
#include "mce/kit/world_clock.h"
#include "mce/scene/scene.h"
#include "mce/script/compiler.h"
#include "mce/script/scene_scripts.h"

using namespace mce;
using namespace cook_test;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr const char* kScene = "12d12000-0000-4000-8000-000000000001";
// Entities in the scene's order.
constexpr u32 kSky = 1, kClock = 2, kCamera = 3, kWeather = 4, kPrecip = 5, kLightning = 6, kScript = 10, kSplash = 11;

u32 bits(f32 v) { return std::bit_cast<u32>(v); }

const std::string& project() {
    static const std::string root = [] {
        const std::string r = fresh_dir("weather_demo_project");
        std::filesystem::copy(std::string(MCE_SOURCE_DIR) + "/samples/12_weather/assets", r + "/assets",
                              std::filesystem::copy_options::recursive);
        cook::CookService svc;
        const cook::CookReport rep = svc.cook(StringView(r.c_str()), cook::CookOptions{});
        std::string diags;
        for (const cook::Diagnostic& d : rep.diags.all()) {
            cook::String line;
            cook::format_diagnostic(d, line);
            diags += std::string(line.view().data(), line.view().size()) + "\n";
        }
        INFO(diags);
        REQUIRE(rep.ok());
        return r;
    }();
    return root;
}

// The scene with its forecast script; `forecast` false removes the script, so the weather stays clear: the control.
struct Demo {
    asset::AssetSystem assets;
    World world;
    Array<Entity> e;
    u64 ticks = 0;
    u32 strikes = 0;
    u64 first_strike = 0, last_strike = 0;

    explicit Demo(bool forecast = true) {
        const std::string cooked = project() + "/cooked";
        const StringView roots[] = {StringView(cooked.c_str())};
        asset::MountOptions own;
        own.allow_code = true;
        REQUIRE(assets.mount(Span<const StringView>(roots, 1), own));
        kit::install_stock(world, &assets);
        script::install_scene_scripts(world, &assets);
        REQUIRE(scene::load(world, assets, asset::parse_guid(kScene).value(), &e));
        REQUIRE(e.size() == 12);
        if (!forecast) world.destroy(e[kScript]);
    }
    void run_to(u64 tick) {
        while (ticks < tick) {
            world.tick(kDt, ++ticks);
            auto ev = EventAccess(world);
            for (const kit::LightningStruck& s : ev.read<kit::LightningStruck>()) {
                (void)s;
                if (strikes == 0) first_strike = ticks;
                last_strike = ticks;
                strikes += 1;
            }
        }
    }
    const kit::SnowCover& snow() { return *world.try_get<kit::SnowCover>(e[kWeather]); }
    const kit::Weather& wx() { return *world.try_get<kit::Weather>(e[kWeather]); }
    const kit::Environment3D& env() { return *world.try_resource<kit::Environment3D>(); }
    const kit::ParticleEmitter3D& em() { return *world.try_get<kit::ParticleEmitter3D>(e[kPrecip]); }
};

}  // namespace

TEST_CASE("weather demo: the sample's assets cook") { CHECK(!project().empty()); }

TEST_CASE("weather demo: the scene loads with a noon clock, a Weather and an emitter, and the sun is the authored one at tick 0") {
    Demo d;
    CHECK(d.world.try_get<kit::WorldClock>(d.e[kClock])->hour == 12.0f);
    CHECK(d.world.try_get<kit::DayCycle>(d.e[kClock]) != nullptr);
    CHECK(d.world.try_get<kit::Precipitation3D>(d.e[kPrecip]) != nullptr);
    CHECK(d.world.try_get<kit::Lightning>(d.e[kLightning]) != nullptr);
    CHECK(d.world.try_get<kit::Camera3DComponent>(d.e[kCamera]) != nullptr);
    CHECK(d.world.try_get<kit::Environment3D>(d.e[kSky])->atmosphere == 0);
    CHECK(d.wx().transition_seconds == 10.0f);
    CHECK(d.wx().target == kit::WeatherKind::Clear);
    // M-WETSURFACE 5d: the Weather entity also carries the WetSurfaces (strength 4 lifts the slow wetness).
    const kit::WetSurfaces* wet = d.world.try_get<kit::WetSurfaces>(d.e[kWeather]);
    REQUIRE(wet != nullptr);
    CHECK(wet->strength == 4.0f);
    CHECK(wet->darken == 0.5f);
    CHECK(wet->puddles == 0.6f);
    CHECK(wet->puddle_scale_m == 4.0f);
    // M-WINDSWAY 2: ... and a WindSway with the default gains plus a Foliage3D patch to sway.
    const kit::WindSway* sway = d.world.try_get<kit::WindSway>(d.e[kWeather]);
    REQUIRE(sway != nullptr);
    CHECK(sway->amplitude_gain == 4.0f);
    CHECK(sway->frequency_gain == 1.5f);
    const kit::Foliage3D* foliage = d.world.try_get<kit::Foliage3D>(d.e[kWeather]);
    REQUIRE(foliage != nullptr);
    CHECK(foliage->count == 60);
    CHECK(foliage->wind_amplitude == 0.08f);
    // M-SNOWCOVER 6d: ... and a SnowCover that builds in 4 s and melts in 10 s (the weather is only snow for 8 s).
    const kit::SnowCover* snow = d.world.try_get<kit::SnowCover>(d.e[kWeather]);
    REQUIRE(snow != nullptr);
    CHECK(snow->build_seconds == 4.0f);
    CHECK(snow->melt_seconds == 10.0f);
    CHECK(snow->strength == 1.0f);
    CHECK(snow->cover == 0.0f);
    // M-RAINFX 7d: ... a RainRipples and a ThunderDelay (no clip: the sample has no sfx asset) ...
    const kit::RainRipples* rip = d.world.try_get<kit::RainRipples>(d.e[kWeather]);
    REQUIRE(rip != nullptr);
    CHECK(rip->strength == 1.0f);
    CHECK(rip->density == 0.5f);
    const kit::ThunderDelay* th = d.world.try_get<kit::ThunderDelay>(d.e[kWeather]);
    REQUIRE(th != nullptr);
    CHECK(th->speed_mps == 343.0f);
    CHECK(th->clip.guid.is_null());
    // M-WINDDIR 8e: ... and a WindDirection that turns 6 degrees a minute from +x.
    const kit::WindDirection* wd = d.world.try_get<kit::WindDirection>(d.e[kWeather]);
    REQUIRE(wd != nullptr);
    CHECK(wd->degrees == 0.0f);
    CHECK(wd->drift_degrees_per_minute == 6.0f);
    CHECK(wd->angle_q == 0u);
    // M-CLOUDS 9d: ... and a CloudLayer with the authoring defaults.
    const kit::CloudLayer* cl = d.world.try_get<kit::CloudLayer>(d.e[kWeather]);
    REQUIRE(cl != nullptr);
    CHECK(cl->strength == 1.0f);
    CHECK(cl->fair_coverage == 0.35f);
    CHECK(cl->overcast_coverage == 0.9f);
    CHECK(cl->scale_m == 900.0f);
    CHECK(cl->speed_scale == 1.0f);
    CHECK(cl->height_m == 2000.0f);
    // ... and a Splash entity: a RainSplash on its own ParticleEmitter3D.
    CHECK(d.world.try_get<kit::RainSplash>(d.e[kSplash]) != nullptr);
    CHECK(d.world.try_get<kit::ParticleEmitter3D>(d.e[kSplash]) != nullptr);
    // Tick 0: nothing has run, the resource is the authored Environment3D.
    CHECK(bits(d.env().sun_intensity) == bits(2.0f));
    CHECK(d.em().emitting == 0);  // as authored: dry until the weather says otherwise
    CHECK(d.em().max_particles == 4096);
    d.run_to(1);
    // The noon DayCycle key is 2.0; one tick later the hour is 12 + 1/1500 and the sun has only just left the key.
    CHECK(d.env().sun_intensity == doctest::Approx(2.0f).epsilon(1e-3));
    CHECK(d.em().emitting == 0);
    CHECK(d.em().rate == 0.0f);
}

TEST_CASE("weather demo: a clear sky is bit-identical to a scene without the forecast, and rain fades in") {
    Demo d, c(false);
    for (u64 t = 1; t <= 60; ++t) {
        d.run_to(t);
        c.run_to(t);
        INFO("tick " << t);
        REQUIRE(bits(d.env().sun_intensity) == bits(c.env().sun_intensity));
        REQUIRE(bits(d.env().exposure) == bits(c.env().exposure));
        REQUIRE(bits(d.env().fog_density) == bits(c.env().fog_density));
        REQUIRE(bits(d.env().ambient_sky.x) == bits(c.env().ambient_sky.x));
    }
    CHECK(d.wx().target == kit::WeatherKind::Rain);  // called at tick 60
    CHECK(d.wx().current == kit::WeatherKind::Clear);
    d.run_to(61);
    CHECK(d.wx().current == kit::WeatherKind::Rain);
    CHECK(bits(d.wx().intensity) == bits(static_cast<f32>(1.0 / 600.0)));

    d.run_to(360);  // 300 / 600
    CHECK(bits(d.wx().intensity) == bits(0.5f));
    CHECK(d.em().emitting == 1);
    CHECK(d.em().rate == doctest::Approx(1500.0f).epsilon(1e-5));
    CHECK(d.em().gravity.y == doctest::Approx(-9.0f).epsilon(1e-5));  // -0.5 * rain_speed 18
    CHECK(d.em().additive == 0);

    d.run_to(600);  // 540 / 600, the storm is called
    CHECK(bits(d.wx().intensity) == bits(static_cast<f32>(540.0 / 600.0)));
    CHECK(d.em().rate == doctest::Approx(2700.0f).epsilon(1e-5));
    // The rain is already dimming the sun against the control.
    c.run_to(600);
    CHECK(d.env().sun_intensity < c.env().sun_intensity * 0.4f);
    CHECK(d.world.try_get<kit::Weather>(d.e[kWeather])->target == kit::WeatherKind::Storm);
}

TEST_CASE("weather demo: the storm is darker than clear, strikes only inside the storm, then snow") {
    Demo d, c(false);
    d.run_to(1139);
    CHECK(d.wx().current == kit::WeatherKind::Rain);
    CHECK(d.strikes == 0);
    d.run_to(1140);
    CHECK(d.wx().current == kit::WeatherKind::Storm);
    d.run_to(1500);  // 361 / 600 when snow is called
    c.run_to(1500);
    CHECK(bits(d.wx().intensity) == bits(static_cast<f32>(361.0 / 600.0)));
    CHECK(d.em().rate == doctest::Approx(3000.0f * 1.6f * 361.0f / 600.0f).epsilon(1e-5));
    CHECK(d.env().sun_intensity < c.env().sun_intensity);
    CHECK(d.env().exposure > c.env().exposure);
    CHECK(d.env().fog_density > c.env().fog_density);
    CHECK(d.wx().wind > 0.6f);
    d.run_to(1860);
    CHECK(d.wx().current == kit::WeatherKind::Storm);
    d.run_to(1861);
    CHECK(d.wx().current == kit::WeatherKind::Snow);
    // At least one strike in the storm window for seed 7 (gaps of 2..5 s), none outside it.
    CHECK(d.strikes >= 1);
    CHECK(d.strikes == 2);
    CHECK(d.first_strike == 1392);
    CHECK(d.last_strike == 1637);
}

TEST_CASE("weather demo: snow, then the environment returns to exactly the clear value") {
    Demo d, c(false);
    d.run_to(2100);  // 240 / 600
    c.run_to(2100);
    CHECK(d.wx().current == kit::WeatherKind::Snow);
    CHECK(bits(d.wx().intensity) == bits(static_cast<f32>(240.0 / 600.0)));
    CHECK(d.em().emitting == 1);
    CHECK(d.em().rate == doctest::Approx(320.0f).epsilon(1e-5));  // snow_rate 800 * 0.4
    CHECK(d.em().gravity.y == doctest::Approx(-0.45f).epsilon(1e-5));  // -0.3 * snow_speed 1.5, snow not rain
    CHECK(d.em().additive == 0);
    CHECK(d.env().sun_intensity < c.env().sun_intensity);
    d.run_to(2340);
    CHECK(d.wx().current == kit::WeatherKind::Clear);
    d.run_to(2400);
    c.run_to(2400);
    CHECK(d.wx().target == kit::WeatherKind::Clear);
    CHECK(bits(d.wx().intensity) == bits(0.0f));
    CHECK(d.wx().flash_mul == 1.0f);
    CHECK(d.em().emitting == 0);
    CHECK(d.em().rate == 0.0f);
    CHECK(bits(d.env().sun_intensity) == bits(c.env().sun_intensity));
    CHECK(bits(d.env().exposure) == bits(c.env().exposure));
    CHECK(bits(d.env().fog_density) == bits(c.env().fog_density));
    CHECK(bits(d.env().ambient_sky.x) == bits(c.env().ambient_sky.x));
    CHECK(bits(d.env().ambient_ground.y) == bits(c.env().ambient_ground.y));
    CHECK(bits(d.env().sun_color.z) == bits(c.env().sun_color.z));
    CHECK(bits(d.env().fog_color.x) == bits(c.env().fog_color.x));
    CHECK(d.wx().wetness > 0.0f);  // the ground is still wet, drying over 120 s
}

TEST_CASE("weather demo: the same run twice has the same state hash") {
    auto run = [] {
        Demo d;
        d.run_to(1500);
        return d.world.state_hash();
    };
    const u64 a = run();
    const u64 b = run();
    CHECK(a == b);
}

TEST_CASE("weather demo: the snow cover stays 0 until the snow is in effect, builds while it snows and melts after") {
    Demo d, c(false);
    d.run_to(1860);
    CHECK(d.snow().snow_q == 0u);
    CHECK(d.snow().cover == 0.0f);
    d.run_to(2100);
    const f32 at_2100 = d.snow().cover;
    CHECK(at_2100 > 0.05f);
    CHECK(at_2100 < 0.3f);
    d.run_to(2340);  // the last tick of snow
    const f32 peak = d.snow().cover;
    CHECK(peak > at_2100);
    CHECK(peak <= 1.0f);
    d.run_to(2700);
    CHECK(d.snow().cover < peak);
    CHECK(d.snow().cover >= 0.0f);
    // The control without the forecast never snows.
    c.run_to(2700);
    CHECK(c.snow().cover == 0.0f);
}

TEST_CASE("weather demo: the splash emitter bursts in the rain and is off in the clear") {
    Demo d;
    const auto& sp = [&]() -> const kit::ParticleEmitter3D& {
        return *d.world.try_get<kit::ParticleEmitter3D>(d.e[kSplash]);
    };
    d.run_to(30);  // clear
    CHECK(sp().emitting == 0);
    CHECK(d.world.try_get<kit::RainSplash>(d.e[kSplash])->counter == 0u);
    d.run_to(600);  // rain 0.9
    CHECK(d.world.try_get<kit::RainSplash>(d.e[kSplash])->counter > 0u);
    CHECK(sp().burst > 0u);
}

TEST_CASE("weather demo: the wind heading drifts 6 degrees a minute from +x") {
    Demo d;
    const auto& wd = [&]() -> const kit::WindDirection& { return *d.world.try_get<kit::WindDirection>(d.e[kWeather]); };
    d.run_to(600);  // ten seconds: one degree
    const u32 at_600 = wd().angle_q;
    CHECK(at_600 > 0u);
    CHECK(static_cast<f64>(at_600) / 65536.0 == doctest::Approx(1.0).epsilon(0.02));
    d.run_to(2700);  // forty-five seconds: 4.5 degrees
    CHECK(wd().angle_q > at_600);
    CHECK(static_cast<f64>(wd().angle_q) / 65536.0 == doctest::Approx(4.5).epsilon(0.02));
    CHECK(wd().seeded == 1u);
    CHECK(wd().degrees == 0.0f);  // the authored start heading is never rewritten
}
