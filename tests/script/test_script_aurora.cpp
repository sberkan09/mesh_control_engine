// M-AURORA step 16d: the aurora functions of the `weather` Luau table over the first kit::Aurora. Getters, clamping,
// aurora_gate / aurora_level against the kit helpers over latitudes and times, named errors, nothing written on error,
// and a missing Aurora raises (the setter never adds one).

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/aurora.h"
#include "mce/kit/cloud_layer.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/weather.h"
#include "mce/render/atmosphere.h"
#include "mce/script/compiler.h"

using namespace mce;
using script::ScriptHost;
using script::ScriptState;

namespace {

struct AuroraRig {
    World w;
    ScriptHost host;
    u64 tick = 0;
    Entity aurora_e, probe, weather_e, cloud_e;

    explicit AuroraRig(const char* source, bool with_aurora = true, bool install = true) {
        w.register_components<LocalTransform>();
        if (install) kit::install_weather(w, nullptr);
        INFO(source);
        Result<Array<u8>> bc = script::compile_unchecked("a", source);
        REQUIRE(bc);
        REQUIRE(host.add_bytecode("a", bc.value().span()));
        host.install(w);
        probe = w.spawn_with(LocalTransform{});
        if (with_aurora) aurora_e = w.spawn_with(kit::Aurora{});
    }
    kit::Aurora& aurora() { return *w.try_get_mut<kit::Aurora>(aurora_e); }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
    const LocalTransform& p() { return *w.try_get<LocalTransform>(probe); }
    const ScriptHost& h() const { return host; }
};

std::string err(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

void expect_error(const char* body, const char* needle, bool with_aurora = true, bool install = true) {
    const std::string src = std::string("function update()\n    if mce.tick() == 1 then\n        ") + body + "\n    end\nend\n";
    AuroraRig r(src.c_str(), with_aurora, install);
    r.step(2);
    INFO(body);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    INFO(err(r.h()));
    CHECK(err(r.h()).find(needle) != std::string::npos);
}

u32 bits(f32 v) { return std::bit_cast<u32>(v); }

// The night fade the extract computes from the hour (0 with no clock).
f32 night_fade(f32 hour) {
    if (!(hour >= 0.0f)) return 0.0f;
    const f32 elevation = -render::sun_direction_at(hour).y;
    const f32 t = std::clamp((elevation + 0.1f) / 0.2f, 0.0f, 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

}  // namespace

TEST_CASE("aurora api: the getters read the first Aurora, the setter writes and clamps to the Range") {
    AuroraRig r(R"(
function update()
    local t = mce.tick()
    if t == 1 then
        assert(weather.aurora_latitude() == 68 and weather.aurora_gate_low() == 55 and weather.aurora_gate_high() == 65)
        assert(weather.aurora_strength() == 1 and weather.aurora_curtain_count() == 3)
        assert(weather.aurora_base_height() > 0.179 and weather.aurora_base_height() < 0.181)
        assert(weather.aurora_height_span() > 0.349 and weather.aurora_height_span() < 0.351)
        assert(weather.aurora_azimuth() == 0 and weather.aurora_width() == 70 and weather.aurora_speed() == 1)
        assert(weather.aurora_night_only() == 1 and weather.aurora_enabled() == 1)
        local r, g, b = weather.aurora_low_color()
        assert(r > 0.099 and r < 0.101 and g == 1 and b > 0.349 and b < 0.351)
        r, g, b = weather.aurora_high_color()
        assert(r > 0.549 and r < 0.551 and g > 0.149 and g < 0.151 and b > 0.899 and b < 0.901)
        weather.set_aurora{latitude = -72.5, gate_low = 40, gate_high = 50, strength = 2.5, curtain_count = 5.9,
                           azimuth = 90, width = 120, low_color = {0.5, 0.25, 1}, high_color = {0, 0.5, 0.75}, night_only = 0}
    end
    if t == 2 then
        assert(weather.aurora_latitude() == -72.5 and weather.aurora_gate_low() == 40 and weather.aurora_gate_high() == 50)
        assert(weather.aurora_strength() == 2.5 and weather.aurora_curtain_count() == 5)
        assert(weather.aurora_azimuth() == 90 and weather.aurora_width() == 120 and weather.aurora_night_only() == 0)
        local r, g, b = weather.aurora_low_color()
        assert(r == 0.5 and g == 0.25 and b == 1)
        r, g, b = weather.aurora_high_color()
        assert(r == 0 and g == 0.5 and b == 0.75)
        assert(weather.aurora_speed() == 1 and weather.aurora_enabled() == 1)  -- only the given keys changed
        weather.set_aurora{latitude = 1e9, gate_low = 1e9, gate_high = 1e9, strength = 1e9, curtain_count = 1e9,
                           base_height = 1e9, height_span = 1e9, azimuth = 1e9, width = 1e9, speed = 1e9,
                           low_color = {9, 9, 9}, high_color = {9, 9, 9}, night_only = 7, enabled = 0}
    end
    if t == 3 then
        assert(weather.aurora_latitude() == 90 and weather.aurora_gate_low() == 90 and weather.aurora_gate_high() == 90)
        assert(weather.aurora_strength() == 4 and weather.aurora_curtain_count() == 6)
        assert(weather.aurora_base_height() > 0.599 and weather.aurora_base_height() < 0.601)
        assert(weather.aurora_height_span() > 0.799 and weather.aurora_height_span() < 0.801)
        assert(weather.aurora_azimuth() == 360 and weather.aurora_width() == 180 and weather.aurora_speed() == 4)
        local r, g, b = weather.aurora_low_color()
        assert(r == 1 and g == 1 and b == 1)
        r, g, b = weather.aurora_high_color()
        assert(r == 1 and g == 1 and b == 1)
        assert(weather.aurora_night_only() == 1 and weather.aurora_enabled() == 0)
        weather.set_aurora{latitude = -1e9, gate_low = -1, gate_high = -1, strength = -1, curtain_count = 0,
                           base_height = -1, height_span = -1, azimuth = -5, width = 0, speed = -1,
                           low_color = {-1, -1, -1}, high_color = {-1, -1, -1}, night_only = 0, enabled = 3}
        weather.set_aurora{}  -- nothing given, nothing changes
    end
    if t == 4 then
        assert(weather.aurora_latitude() == -90 and weather.aurora_gate_low() == 0 and weather.aurora_gate_high() == 0)
        assert(weather.aurora_strength() == 0 and weather.aurora_curtain_count() == 1)
        assert(weather.aurora_base_height() > 0.019 and weather.aurora_base_height() < 0.021)
        assert(weather.aurora_height_span() > 0.049 and weather.aurora_height_span() < 0.051)
        assert(weather.aurora_azimuth() == 0 and weather.aurora_width() == 10 and weather.aurora_speed() == 0)
        local r, g, b = weather.aurora_low_color()
        assert(r == 0 and g == 0 and b == 0)
        assert(weather.aurora_night_only() == 0 and weather.aurora_enabled() == 1)
    end
end
)");
    r.step(5);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.aurora().curtain_count == 1);
    CHECK(r.aurora().enabled == 1);
}

TEST_CASE("aurora api: aurora_gate and aurora_level are the kit helpers over latitudes, hours and weather") {
    // Every tick the script writes the gate to position.x and the level to position.y.
    AuroraRig r(R"(
function update()
    mce.each("LocalTransform", function(e)
        mce.set(e, "LocalTransform", "position", vector.create(weather.aurora_gate(), weather.aurora_level(), 0))
    end)
end
)");
    r.w.set_resource(kit::Environment3D{});
    r.weather_e = r.w.spawn_with(kit::Weather{});
    r.cloud_e = r.w.spawn_with(kit::CloudLayer{});
    auto set_hour = [&](f32 h) { r.w.try_resource<kit::Environment3D>()->time_of_day = h; };

    u32 nonzero = 0, zero_by_gate = 0;
    for (f32 lat : {-90.0f, -70.0f, -60.0f, -56.0f, -30.0f, 0.0f, 54.0f, 55.0f, 57.0f, 60.0f, 63.0f, 65.0f, 66.0f, 80.0f, 90.0f}) {
        for (f32 hour : {0.0f, 2.0f, 4.5f, 5.5f, 6.0f, 6.5f, 8.0f, 12.0f, 17.0f, 18.5f, 19.5f, 21.0f, 23.5f}) {
            for (int kind = 0; kind < 3; ++kind) {
                r.aurora().latitude_deg = lat;
                set_hour(hour);
                kit::Weather& wx = *r.w.try_get_mut<kit::Weather>(r.weather_e);
                wx.current = kind == 0 ? kit::WeatherKind::Clear : (kind == 1 ? kit::WeatherKind::Rain : kit::WeatherKind::Snow);
                wx.intensity = kind == 0 ? 0.0f : 0.7f;
                r.step(1);
                REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
                const kit::CloudLayer& cl = *r.w.try_get<kit::CloudLayer>(r.cloud_e);
                const f32 k = kind == 0 ? 0.0f : (kind == 1 ? 1.0f : 0.9f);
                const f32 blend = k * wx.intensity;
                const f32 cover = std::clamp((cl.fair_coverage + (cl.overcast_coverage - cl.fair_coverage) * blend) * cl.strength, 0.0f, 1.0f);
                INFO("lat " << lat << " hour " << hour << " kind " << kind);
                CHECK(bits(r.p().position.x) == bits(kit::aurora_latitude_gate(r.aurora())));
                const f32 expected = kit::aurora_level(r.aurora(), night_fade(hour), cover);
                CHECK(bits(r.p().position.y) == bits(expected));
                if (expected > 0.0f) ++nonzero;
                if (r.p().position.x == 0.0f) {
                    ++zero_by_gate;
                    CHECK(r.p().position.y == 0.0f);
                }
            }
        }
    }
    CHECK(nonzero > 50);
    CHECK(zero_by_gate > 50);

    // The extremes: deep night, a fully open gate, clear (the CloudLayer's fair coverage 0.35 still damps): the level is
    // the strength * (1 - 0.85 * 0.35); at noon with night_only it is 0, and night_only 0 shows by day; the overcast deck
    // (coverage 0.9) damps by 1 - 0.85 * 0.9.
    r.aurora().latitude_deg = 80.0f;
    r.aurora().strength = 2.0f;
    r.w.try_get_mut<kit::Weather>(r.weather_e)->current = kit::WeatherKind::Clear;
    r.w.try_get_mut<kit::Weather>(r.weather_e)->intensity = 0.0f;
    set_hour(0.0f);
    r.step(1);
    CHECK(r.p().position.x == 1.0f);
    CHECK(r.p().position.y == doctest::Approx(2.0f * (1.0f - 0.85f * 0.35f)));
    set_hour(12.0f);
    r.step(1);
    CHECK(r.p().position.y == 0.0f);
    r.aurora().night_only = 0;
    r.step(1);
    CHECK(r.p().position.y == doctest::Approx(2.0f * (1.0f - 0.85f * 0.35f)));
    {  // the weather system drives current / intensity from target, so set the target and read what it made
        kit::Weather& wx = *r.w.try_get_mut<kit::Weather>(r.weather_e);
        wx.target = kit::WeatherKind::Overcast;
        wx.transition_seconds = 1.0f;
    }
    r.step(200);
    const kit::Weather& wx = *r.w.try_get<kit::Weather>(r.weather_e);
    REQUIRE(wx.current == kit::WeatherKind::Overcast);
    CHECK(wx.intensity == 1.0f);
    CHECK(r.p().position.y == doctest::Approx(2.0f * (1.0f - 0.85f * 0.9f)));
    // enabled 0 reads the authoring but the level is 0.
    r.aurora().enabled = 0;
    r.step(1);
    CHECK(r.p().position.x == 1.0f);
    CHECK(r.p().position.y == 0.0f);
}

TEST_CASE("aurora api: with no clock the level is 0 unless night_only is 0; no Weather or CloudLayer means no damping") {
    AuroraRig r(R"(
function update()
    mce.each("LocalTransform", function(e)
        mce.set(e, "LocalTransform", "position", vector.create(weather.aurora_gate(), weather.aurora_level(), 0))
    end)
end
)");
    // No Environment3D at all: the default has time_of_day -1 (no clock).
    r.step(1);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.p().position.x == 1.0f);  // the default 68 degrees is past the gate
    CHECK(r.p().position.y == 0.0f);
    r.aurora().night_only = 0;
    r.step(1);
    CHECK(r.p().position.y == 1.0f);
    // An Environment3D resource with no clock behaves the same, and a clock at night with no Weather has no damping.
    r.w.set_resource(kit::Environment3D{});
    r.step(1);
    CHECK(r.p().position.y == 1.0f);
    r.aurora().night_only = 1;
    r.step(1);
    CHECK(r.p().position.y == 0.0f);
    r.w.try_resource<kit::Environment3D>()->time_of_day = 0.0f;
    r.step(1);
    CHECK(bits(r.p().position.y) == bits(kit::aurora_level(r.aurora(), night_fade(0.0f), 0.0f)));
    CHECK(r.p().position.y == doctest::Approx(1.0f));
}

TEST_CASE("aurora api: a world without an Aurora raises a named error and the setter never adds one") {
    for (const char* name : {"aurora_latitude", "aurora_gate_low", "aurora_gate_high", "aurora_strength",
                             "aurora_curtain_count", "aurora_base_height", "aurora_height_span", "aurora_azimuth",
                             "aurora_width", "aurora_speed", "aurora_low_color", "aurora_high_color", "aurora_night_only",
                             "aurora_enabled", "aurora_gate", "aurora_level"})
        expect_error((std::string("weather.") + name + "()").c_str(),
                     (std::string("weather.") + name + ": the scene has no Aurora entity").c_str(), false);
    expect_error("weather.set_aurora{}", "weather.set_aurora: the scene has no Aurora entity", false);
    expect_error("weather.set_aurora{latitude = 70}", "weather.set_aurora: the scene has no Aurora entity", false);
    expect_error("weather.aurora_level()", "weather.aurora_level: this world has no weather", false, false);
    expect_error("weather.set_aurora{}", "weather.set_aurora: this world has no weather", false, false);
    AuroraRig r("function update()\n    if mce.tick() == 1 then weather.set_aurora{enabled = 0} end\nend\n", false);
    r.step(2);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    usize n = 0;
    r.w.query<kit::Aurora>().each([&](Entity, kit::Aurora&) { ++n; });
    CHECK(n == 0);
}

TEST_CASE("aurora api: bad arguments raise an error that names the call and write nothing") {
    expect_error("weather.set_aurora{bogus = 1}", "weather.set_aurora: unknown key 'bogus'");
    expect_error("weather.set_aurora{latitude_deg = 1}", "weather.set_aurora: unknown key 'latitude_deg'");
    expect_error("weather.set_aurora{latitude = 'high'}", "weather.set_aurora: 'latitude' is not a number");
    expect_error("weather.set_aurora{strength = 0/0}", "weather.set_aurora: 'strength' is not a number");
    expect_error("weather.set_aurora{enabled = true}", "weather.set_aurora: 'enabled' is not a number");
    expect_error("weather.set_aurora{low_color = 1}", "weather.set_aurora: 'low_color' is not a table of three numbers");
    expect_error("weather.set_aurora{high_color = {1, 2}}", "weather.set_aurora: 'high_color' is not a table of three numbers");
    expect_error("weather.set_aurora{high_color = {1, 'x', 3}}", "weather.set_aurora: 'high_color' is not a table of three numbers");
    expect_error("weather.set_aurora{[1] = 0.5}", "weather.set_aurora: the keys are names");
    expect_error("weather.set_aurora(0.5)", "table expected");
    expect_error("weather.set_aurora()", "table expected");
    const kit::Aurora def;
    for (const char* body : {"weather.set_aurora{latitude = 10, strength = 'x'}", "weather.set_aurora{strength = 'x', latitude = 10}",
                             "weather.set_aurora{latitude = 10, nope = 1}", "weather.set_aurora{latitude = 10, high_color = {1, 1}}",
                             "weather.set_aurora{high_color = {1, 1}, latitude = 10}",
                             "weather.set_aurora{latitude = 10, low_color = {1, 1, 1}, high_color = 'x'}",
                             "weather.set_aurora{high_color = 'x', latitude = 10, low_color = {1, 1, 1}}"}) {
        const std::string src = std::string("function update()\n    if mce.tick() == 1 then ") + body + " end\nend\n";
        AuroraRig r(src.c_str());
        r.step(2);
        INFO(body);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
        CHECK(r.aurora().latitude_deg == def.latitude_deg);
        CHECK(r.aurora().strength == def.strength);
        CHECK(r.aurora().low_color.x == def.low_color.x);
        CHECK(r.aurora().high_color.y == def.high_color.y);
    }
}
