// M-WEATHER 4a: the `weather` Luau global over the kit's Weather. Exact numbers: a 10 s transition is 600 ticks; a
// script that calls weather.set('storm') on its tick 1 (after the weather system of that tick) sees the kind switch
// on tick 2 and the intensity (N - 1) / 600 after N ticks. A world without a Weather, an unknown kind and bad
// arguments raise a Luau error that names the call and disables only that script.

#include <doctest/doctest.h>

#include <bit>
#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/cloud_layer.h"
#include "mce/kit/rain_ripples.h"
#include "mce/kit/rain_splash.h"
#include "mce/kit/snow_cover.h"
#include "mce/kit/thunder_delay.h"
#include "mce/kit/weather.h"
#include "mce/kit/wind_direction.h"
#include "mce/kit/wind_sway.h"
#include "mce/kit/wet_surfaces.h"
#include "mce/script/compiler.h"

using namespace mce;
using script::ScriptHost;
using script::ScriptState;

namespace {

// A World with the weather system (and a Weather, 10 s transitions) and one script in FixedUpdate_Post. `probe` is an
// entity whose LocalTransform the script may write.
struct WRig {
    World w;
    ScriptHost host;
    u64 tick = 0;
    Entity wx_e, wet_e, sway_e, snow_e, ripples_e, splash_e, thunder_e, wind_e, cloud_e, probe;

    explicit WRig(const char* source, bool with_weather = true, bool install = true,
                  bool with_wet = false, bool with_sway = false, bool with_snow = false, bool with_rainfx = false,
                  bool with_wind = false, bool with_clouds = false) {
        w.register_components<LocalTransform>();
        if (install) kit::install_weather(w, nullptr);
        INFO(source);
        // Unchecked: the error tests pass what the type check would reject; the VM must still refuse it by name.
        Result<Array<u8>> bc = script::compile_unchecked("w", source);
        REQUIRE(bc);
        REQUIRE(host.add_bytecode("w", bc.value().span()));
        host.install(w);
        probe = w.spawn_with(LocalTransform{});
        if (with_weather) {
            kit::Weather c;
            c.transition_seconds = 10.0f;
            wx_e = w.spawn_with(c);
        }
        if (with_wet) wet_e = w.spawn_with(kit::WetSurfaces{});
        if (with_sway) sway_e = w.spawn_with(kit::WindSway{});
        if (with_snow) snow_e = w.spawn_with(kit::SnowCover{});
        if (with_rainfx) {
            ripples_e = w.spawn_with(kit::RainRipples{});
            splash_e = w.spawn_with(kit::RainSplash{});
            thunder_e = w.spawn_with(kit::ThunderDelay{});
        }
        if (with_wind) wind_e = w.spawn_with(kit::WindDirection{});
        if (with_clouds) cloud_e = w.spawn_with(kit::CloudLayer{});
    }
    kit::CloudLayer& clouds() { return *w.try_get_mut<kit::CloudLayer>(cloud_e); }
    kit::WindDirection& wind() { return *w.try_get_mut<kit::WindDirection>(wind_e); }
    kit::RainRipples& ripples() { return *w.try_get_mut<kit::RainRipples>(ripples_e); }
    kit::RainSplash& splash() { return *w.try_get_mut<kit::RainSplash>(splash_e); }
    kit::ThunderDelay& thunder() { return *w.try_get_mut<kit::ThunderDelay>(thunder_e); }
    kit::SnowCover& snow() { return *w.try_get_mut<kit::SnowCover>(snow_e); }
    kit::WindSway& sway() { return *w.try_get_mut<kit::WindSway>(sway_e); }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
    kit::WetSurfaces& wet() { return *w.try_get_mut<kit::WetSurfaces>(wet_e); }
    kit::Weather& wx() { return *w.try_get_mut<kit::Weather>(wx_e); }
    const LocalTransform& p() { return *w.try_get<LocalTransform>(probe); }
    const ScriptHost& h() const { return host; }
};

std::string err(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

// A script that runs `body` once on tick 1 and must fail with `needle`.
void expect_error(const char* body, const char* needle, bool with_weather = true, bool install = true,
                  bool with_wet = false, bool with_sway = false, bool with_snow = false, bool with_rainfx = false,
                  bool with_wind = false, bool with_clouds = false) {
    const std::string src = std::string("function update()\n    if mce.tick() == 1 then\n        ") + body + "\n    end\nend\n";
    WRig r(src.c_str(), with_weather, install, with_wet, with_sway, with_snow, with_rainfx, with_wind, with_clouds);
    r.step(2);
    INFO(body);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    INFO(err(r.h()));
    CHECK(err(r.h()).find(needle) != std::string::npos);
}

u32 bits(f32 v) { return std::bit_cast<u32>(v); }

}  // namespace

TEST_CASE("weather api: set storm, the kinds and the numbers at pinned ticks") {
    WRig r(R"(
function update()
    local t = mce.tick()
    if t == 1 then
        assert(weather.kind() == 'clear' and weather.target() == 'clear')
        assert(weather.intensity() == 0 and weather.wetness() == 0)
        weather.set('storm')
        assert(weather.target() == 'storm' and weather.kind() == 'clear')
    end
    if t == 301 then
        assert(weather.kind() == 'storm' and weather.target() == 'storm')
        mce.each("LocalTransform", function(e)
            mce.set(e, "LocalTransform", "position", vector.create(weather.intensity(), weather.wind(), weather.wetness()))
        end)
    end
end
)");
    r.step(1);
    CHECK(r.wx().target == kit::WeatherKind::Storm);
    CHECK(r.wx().current == kit::WeatherKind::Clear);
    CHECK(r.wx().transition_seconds == 10.0f);  // set without seconds leaves it alone
    r.step(300);                                // N = 301: fade 300 of 600
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.wx().current == kit::WeatherKind::Storm);
    CHECK(bits(r.wx().intensity) == bits(0.5f));
    CHECK(bits(r.p().position.x) == bits(0.5f));
    CHECK(r.p().position.y == doctest::Approx(0.55f).epsilon(1e-6));  // 0.1 + (1.0 - 0.1) * 0.5
    CHECK(bits(r.p().position.y) == bits(r.wx().wind));
    CHECK(r.p().position.z > 0.0f);
    CHECK(bits(r.p().position.z) == bits(r.wx().wetness));
    r.step(300);  // N = 601: full
    CHECK(bits(r.wx().intensity) == bits(1.0f));
    CHECK(r.wx().wind == doctest::Approx(1.0f).epsilon(1e-6));
}

TEST_CASE("weather api: set with seconds writes the transition, and a change between kinds goes through clear") {
    WRig r(R"(
function update()
    local t = mce.tick()
    if t == 1 then weather.set('rain', 2) end
    if t == 121 then
        assert(weather.kind() == 'rain' and weather.intensity() == 1)
        weather.set('snow', 1)
    end
    if t == 122 then assert(weather.kind() == 'rain' and weather.target() == 'snow') end
    if t == 300 then assert(weather.kind() == 'snow') end
end
)");
    r.step(1);
    CHECK(r.wx().transition_seconds == 2.0f);
    r.step(120);  // N = 121: 2 s is 120 ticks, rain from tick 2 reaches 1 on tick 121
    CHECK(bits(r.wx().intensity) == bits(1.0f));
    CHECK(r.wx().transition_seconds == 1.0f);
    r.step(179);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.wx().current == kit::WeatherKind::Snow);
}

TEST_CASE("weather api: every kind name round-trips") {
    WRig r(R"(
function update()
    local t = mce.tick()
    if t == 1 then weather.set('overcast', 0.1) end
    if t == 3 then assert(weather.kind() == 'overcast') weather.set('rain') end
    if t == 7 then assert(weather.kind() == 'rain') weather.set('storm') end
    if t == 11 then assert(weather.kind() == 'storm') weather.set('snow') end
    if t == 15 then assert(weather.kind() == 'snow') weather.set('clear') end
    if t == 20 then assert(weather.kind() == 'clear' and weather.target() == 'clear') end
end
)");
    // transition 0.1 s = 6 ticks: tick 2 switches to overcast (fade 1 of 6) ... the asserts only need the switch.
    r.step(20);
    INFO(err(r.h()));
    CHECK(r.h().scripts()[0].state == ScriptState::Running);
}

TEST_CASE("weather api: a world without a Weather raises a named error") {
    expect_error("weather.kind()", "weather.kind: the scene has no Weather entity", false);
    expect_error("weather.target()", "weather.target: the scene has no Weather entity", false);
    expect_error("weather.set('rain')", "weather.set: the scene has no Weather entity", false);
    expect_error("weather.intensity()", "weather.intensity: the scene has no Weather entity", false);
    expect_error("weather.wind()", "weather.wind: the scene has no Weather entity", false);
    expect_error("weather.wetness()", "weather.wetness: the scene has no Weather entity", false);
    expect_error("weather.kind()", "weather.kind: this world has no weather", false, false);
}

TEST_CASE("weather api: bad arguments raise an error that names the call") {
    expect_error("weather.set('hail')", "weather.set: unknown weather 'hail'");
    expect_error("weather.set('')", "weather.set: unknown weather ''");
    expect_error("weather.set('Rain')", "weather.set: unknown weather 'Rain'");
    expect_error("weather.set('rain', 0/0)", "weather.set: the transition seconds are not a number");
    expect_error("weather.set(3)", "weather.set: unknown weather '3'");
    expect_error("weather.set('rain', 'slow')", "number expected");
    expect_error("weather.set()", "string expected");
}

TEST_CASE("weather api: the wet getters read the first WetSurfaces, set_wet writes and clamps to the Range") {
    WRig r(R"(
function update()
    local t = mce.tick()
    if t == 1 then
        assert(weather.wet_strength() == 1 and weather.wet_darken() == 0.5)
        assert(weather.wet_puddles() == 0.5 and weather.wet_puddle_scale() == 4)
        weather.set_wet{darken = 0.25}
    end
    if t == 2 then
        assert(weather.wet_darken() == 0.25 and weather.wet_strength() == 1)  -- only the given key changed
        weather.set_wet{strength = 9, darken = -3, puddles = 2, puddle_scale = 0}
    end
    if t == 3 then
        assert(weather.wet_strength() == 4 and weather.wet_darken() == 0)
        assert(weather.wet_puddles() == 1 and math.abs(weather.wet_puddle_scale() - 0.1) < 1e-6)  -- f32 0.1
        weather.set_wet{puddle_scale = 1000}
    end
    if t == 4 then
        assert(weather.wet_puddle_scale() == 100)
        weather.set_wet{}  -- nothing given, nothing changes
        weather.set_wet{strength = 2, darken = 0.8, puddles = 0.125, puddle_scale = 6}
    end
end
)",
           true, true, true);
    r.step(1);
    CHECK(r.wet().darken == 0.25f);
    r.step(1);
    CHECK(r.wet().strength == 4.0f);
    CHECK(r.wet().darken == 0.0f);
    CHECK(r.wet().puddles == 1.0f);
    CHECK(r.wet().puddle_scale_m == 0.1f);
    r.step(1);
    CHECK(r.wet().puddle_scale_m == 100.0f);
    r.step(1);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.wet().strength == 2.0f);
    CHECK(r.wet().darken == 0.8f);
    CHECK(r.wet().puddles == 0.125f);
    CHECK(r.wet().puddle_scale_m == 6.0f);
    CHECK(r.wet().wet_roughness == 0.15f);  // not reachable from Luau
}

TEST_CASE("weather api: a world without a WetSurfaces raises a named error") {
    expect_error("weather.wet_strength()", "weather.wet_strength: the scene has no WetSurfaces entity");
    expect_error("weather.wet_darken()", "weather.wet_darken: the scene has no WetSurfaces entity");
    expect_error("weather.wet_puddles()", "weather.wet_puddles: the scene has no WetSurfaces entity");
    expect_error("weather.wet_puddle_scale()", "weather.wet_puddle_scale: the scene has no WetSurfaces entity");
    expect_error("weather.set_wet{strength = 1}", "weather.set_wet: the scene has no WetSurfaces entity");
    expect_error("weather.set_wet{}", "weather.set_wet: the scene has no WetSurfaces entity");
    expect_error("weather.wet_strength()", "weather.wet_strength: this world has no weather", false, false);
    // A Weather alone is not enough.
    expect_error("weather.wet_darken()", "weather.wet_darken: the scene has no WetSurfaces entity", true, true, false);
}

TEST_CASE("weather api: set_wet bad arguments raise an error that names the call and write nothing") {
    expect_error("weather.set_wet{bogus = 1}", "weather.set_wet: unknown key 'bogus'", true, true, true);
    expect_error("weather.set_wet{Strength = 1}", "weather.set_wet: unknown key 'Strength'", true, true, true);
    expect_error("weather.set_wet{puddles = 'lots'}", "weather.set_wet: 'puddles' is not a number", true, true, true);
    expect_error("weather.set_wet{darken = 0/0}", "weather.set_wet: 'darken' is not a number", true, true, true);
    expect_error("weather.set_wet{[1] = 0.5}", "weather.set_wet: the keys are names", true, true, true);
    expect_error("weather.set_wet(0.5)", "table expected", true, true, true);
    expect_error("weather.set_wet()", "table expected", true, true, true);
    // A bad key after a good one: the good one is not written.
    WRig r(R"(
function update()
    if mce.tick() == 1 then weather.set_wet{strength = 3, puddle_scale = 'big'} end
end
)",
           true, true, true);
    r.step(2);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    CHECK(r.wet().strength == 1.0f);
    CHECK(r.wet().puddle_scale_m == 4.0f);
}

TEST_CASE("weather api: the sway getters read the first WindSway, set_sway writes and clamps to the Range") {
    WRig r(R"(
function update()
    local t = mce.tick()
    if t == 1 then
        assert(weather.sway_amplitude() == 4 and weather.sway_frequency() == 1.5)
        assert(math.abs(weather.sway_gust() - 0.6) < 1e-6 and weather.sway_flutter() == 3)
        weather.set_sway{gust = 0.25}
    end
    if t == 2 then
        assert(weather.sway_gust() == 0.25 and weather.sway_amplitude() == 4)  -- only the given key changed
        weather.set_sway{amplitude = 99, frequency = -3, gust = -7, flutter = 51}
    end
    if t == 3 then
        assert(weather.sway_amplitude() == 50 and weather.sway_frequency() == 0)
        assert(weather.sway_gust() == -1 and weather.sway_flutter() == 50)
        weather.set_sway{frequency = 99, gust = 7}
    end
    if t == 4 then
        assert(weather.sway_frequency() == 10 and weather.sway_gust() == 1)
        weather.set_sway{}  -- nothing given, nothing changes
        weather.set_sway{amplitude = 2, frequency = 0.5, gust = 0.125, flutter = 6}
    end
end
)",
           true, true, false, true);
    r.step(1);
    CHECK(r.sway().gust_gain == 0.25f);
    r.step(1);
    CHECK(r.sway().amplitude_gain == 50.0f);
    CHECK(r.sway().frequency_gain == 0.0f);
    CHECK(r.sway().gust_gain == -1.0f);
    CHECK(r.sway().flutter_gain == 50.0f);
    r.step(1);
    CHECK(r.sway().frequency_gain == 10.0f);
    CHECK(r.sway().gust_gain == 1.0f);
    r.step(1);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.sway().amplitude_gain == 2.0f);
    CHECK(r.sway().frequency_gain == 0.5f);
    CHECK(r.sway().gust_gain == 0.125f);
    CHECK(r.sway().flutter_gain == 6.0f);
}

TEST_CASE("weather api: a world without a WindSway raises a named error") {
    expect_error("weather.sway_amplitude()", "weather.sway_amplitude: the scene has no WindSway entity");
    expect_error("weather.sway_frequency()", "weather.sway_frequency: the scene has no WindSway entity");
    expect_error("weather.sway_gust()", "weather.sway_gust: the scene has no WindSway entity");
    expect_error("weather.sway_flutter()", "weather.sway_flutter: the scene has no WindSway entity");
    expect_error("weather.set_sway{amplitude = 1}", "weather.set_sway: the scene has no WindSway entity");
    expect_error("weather.set_sway{}", "weather.set_sway: the scene has no WindSway entity");
    expect_error("weather.sway_amplitude()", "weather.sway_amplitude: this world has no weather", false, false);
    expect_error("weather.set_sway{}", "weather.set_sway: this world has no weather", false, false);
    // A Weather and a WetSurfaces alone are not enough.
    expect_error("weather.sway_gust()", "weather.sway_gust: the scene has no WindSway entity", true, true, true);
}

TEST_CASE("weather api: set_sway bad arguments raise an error that names the call and write nothing") {
    expect_error("weather.set_sway{bogus = 1}", "weather.set_sway: unknown key 'bogus'", true, true, false, true);
    expect_error("weather.set_sway{Gust = 1}", "weather.set_sway: unknown key 'Gust'", true, true, false, true);
    expect_error("weather.set_sway{gust = 'lots'}", "weather.set_sway: 'gust' is not a number", true, true, false, true);
    expect_error("weather.set_sway{flutter = 0/0}", "weather.set_sway: 'flutter' is not a number", true, true, false, true);
    expect_error("weather.set_sway{[1] = 0.5}", "weather.set_sway: the keys are names", true, true, false, true);
    expect_error("weather.set_sway(0.5)", "table expected", true, true, false, true);
    expect_error("weather.set_sway()", "table expected", true, true, false, true);
    // A bad key after a good one: the good one is not written (and the order of the table does not matter).
    for (const char* body : {"weather.set_sway{amplitude = 3, flutter = 'big'}",
                             "weather.set_sway{flutter = 'big', amplitude = 3}",
                             "weather.set_sway{amplitude = 3, nope = 1}"}) {
        const std::string src = std::string("function update()\n    if mce.tick() == 1 then ") + body + " end\nend\n";
        WRig r(src.c_str(), true, true, false, true);
        r.step(2);
        INFO(body);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
        CHECK(r.sway().amplitude_gain == 4.0f);
        CHECK(r.sway().flutter_gain == 3.0f);
    }
}

TEST_CASE("weather api: the snow getters read the first SnowCover, set_snow writes and clamps to the Range") {
    WRig r(R"(
function update()
    local t = mce.tick()
    if t == 1 then
        assert(weather.snow_cover() == 0 and weather.snow_strength() == 1)
        assert(weather.snow_build() == 90 and weather.snow_melt() == 120)
        assert(math.abs(weather.snow_coverage() - 0.8) < 1e-6 and weather.snow_scale() == 3)
        assert(weather.snow_wind() == 0.5)
        weather.set_snow{coverage = 0.25}
    end
    if t == 2 then
        assert(weather.snow_coverage() == 0.25 and weather.snow_build() == 90)  -- only the given key changed
        weather.set_snow{strength = 9, build = -5, melt = 1e9, coverage = 7, scale = 0.01, wind = -2}
    end
    if t == 3 then
        assert(weather.snow_strength() == 4 and weather.snow_build() == 0 and weather.snow_melt() == 86400)
        assert(weather.snow_coverage() == 1 and math.abs(weather.snow_scale() - 0.1) < 1e-6 and weather.snow_wind() == 0)
        weather.set_snow{scale = 1e6, wind = 9, strength = -1, melt = -1, build = 1e9, coverage = -1}
    end
    if t == 4 then
        assert(weather.snow_scale() == 100 and weather.snow_wind() == 1 and weather.snow_strength() == 0)
        assert(weather.snow_melt() == 0 and weather.snow_build() == 86400 and weather.snow_coverage() == 0)
        weather.set_snow{}  -- nothing given, nothing changes
        weather.set_snow{strength = 2, build = 30, melt = 60, coverage = 0.5, scale = 6, wind = 0.25}
    end
end
)",
           true, true, false, false, true);
    r.step(1);
    CHECK(r.snow().coverage == 0.25f);
    r.step(1);
    CHECK(r.snow().strength == 4.0f);
    CHECK(r.snow().build_seconds == 0.0f);
    CHECK(r.snow().melt_seconds == 86400.0f);
    CHECK(r.snow().coverage == 1.0f);
    CHECK(r.snow().noise_scale_m == 0.1f);
    CHECK(r.snow().wind_bias == 0.0f);
    r.step(1);
    CHECK(r.snow().noise_scale_m == 100.0f);
    CHECK(r.snow().wind_bias == 1.0f);
    r.step(1);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.snow().strength == 2.0f);
    CHECK(r.snow().build_seconds == 30.0f);
    CHECK(r.snow().melt_seconds == 60.0f);
    CHECK(r.snow().coverage == 0.5f);
    CHECK(r.snow().noise_scale_m == 6.0f);
    CHECK(r.snow().wind_bias == 0.25f);
}

TEST_CASE("weather api: snow_cover is the engine-written cover and rises while it snows") {
    WRig r(R"(
function update()
    if mce.tick() == 1 then
        weather.set_snow{build = 1}
        weather.set("snow", 0)
    end
end
)",
           true, true, false, false, true);
    r.step(1);
    CHECK(r.snow().cover == 0.0f);  // the script has not run yet on tick 1's weather pass
    r.step(121);
    INFO("cover " << r.snow().cover);
    CHECK(r.snow().cover > 0.5f);
}

TEST_CASE("weather api: a world without a SnowCover raises a named error") {
    expect_error("weather.snow_cover()", "weather.snow_cover: the scene has no SnowCover entity");
    expect_error("weather.snow_strength()", "weather.snow_strength: the scene has no SnowCover entity");
    expect_error("weather.snow_build()", "weather.snow_build: the scene has no SnowCover entity");
    expect_error("weather.snow_melt()", "weather.snow_melt: the scene has no SnowCover entity");
    expect_error("weather.snow_coverage()", "weather.snow_coverage: the scene has no SnowCover entity");
    expect_error("weather.snow_scale()", "weather.snow_scale: the scene has no SnowCover entity");
    expect_error("weather.snow_wind()", "weather.snow_wind: the scene has no SnowCover entity");
    expect_error("weather.set_snow{strength = 1}", "weather.set_snow: the scene has no SnowCover entity");
    expect_error("weather.set_snow{}", "weather.set_snow: the scene has no SnowCover entity");
    expect_error("weather.snow_cover()", "weather.snow_cover: this world has no weather", false, false);
    expect_error("weather.set_snow{}", "weather.set_snow: this world has no weather", false, false);
    // A Weather, a WetSurfaces and a WindSway alone are not enough.
    expect_error("weather.snow_cover()", "weather.snow_cover: the scene has no SnowCover entity", true, true, true, true);
}

TEST_CASE("weather api: set_snow bad arguments raise an error that names the call and write nothing") {
    expect_error("weather.set_snow{bogus = 1}", "weather.set_snow: unknown key 'bogus'", true, true, false, false, true);
    expect_error("weather.set_snow{cover = 1}", "weather.set_snow: unknown key 'cover'", true, true, false, false, true);
    expect_error("weather.set_snow{Wind = 1}", "weather.set_snow: unknown key 'Wind'", true, true, false, false, true);
    expect_error("weather.set_snow{wind = 'lots'}", "weather.set_snow: 'wind' is not a number", true, true, false, false, true);
    expect_error("weather.set_snow{scale = 0/0}", "weather.set_snow: 'scale' is not a number", true, true, false, false, true);
    expect_error("weather.set_snow{[1] = 0.5}", "weather.set_snow: the keys are names", true, true, false, false, true);
    expect_error("weather.set_snow(0.5)", "table expected", true, true, false, false, true);
    expect_error("weather.set_snow()", "table expected", true, true, false, false, true);
    for (const char* body : {"weather.set_snow{strength = 3, scale = 'big'}",
                             "weather.set_snow{scale = 'big', strength = 3}",
                             "weather.set_snow{strength = 3, nope = 1}"}) {
        const std::string src = std::string("function update()\n    if mce.tick() == 1 then ") + body + " end\nend\n";
        WRig r(src.c_str(), true, true, false, false, true);
        r.step(2);
        INFO(body);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
        CHECK(r.snow().strength == 1.0f);
        CHECK(r.snow().noise_scale_m == 3.0f);
    }
}

TEST_CASE("weather api: the rain effect getters read the first component, the setters write and clamp to the Range") {
    WRig r(R"(
function update()
    local t = mce.tick()
    if t == 1 then
        assert(weather.ripples_strength() == 1 and weather.ripples_density() == 0.5)
        assert(weather.ripples_speed() == 1 and math.abs(weather.ripples_ring_width() - 0.15) < 1e-6)
        assert(weather.splash_rate() == 1 and math.abs(weather.splash_height() - 0.15) < 1e-6)
        assert(weather.splash_life() == 0.25 and weather.splash_radius() == 25 and weather.splash_seed() == 1)
        assert(weather.thunder_speed() == 343 and weather.thunder_volume() == 1)
        weather.set_ripples{density = 0.25}
        weather.set_splash{seed = 42}
        weather.set_thunder{volume = 2}
    end
    if t == 2 then
        assert(weather.ripples_density() == 0.25 and weather.ripples_strength() == 1)  -- only the given key changed
        assert(weather.splash_seed() == 42 and weather.splash_rate() == 1)
        assert(weather.thunder_volume() == 2 and weather.thunder_speed() == 343)
        weather.set_ripples{strength = 9, density = -1, speed = 1e6, ring_width = 7}
        weather.set_splash{rate = 1e6, height = 0, life = 99, radius = -3, seed = 1e12}
        weather.set_thunder{speed = -5, volume = 99}
    end
    if t == 3 then
        assert(weather.ripples_strength() == 1 and weather.ripples_density() == 0 and weather.ripples_speed() == 10)
        assert(weather.ripples_ring_width() == 1)
        assert(weather.splash_rate() == 64 and math.abs(weather.splash_height() - 0.01) < 1e-6)
        assert(weather.splash_life() == 5 and weather.splash_radius() == 0.5 and weather.splash_seed() == 4294967295)
        assert(weather.thunder_speed() == 1 and weather.thunder_volume() == 4)
        weather.set_ripples{strength = -1, speed = 0}
        weather.set_splash{seed = -7, rate = -1}
        weather.set_ripples{}  -- nothing given, nothing changes
        weather.set_splash{}
        weather.set_thunder{}
    end
    if t == 4 then
        assert(weather.ripples_strength() == 0 and math.abs(weather.ripples_speed() - 0.1) < 1e-6)
        assert(weather.splash_seed() == 0 and weather.splash_rate() == 0)
        weather.set_ripples{strength = 0.5, density = 0.75, speed = 2, ring_width = 0.3}
        weather.set_splash{rate = 2, height = 0.5, life = 0.5, radius = 40, seed = 9}
        weather.set_thunder{speed = 300, volume = 0.5}
    end
end
)",
           true, true, false, false, false, true);
    r.step(1);
    CHECK(r.ripples().density == 0.25f);
    CHECK(r.splash().seed == 42u);
    CHECK(r.thunder().volume == 2.0f);
    r.step(1);
    CHECK(r.ripples().speed == 10.0f);
    CHECK(r.splash().seed == 4294967295u);
    CHECK(r.thunder().speed_mps == 1.0f);
    r.step(2);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.ripples().strength == 0.5f);
    CHECK(r.ripples().density == 0.75f);
    CHECK(r.ripples().speed == 2.0f);
    CHECK(r.ripples().ring_width == 0.3f);
    CHECK(r.splash().rate == 2.0f);
    CHECK(r.splash().height_m == 0.5f);
    CHECK(r.splash().life_seconds == 0.5f);
    CHECK(r.splash().radius_m == 40.0f);
    CHECK(r.splash().seed == 9u);
    CHECK(r.splash().counter == 0u);  // engine-written, untouched
    CHECK(r.thunder().speed_mps == 300.0f);
    CHECK(r.thunder().volume == 0.5f);
    CHECK(r.thunder().pending == 0u);
}

TEST_CASE("weather api: a world without the rain effect components raises a named error") {
    for (const char* name : {"ripples_strength", "ripples_density", "ripples_speed", "ripples_ring_width"})
        expect_error((std::string("weather.") + name + "()").c_str(),
                     (std::string("weather.") + name + ": the scene has no RainRipples entity").c_str());
    for (const char* name : {"splash_rate", "splash_height", "splash_life", "splash_radius", "splash_seed"})
        expect_error((std::string("weather.") + name + "()").c_str(),
                     (std::string("weather.") + name + ": the scene has no RainSplash entity").c_str());
    for (const char* name : {"thunder_speed", "thunder_volume"})
        expect_error((std::string("weather.") + name + "()").c_str(),
                     (std::string("weather.") + name + ": the scene has no ThunderDelay entity").c_str());
    expect_error("weather.set_ripples{}", "weather.set_ripples: the scene has no RainRipples entity");
    expect_error("weather.set_splash{rate = 1}", "weather.set_splash: the scene has no RainSplash entity");
    expect_error("weather.set_thunder{volume = 1}", "weather.set_thunder: the scene has no ThunderDelay entity");
    expect_error("weather.ripples_strength()", "weather.ripples_strength: this world has no weather", false, false);
    expect_error("weather.set_splash{}", "weather.set_splash: this world has no weather", false, false);
    expect_error("weather.set_thunder{}", "weather.set_thunder: this world has no weather", false, false);
    // The snow, wet and sway components alone are not enough.
    expect_error("weather.splash_rate()", "weather.splash_rate: the scene has no RainSplash entity", true, true, true,
                 true, true);
}

TEST_CASE("weather api: the rain effect setters' bad arguments raise an error that names the call and write nothing") {
    expect_error("weather.set_ripples{bogus = 1}", "weather.set_ripples: unknown key 'bogus'", true, true, false, false,
                 false, true);
    expect_error("weather.set_splash{counter = 1}", "weather.set_splash: unknown key 'counter'", true, true, false,
                 false, false, true);
    expect_error("weather.set_thunder{clip = 1}", "weather.set_thunder: unknown key 'clip'", true, true, false, false,
                 false, true);
    expect_error("weather.set_thunder{pending = 1}", "weather.set_thunder: unknown key 'pending'", true, true, false,
                 false, false, true);
    expect_error("weather.set_ripples{strength = 'lots'}", "weather.set_ripples: 'strength' is not a number", true, true,
                 false, false, false, true);
    expect_error("weather.set_splash{radius = 0/0}", "weather.set_splash: 'radius' is not a number", true, true, false,
                 false, false, true);
    expect_error("weather.set_thunder{[1] = 0.5}", "weather.set_thunder: the keys are names", true, true, false, false,
                 false, true);
    expect_error("weather.set_splash(0.5)", "table expected", true, true, false, false, false, true);
    expect_error("weather.set_ripples()", "table expected", true, true, false, false, false, true);
    for (const char* body : {"weather.set_ripples{strength = 0.5, speed = 'fast'}",
                             "weather.set_ripples{speed = 'fast', strength = 0.5}",
                             "weather.set_ripples{strength = 0.5, nope = 1}"}) {
        const std::string src = std::string("function update()\n    if mce.tick() == 1 then ") + body + " end\nend\n";
        WRig r(src.c_str(), true, true, false, false, false, true);
        r.step(2);
        INFO(body);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
        CHECK(r.ripples().strength == 1.0f);
        CHECK(r.ripples().speed == 1.0f);
    }
}

TEST_CASE("weather api: the wind direction getters read the first component, the setter writes and clamps to the Range") {
    WRig r(R"(
function update()
    local t = mce.tick()
    if t == 1 then
        assert(weather.wind_degrees() == 0 and weather.wind_drift() == 0 and weather.wind_heading() == 0)
        weather.set_wind_direction{degrees = 90}
    end
    if t == 3 then
        assert(weather.wind_degrees() == 90 and weather.wind_drift() == 0)  -- only the given key changed
        assert(weather.wind_heading() == 90)  -- kit_wind_direction resolved it (drift 0: round(degrees * 65536))
        weather.set_wind_direction{degrees = 1e6, drift = 1e6}
    end
    if t == 4 then
        assert(weather.wind_degrees() == 360 and weather.wind_drift() == 3600)
        weather.set_wind_direction{degrees = -5, drift = -1e6}
        weather.set_wind_direction{}  -- nothing given, nothing changes
    end
    if t == 5 then
        assert(weather.wind_degrees() == 0 and weather.wind_drift() == -3600)
        assert(weather.wind_heading() >= 0 and weather.wind_heading() < 360)
        weather.set_wind_direction{degrees = 45, drift = 0}
    end
    if t == 7 then
        assert(weather.wind_heading() == 45)  -- stopping the drift returns to the authored degrees
        weather.set_wind_direction{drift = 6}
    end
end
)",
           true, true, false, false, false, false, true);
    r.step(1);
    CHECK(r.wind().degrees == 90.0f);
    r.step(3);
    CHECK(r.wind().degrees == 0.0f);
    CHECK(r.wind().drift_degrees_per_minute == -3600.0f);
    r.step(3);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.wind().degrees == 45.0f);
    CHECK(r.wind().drift_degrees_per_minute == 6.0f);
    r.step(60);  // one second at 6 degrees per minute: 0.1 degree
    const f32 heading = static_cast<f32>(r.wind().angle_q) / 65536.0f;
    CHECK(heading > 45.0f);
    CHECK(heading < 45.5f);
}

TEST_CASE("weather api: a world without a WindDirection raises a named error") {
    for (const char* name : {"wind_degrees", "wind_drift", "wind_heading"})
        expect_error((std::string("weather.") + name + "()").c_str(),
                     (std::string("weather.") + name + ": the scene has no WindDirection entity").c_str());
    expect_error("weather.set_wind_direction{}", "weather.set_wind_direction: the scene has no WindDirection entity");
    expect_error("weather.wind_heading()", "weather.wind_heading: this world has no weather", false, false);
    expect_error("weather.set_wind_direction{}", "weather.set_wind_direction: this world has no weather", false, false);
    // The rain effect components alone are not enough.
    expect_error("weather.wind_degrees()", "weather.wind_degrees: the scene has no WindDirection entity", true, true,
                 true, true, true, true);
}

TEST_CASE("weather api: set_wind_direction bad arguments raise an error that names the call and write nothing") {
    expect_error("weather.set_wind_direction{bogus = 1}", "weather.set_wind_direction: unknown key 'bogus'", true, true,
                 false, false, false, false, true);
    expect_error("weather.set_wind_direction{angle_q = 1}", "weather.set_wind_direction: unknown key 'angle_q'", true,
                 true, false, false, false, false, true);
    expect_error("weather.set_wind_direction{frac = 1}", "weather.set_wind_direction: unknown key 'frac'", true, true,
                 false, false, false, false, true);
    expect_error("weather.set_wind_direction{degrees = 'north'}", "weather.set_wind_direction: 'degrees' is not a number",
                 true, true, false, false, false, false, true);
    expect_error("weather.set_wind_direction{drift = 0/0}", "weather.set_wind_direction: 'drift' is not a number", true,
                 true, false, false, false, false, true);
    expect_error("weather.set_wind_direction{[1] = 0.5}", "weather.set_wind_direction: the keys are names", true, true,
                 false, false, false, false, true);
    expect_error("weather.set_wind_direction(0.5)", "table expected", true, true, false, false, false, false, true);
    expect_error("weather.set_wind_direction()", "table expected", true, true, false, false, false, false, true);
    for (const char* body : {"weather.set_wind_direction{degrees = 90, drift = 'fast'}",
                             "weather.set_wind_direction{drift = 'fast', degrees = 90}",
                             "weather.set_wind_direction{degrees = 90, nope = 1}"}) {
        const std::string src = std::string("function update()\n    if mce.tick() == 1 then ") + body + " end\nend\n";
        WRig r(src.c_str(), true, true, false, false, false, false, true);
        r.step(2);
        INFO(body);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
        CHECK(r.wind().degrees == 0.0f);
        CHECK(r.wind().drift_degrees_per_minute == 0.0f);
    }
}

TEST_CASE("weather api: the cloud getters read the first CloudLayer, the setter writes and clamps to the Range") {
    WRig r(R"(
function update()
    local t = mce.tick()
    if t == 1 then
        assert(weather.cloud_strength() == 1 and weather.cloud_scale() == 900 and weather.cloud_height() == 2000)
        assert(weather.cloud_speed_scale() == 1)
        assert(math.abs(weather.cloud_fair_coverage() - 0.35) < 1e-6)
        assert(math.abs(weather.cloud_overcast_coverage() - 0.9) < 1e-6)
        weather.set_clouds{strength = 0.5, scale = 1200}
    end
    if t == 2 then
        assert(weather.cloud_strength() == 0.5 and weather.cloud_scale() == 1200)
        assert(weather.cloud_height() == 2000)  -- only the given keys changed
        weather.set_clouds{strength = 9, fair_coverage = 9, overcast_coverage = 9, scale = 1e9, speed_scale = 1e9, height = 1e9}
    end
    if t == 3 then
        assert(weather.cloud_strength() == 1 and weather.cloud_fair_coverage() == 1 and weather.cloud_overcast_coverage() == 1)
        assert(weather.cloud_scale() == 20000 and weather.cloud_speed_scale() == 10 and weather.cloud_height() == 20000)
        weather.set_clouds{strength = -5, fair_coverage = -5, overcast_coverage = -5, scale = -1e9, speed_scale = -1e9, height = -1e9}
        weather.set_clouds{}  -- nothing given, nothing changes
    end
    if t == 4 then
        assert(weather.cloud_strength() == 0 and weather.cloud_fair_coverage() == 0 and weather.cloud_overcast_coverage() == 0)
        assert(weather.cloud_scale() == 50 and weather.cloud_speed_scale() == 0 and weather.cloud_height() == 100)
        weather.set_clouds{fair_coverage = 0.2, overcast_coverage = 0.8, speed_scale = 2}
    end
end
)",
           true, true, false, false, false, false, false, true);
    r.step(1);
    CHECK(r.clouds().strength == 0.5f);
    CHECK(r.clouds().scale_m == 1200.0f);
    r.step(4);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.clouds().strength == 0.0f);
    CHECK(r.clouds().fair_coverage == 0.2f);
    CHECK(r.clouds().overcast_coverage == 0.8f);
    CHECK(r.clouds().speed_scale == 2.0f);
    CHECK(r.clouds().height_m == 100.0f);
}

TEST_CASE("weather api: a world without a CloudLayer raises a named error") {
    for (const char* name : {"cloud_strength", "cloud_fair_coverage", "cloud_overcast_coverage", "cloud_scale",
                             "cloud_speed_scale", "cloud_height"})
        expect_error((std::string("weather.") + name + "()").c_str(),
                     (std::string("weather.") + name + ": the scene has no CloudLayer entity").c_str());
    expect_error("weather.set_clouds{}", "weather.set_clouds: the scene has no CloudLayer entity");
    expect_error("weather.cloud_height()", "weather.cloud_height: this world has no weather", false, false);
    expect_error("weather.set_clouds{}", "weather.set_clouds: this world has no weather", false, false);
    // The other weather components alone are not enough.
    expect_error("weather.cloud_scale()", "weather.cloud_scale: the scene has no CloudLayer entity", true, true, true,
                 true, true, true, true);
}

TEST_CASE("weather api: set_clouds bad arguments raise an error that names the call and write nothing") {
    auto bad = [](const char* body, const char* needle) {
        expect_error(body, needle, true, true, false, false, false, false, false, true);
    };
    bad("weather.set_clouds{bogus = 1}", "weather.set_clouds: unknown key 'bogus'");
    bad("weather.set_clouds{scale_m = 1}", "weather.set_clouds: unknown key 'scale_m'");
    bad("weather.set_clouds{strength = 'thick'}", "weather.set_clouds: 'strength' is not a number");
    bad("weather.set_clouds{height = 0/0}", "weather.set_clouds: 'height' is not a number");
    bad("weather.set_clouds{[1] = 0.5}", "weather.set_clouds: the keys are names");
    bad("weather.set_clouds(0.5)", "table expected");
    bad("weather.set_clouds()", "table expected");
    for (const char* body : {"weather.set_clouds{strength = 0.5, height = 'high'}",
                             "weather.set_clouds{height = 'high', strength = 0.5}",
                             "weather.set_clouds{strength = 0.5, nope = 1}"}) {
        const std::string src = std::string("function update()\n    if mce.tick() == 1 then ") + body + " end\nend\n";
        WRig r(src.c_str(), true, true, false, false, false, false, false, true);
        r.step(2);
        INFO(body);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
        CHECK(r.clouds().strength == 1.0f);
        CHECK(r.clouds().height_m == 2000.0f);
    }
}
