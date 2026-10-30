// M-RAINBOW step 17d: the rainbow functions of the `weather` Luau table over the first kit::Rainbow. Getters, clamping,
// rainbow_level against the kit helper over hours, weather and cloud cover, named errors, nothing written on error, and a
// missing Rainbow raises (the setter never adds one).

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/cloud_layer.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/rainbow.h"
#include "mce/kit/weather.h"
#include "mce/render/atmosphere.h"
#include "mce/script/compiler.h"

using namespace mce;
using script::ScriptHost;
using script::ScriptState;

namespace {

struct RainbowRig {
    World w;
    ScriptHost host;
    u64 tick = 0;
    Entity rainbow_e, probe, weather_e, cloud_e;

    explicit RainbowRig(const char* source, bool with_rainbow = true, bool install = true) {
        w.register_components<LocalTransform>();
        if (install) kit::install_weather(w, nullptr);
        INFO(source);
        Result<Array<u8>> bc = script::compile_unchecked("a", source);
        REQUIRE(bc);
        REQUIRE(host.add_bytecode("a", bc.value().span()));
        host.install(w);
        probe = w.spawn_with(LocalTransform{});
        if (with_rainbow) rainbow_e = w.spawn_with(kit::Rainbow{});
    }
    kit::Rainbow& rainbow() { return *w.try_get_mut<kit::Rainbow>(rainbow_e); }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
    const LocalTransform& p() { return *w.try_get<LocalTransform>(probe); }
    const ScriptHost& h() const { return host; }
};

std::string err(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

void expect_error(const char* body, const char* needle, bool with_rainbow = true, bool install = true) {
    const std::string src = std::string("function update()\n    if mce.tick() == 1 then\n        ") + body + "\n    end\nend\n";
    RainbowRig r(src.c_str(), with_rainbow, install);
    r.step(2);
    INFO(body);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    INFO(err(r.h()));
    CHECK(err(r.h()).find(needle) != std::string::npos);
}

u32 bits(f32 v) { return std::bit_cast<u32>(v); }

// The sun elevation in degrees the extract derives from the light direction of the hour.
f32 elevation_of(const math::vec3& light) {
    const f32 len = std::sqrt(light.x * light.x + light.y * light.y + light.z * light.z);
    const f32 sine = std::clamp(-light.y / len, -1.0f, 1.0f);
    return static_cast<f32>(std::asin(static_cast<f64>(sine)) * (180.0 / 3.14159265358979323846));
}

f32 cloud_cover_of(const kit::Weather& wx, const kit::CloudLayer& cl) {
    const f32 k = wx.current == kit::WeatherKind::Clear ? 0.0f : (wx.current == kit::WeatherKind::Snow ? 0.9f : 1.0f);
    const f32 inten = std::clamp(wx.intensity, 0.0f, 1.0f);
    return std::clamp((cl.fair_coverage + (cl.overcast_coverage - cl.fair_coverage) * k * inten) * cl.strength, 0.0f, 1.0f);
}

}  // namespace

TEST_CASE("rainbow api: the getters read the first Rainbow, the setter writes and clamps to the Range") {
    RainbowRig r(R"(
function update()
    local t = mce.tick()
    if t == 1 then
        assert(weather.rainbow_strength() == 1 and weather.rainbow_band_width() == 1 and weather.rainbow_secondary() == 1)
        assert(weather.rainbow_min_wetness() == 0.25 and weather.rainbow_max_sun_elevation() == 42 and weather.rainbow_enabled() == 1)
        weather.set_rainbow{strength = 2.5, band_width = 1.5, secondary = 0.5, min_wetness = 0.5, max_sun_elevation = 30}
    end
    if t == 2 then
        assert(weather.rainbow_strength() == 2.5 and weather.rainbow_band_width() == 1.5 and weather.rainbow_secondary() == 0.5)
        assert(weather.rainbow_min_wetness() == 0.5 and weather.rainbow_max_sun_elevation() == 30 and weather.rainbow_enabled() == 1)
        weather.set_rainbow{enabled = 0}
    end
    if t == 3 then
        assert(weather.rainbow_strength() == 2.5 and weather.rainbow_enabled() == 0)  -- only the given keys changed
        weather.set_rainbow{strength = 1e9, band_width = 1e9, secondary = 1e9, min_wetness = 1e9, max_sun_elevation = 1e9, enabled = 7}
    end
    if t == 4 then
        assert(weather.rainbow_strength() == 4 and weather.rainbow_band_width() == 2 and weather.rainbow_secondary() == 1)
        assert(weather.rainbow_min_wetness() == 1 and weather.rainbow_max_sun_elevation() == 60 and weather.rainbow_enabled() == 1)
        weather.set_rainbow{strength = -1, band_width = -1, secondary = -1, min_wetness = -1, max_sun_elevation = -1, enabled = 0}
        weather.set_rainbow{}  -- nothing given, nothing changes
    end
    if t == 5 then
        assert(weather.rainbow_strength() == 0 and weather.rainbow_band_width() == 0.5 and weather.rainbow_secondary() == 0)
        assert(weather.rainbow_min_wetness() == 0 and weather.rainbow_max_sun_elevation() == 10 and weather.rainbow_enabled() == 0)
    end
end
)");
    r.step(6);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.rainbow().band_width == 0.5f);
    CHECK(r.rainbow().enabled == 0);
    CHECK(r.rainbow().pad_[0] == 0);
}

TEST_CASE("rainbow api: rainbow_level is the kit helper over hours, wetness, weather and cloud cover") {
    // Every tick the script writes the level to position.x.
    RainbowRig r(R"(
function update()
    mce.each("LocalTransform", function(e)
        mce.set(e, "LocalTransform", "position", vector.create(weather.rainbow_level(), 0, 0))
    end)
end
)");
    r.w.set_resource(kit::Environment3D{});
    r.weather_e = r.w.spawn_with(kit::Weather{});
    r.cloud_e = r.w.spawn_with(kit::CloudLayer{});
    auto set_hour = [&](f32 h) { r.w.try_resource<kit::Environment3D>()->time_of_day = h; };

    u32 nonzero = 0, zero = 0;
    for (f32 hour : {0.0f, 5.0f, 6.0f, 6.5f, 7.0f, 8.0f, 10.0f, 12.0f, 16.0f, 17.5f, 18.5f, 19.5f, 22.0f}) {
        for (f32 wet : {0.0f, 0.1f, 0.2f, 0.3f, 0.5f, 1.0f}) {
            for (int kind = 0; kind < 4; ++kind) {
                set_hour(hour);
                kit::Weather& wx = *r.w.try_get_mut<kit::Weather>(r.weather_e);
                wx.current = kind == 0 ? kit::WeatherKind::Clear
                             : kind == 1 ? kit::WeatherKind::Rain
                             : kind == 2 ? kit::WeatherKind::Snow
                                         : kit::WeatherKind::Storm;
                wx.target = wx.current;
                wx.intensity = kind == 0 ? 0.0f : 0.6f;
                wx.wet_q = static_cast<u32>(wet * 16777216.0f);  // the system derives wetness from wet_q
                r.step(1);
                REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
                // the weather system may move wetness one tick; read what the script saw from the component
                const kit::CloudLayer& cl = *r.w.try_get<kit::CloudLayer>(r.cloud_e);
                const bool raining = wx.current == kit::WeatherKind::Rain || wx.current == kit::WeatherKind::Storm;
                // the system ran before the script this tick, so its post-tick state is what the script read
                const f32 expected = kit::rainbow_level(r.rainbow(), elevation_of(render::sun_direction_at(hour)), wx.wetness,
                                                        raining ? wx.intensity : 0.0f, cloud_cover_of(wx, cl));
                INFO("hour " << hour << " wet " << wet << " kind " << kind);
                CHECK(bits(r.p().position.x) == bits(expected));
                if (expected > 0.0f) ++nonzero; else ++zero;
            }
        }
    }
    CHECK(nonzero > 5);
    CHECK(zero > 50);

    // A clear, wet morning: sun 20 degrees up gives a positive level; the same hour in the rain, on a dry ground, at night
    // and with the rainbow disabled gives exactly 0.
    kit::Weather& wx = *r.w.try_get_mut<kit::Weather>(r.weather_e);
    wx.current = wx.target = kit::WeatherKind::Clear;
    wx.intensity = 0.0f;
    wx.wet_q = static_cast<u32>(0.9f * 16777216.0f);
    set_hour(8.0f);
    r.step(1);
    const f32 morning = r.p().position.x;
    CHECK(morning > 0.0f);
    r.rainbow().strength = 2.0f;
    r.step(1);
    CHECK(r.p().position.x == doctest::Approx(2.0f * morning));
    r.rainbow().strength = 1.0f;
    set_hour(0.0f);
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
    set_hour(8.0f);
    r.rainbow().enabled = 0;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
}

TEST_CASE("rainbow api: with no clock the level follows Environment3D.sun_direction; no Weather or CloudLayer") {
    RainbowRig r(R"(
function update()
    mce.each("LocalTransform", function(e)
        mce.set(e, "LocalTransform", "position", vector.create(weather.rainbow_level(), 0, 0))
    end)
end
)");
    // No Weather: the extract's carrier stays default, so the level is 0.
    r.step(1);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.p().position.x == 0.0f);
    r.weather_e = r.w.spawn_with(kit::Weather{});
    r.w.try_get_mut<kit::Weather>(r.weather_e)->wet_q = 1u << 24;
    // Sun directions from straight down to rising; the helper decides, the script must agree to the bit.
    r.w.set_resource(kit::Environment3D{});
    for (f32 y : {-1.0f, -0.5f, -0.3f, -0.1f, 0.0f, 0.2f}) {
        kit::Environment3D& env = *r.w.try_resource<kit::Environment3D>();
        env.time_of_day = -1.0f;
        env.sun_direction = math::vec3{-0.4f, y, -0.3f};
        r.step(1);
        const kit::Weather& now = *r.w.try_get<kit::Weather>(r.weather_e);
        INFO("y " << y);
        CHECK(bits(r.p().position.x) == bits(kit::rainbow_level(r.rainbow(), elevation_of(env.sun_direction), now.wetness, 0.0f, 0.0f)));
    }
    // A sun at 20 degrees (light travelling down at -sin 20) shows with wet ground and no cloud layer.
    kit::Environment3D& env = *r.w.try_resource<kit::Environment3D>();
    env.sun_direction = math::vec3{0.0f, -0.342f, -0.94f};
    r.step(1);
    CHECK(r.p().position.x > 0.0f);
    // An equirect sky texture wins: no rainbow.
    env.sky_texture.guid = asset::Guid{1, 2};
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
}

TEST_CASE("rainbow api: a world without a Rainbow raises a named error and the setter never adds one") {
    for (const char* name : {"rainbow_strength", "rainbow_band_width", "rainbow_secondary", "rainbow_min_wetness",
                             "rainbow_max_sun_elevation", "rainbow_enabled", "rainbow_level"})
        expect_error((std::string("weather.") + name + "()").c_str(),
                     (std::string("weather.") + name + ": the scene has no Rainbow entity").c_str(), false);
    expect_error("weather.set_rainbow{}", "weather.set_rainbow: the scene has no Rainbow entity", false);
    expect_error("weather.set_rainbow{strength = 2}", "weather.set_rainbow: the scene has no Rainbow entity", false);
    expect_error("weather.rainbow_level()", "weather.rainbow_level: this world has no weather", false, false);
    expect_error("weather.set_rainbow{}", "weather.set_rainbow: this world has no weather", false, false);
    RainbowRig r("function update()\n    if mce.tick() == 1 then weather.set_rainbow{enabled = 0} end\nend\n", false);
    r.step(2);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    usize n = 0;
    r.w.query<kit::Rainbow>().each([&](Entity, kit::Rainbow&) { ++n; });
    CHECK(n == 0);
}

TEST_CASE("rainbow api: bad arguments raise an error that names the call and write nothing") {
    expect_error("weather.set_rainbow{bogus = 1}", "weather.set_rainbow: unknown key 'bogus'");
    expect_error("weather.set_rainbow{max_sun_elevation_deg = 1}", "weather.set_rainbow: unknown key 'max_sun_elevation_deg'");
    expect_error("weather.set_rainbow{strength = 'high'}", "weather.set_rainbow: 'strength' is not a number");
    expect_error("weather.set_rainbow{secondary = 0/0}", "weather.set_rainbow: 'secondary' is not a number");
    expect_error("weather.set_rainbow{enabled = true}", "weather.set_rainbow: 'enabled' is not a number");
    expect_error("weather.set_rainbow{[1] = 0.5}", "weather.set_rainbow: the keys are names");
    expect_error("weather.set_rainbow(0.5)", "table expected");
    expect_error("weather.set_rainbow()", "table expected");
    const kit::Rainbow def;
    for (const char* body : {"weather.set_rainbow{strength = 3, band_width = 'x'}", "weather.set_rainbow{band_width = 'x', strength = 3}",
                             "weather.set_rainbow{strength = 3, nope = 1}", "weather.set_rainbow{enabled = 0, min_wetness = 'x'}",
                             "weather.set_rainbow{min_wetness = 'x', enabled = 0, strength = 3}"}) {
        const std::string src = std::string("function update()\n    if mce.tick() == 1 then ") + body + " end\nend\n";
        RainbowRig r(src.c_str());
        r.step(2);
        INFO(body);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
        CHECK(r.rainbow().strength == def.strength);
        CHECK(r.rainbow().band_width == def.band_width);
        CHECK(r.rainbow().min_wetness == def.min_wetness);
        CHECK(r.rainbow().enabled == def.enabled);
    }
}
