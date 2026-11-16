// M-METEOR step 18d: the meteor functions of the `weather` Luau table over the first kit::Meteor. Getters, clamping,
// meteor_level against the kit helper over hours, weather and cloud cover, named errors, nothing written on error, and a
// missing Meteor raises (the setter never adds one).

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/cloud_layer.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/meteor.h"
#include "mce/kit/weather.h"
#include "mce/render/atmosphere.h"
#include "mce/script/compiler.h"

using namespace mce;
using script::ScriptHost;
using script::ScriptState;

namespace {

struct MeteorRig {
    World w;
    ScriptHost host;
    u64 tick = 0;
    Entity meteor_e, probe, weather_e, cloud_e;

    explicit MeteorRig(const char* source, bool with_meteor = true, bool install = true) {
        w.register_components<LocalTransform>();
        if (install) kit::install_weather(w, nullptr);
        INFO(source);
        Result<Array<u8>> bc = script::compile_unchecked("a", source);
        REQUIRE(bc);
        REQUIRE(host.add_bytecode("a", bc.value().span()));
        host.install(w);
        probe = w.spawn_with(LocalTransform{});
        if (with_meteor) meteor_e = w.spawn_with(kit::Meteor{});
    }
    kit::Meteor& meteor() { return *w.try_get_mut<kit::Meteor>(meteor_e); }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
    const LocalTransform& p() { return *w.try_get<LocalTransform>(probe); }
    const ScriptHost& h() const { return host; }
};

std::string err(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

void expect_error(const char* body, const char* needle, bool with_meteor = true, bool install = true) {
    const std::string src = std::string("function update()\n    if mce.tick() == 1 then\n        ") + body + "\n    end\nend\n";
    MeteorRig r(src.c_str(), with_meteor, install);
    r.step(2);
    INFO(body);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    INFO(err(r.h()));
    CHECK(err(r.h()).find(needle) != std::string::npos);
}

u32 bits(f32 v) { return std::bit_cast<u32>(v); }

// The night fade the extract derives from the sun direction of the hour: 1 - smoothstep over the elevation -0.1..0.1.
f32 night_fade_of(f32 hour) {
    const f32 elevation = -render::sun_direction_at(hour).y;
    const f32 t = std::clamp((elevation + 0.1f) / 0.2f, 0.0f, 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

f32 cloud_cover_of(const kit::Weather& wx, const kit::CloudLayer& cl) {
    const f32 k = wx.current == kit::WeatherKind::Clear ? 0.0f : (wx.current == kit::WeatherKind::Snow ? 0.9f : 1.0f);
    const f32 inten = std::clamp(wx.intensity, 0.0f, 1.0f);
    return std::clamp((cl.fair_coverage + (cl.overcast_coverage - cl.fair_coverage) * (k * inten)) * cl.strength, 0.0f, 1.0f);
}

}  // namespace

TEST_CASE("meteor api: the getters read the first Meteor, the setter writes and clamps to the Range") {
    MeteorRig r(R"(
function update()
    local t = mce.tick()
    if t == 1 then
        assert(weather.meteor_rate() == 4 and weather.meteor_brightness() == 1 and weather.meteor_length() == 1)
        assert(weather.meteor_speed() == 1 and weather.meteor_seed() == 0 and weather.meteor_enabled() == 1)
        assert(weather.meteor_night_only() == 1 and math.abs(weather.meteor_min_darkness() - 0.6) < 1e-6)
        weather.set_meteor{rate = 12.5, brightness = 2.5, length = 1.5, speed = 0.75, min_darkness = 0.5, seed = 77.9}
    end
    if t == 2 then
        assert(weather.meteor_rate() == 12.5 and weather.meteor_brightness() == 2.5 and weather.meteor_length() == 1.5)
        assert(weather.meteor_speed() == 0.75 and weather.meteor_min_darkness() == 0.5 and weather.meteor_seed() == 77)
        assert(weather.meteor_night_only() == 1 and weather.meteor_enabled() == 1)
        weather.set_meteor{enabled = 0, night_only = 0}
    end
    if t == 3 then
        assert(weather.meteor_rate() == 12.5 and weather.meteor_enabled() == 0 and weather.meteor_night_only() == 0)  -- only the given keys changed
        weather.set_meteor{rate = 1e9, brightness = 1e9, length = 1e9, speed = 1e9, min_darkness = 1e9, seed = 1e9, enabled = 7, night_only = 3}
    end
    if t == 4 then
        assert(weather.meteor_rate() == 30 and weather.meteor_brightness() == 4 and weather.meteor_length() == 2)
        assert(weather.meteor_speed() == 2 and weather.meteor_min_darkness() == 1 and weather.meteor_seed() == 65535)
        assert(weather.meteor_enabled() == 1 and weather.meteor_night_only() == 1)
        weather.set_meteor{rate = -1, brightness = -1, length = -1, speed = -1, min_darkness = -1, seed = -1, enabled = 0, night_only = 0}
        weather.set_meteor{}  -- nothing given, nothing changes
    end
    if t == 5 then
        assert(weather.meteor_rate() == 0 and weather.meteor_brightness() == 0 and weather.meteor_length() == 0.5)
        assert(weather.meteor_speed() == 0.5 and weather.meteor_min_darkness() == 0 and weather.meteor_seed() == 0)
        assert(weather.meteor_enabled() == 0 and weather.meteor_night_only() == 0)
    end
end
)");
    r.step(6);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.meteor().length == 0.5f);
    CHECK(r.meteor().enabled == 0);
    CHECK(r.meteor().pad_[0] == 0);
    CHECK(r.meteor().pad_[1] == 0);
}

TEST_CASE("meteor api: meteor_level is the kit helper over hours, weather and cloud cover") {
    // Every tick the script writes the level to position.x.
    MeteorRig r(R"(
function update()
    mce.each("LocalTransform", function(e)
        mce.set(e, "LocalTransform", "position", vector.create(weather.meteor_level(), 0, 0))
    end)
end
)");
    r.w.set_resource(kit::Environment3D{});
    r.weather_e = r.w.spawn_with(kit::Weather{});
    r.cloud_e = r.w.spawn_with(kit::CloudLayer{});
    auto set_hour = [&](f32 h) { r.w.try_resource<kit::Environment3D>()->time_of_day = h; };

    u32 nonzero = 0, zero = 0;
    for (f32 hour : {0.0f, 2.0f, 4.0f, 5.0f, 6.0f, 6.5f, 8.0f, 12.0f, 16.0f, 18.0f, 19.0f, 19.5f, 20.0f, 22.0f}) {
        for (f32 inten : {0.0f, 0.3f, 0.6f, 1.0f}) {
            for (int kind = 0; kind < 4; ++kind) {
                for (u8 night_only : {u8{1}, u8{0}}) {
                    set_hour(hour);
                    r.meteor().night_only = night_only;
                    kit::Weather& wx = *r.w.try_get_mut<kit::Weather>(r.weather_e);
                    wx.current = kind == 0 ? kit::WeatherKind::Clear
                                 : kind == 1 ? kit::WeatherKind::Rain
                                 : kind == 2 ? kit::WeatherKind::Snow
                                             : kit::WeatherKind::Storm;
                    wx.target = wx.current;
                    wx.intensity = kind == 0 ? 0.0f : inten;
                    r.step(1);
                    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
                    // the system ran before the script this tick, so its post-tick state is what the script read
                    const kit::CloudLayer& cl = *r.w.try_get<kit::CloudLayer>(r.cloud_e);
                    const bool raining = wx.current == kit::WeatherKind::Rain || wx.current == kit::WeatherKind::Storm;
                    const f32 expected =
                        kit::meteor_level(r.meteor(), night_fade_of(hour), cloud_cover_of(wx, cl), raining ? wx.intensity : 0.0f);
                    INFO("hour " << hour << " intensity " << inten << " kind " << kind << " night_only " << int(night_only));
                    CHECK(bits(r.p().position.x) == bits(expected));
                    if (expected > 0.0f) ++nonzero; else ++zero;
                }
            }
        }
    }
    CHECK(nonzero > 20);
    CHECK(zero > 50);

    // A clear night shows; the same night disabled, in the rain, by day (night_only) and with rate or brightness 0 is 0.
    r.meteor().night_only = 1;
    kit::Weather& wx = *r.w.try_get_mut<kit::Weather>(r.weather_e);
    wx.current = wx.target = kit::WeatherKind::Clear;
    wx.intensity = 0.0f;
    set_hour(0.0f);
    r.step(1);
    const f32 night = r.p().position.x;
    CHECK(night > 0.0f);
    CHECK(night <= 1.0f);
    set_hour(12.0f);
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
    r.meteor().night_only = 0;
    r.step(1);
    CHECK(r.p().position.x > 0.0f);  // by day too
    r.meteor().night_only = 1;
    set_hour(0.0f);
    for (int what = 0; what < 3; ++what) {
        r.meteor() = kit::Meteor{};
        if (what == 0) r.meteor().enabled = 0;
        if (what == 1) r.meteor().rate = 0.0f;
        if (what == 2) r.meteor().brightness = 0.0f;
        r.step(1);
        CHECK(r.p().position.x == 0.0f);
    }
    r.meteor() = kit::Meteor{};
    wx.current = wx.target = kit::WeatherKind::Rain;
    wx.intensity = 1.0f;
    r.step(1);
    // the weather system owns the intensity: compare with the helper on the post-tick state, which the rain damps
    const f32 wet_night = kit::meteor_level(r.meteor(), night_fade_of(0.0f), cloud_cover_of(wx, *r.w.try_get<kit::CloudLayer>(r.cloud_e)),
                                            wx.intensity);
    CHECK(bits(r.p().position.x) == bits(wet_night));
    CHECK(r.p().position.x < night);
}

TEST_CASE("meteor api: with no clock the level is 0 unless night_only is 0; no Weather or CloudLayer; sky texture") {
    MeteorRig r(R"(
function update()
    mce.each("LocalTransform", function(e)
        mce.set(e, "LocalTransform", "position", vector.create(weather.meteor_level(), 0, 0))
    end)
end
)");
    // No Environment3D at all (no clock) and no Weather: night_only 1 shows nothing.
    r.step(1);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.p().position.x == 0.0f);
    r.meteor().night_only = 0;
    r.step(1);
    CHECK(bits(r.p().position.x) == bits(kit::meteor_level(r.meteor(), 0.0f, 0.0f, 0.0f)));
    CHECK(r.p().position.x > 0.0f);
    // A negative time_of_day is no clock either.
    r.w.set_resource(kit::Environment3D{});
    kit::Environment3D& env = *r.w.try_resource<kit::Environment3D>();
    env.time_of_day = -1.0f;
    r.step(1);
    CHECK(bits(r.p().position.x) == bits(kit::meteor_level(r.meteor(), 0.0f, 0.0f, 0.0f)));
    r.meteor().night_only = 1;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
    // A clock at midnight shows without a Weather; an equirect sky texture wins: no meteors.
    env.time_of_day = 0.0f;
    r.step(1);
    CHECK(bits(r.p().position.x) == bits(kit::meteor_level(r.meteor(), night_fade_of(0.0f), 0.0f, 0.0f)));
    CHECK(r.p().position.x > 0.0f);
    env.sky_texture.guid = asset::Guid{1, 2};
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
}

TEST_CASE("meteor api: a world without a Meteor raises a named error and the setter never adds one") {
    for (const char* name : {"meteor_rate", "meteor_brightness", "meteor_length", "meteor_speed", "meteor_min_darkness",
                             "meteor_night_only", "meteor_seed", "meteor_enabled", "meteor_level"})
        expect_error((std::string("weather.") + name + "()").c_str(),
                     (std::string("weather.") + name + ": the scene has no Meteor entity").c_str(), false);
    expect_error("weather.set_meteor{}", "weather.set_meteor: the scene has no Meteor entity", false);
    expect_error("weather.set_meteor{rate = 2}", "weather.set_meteor: the scene has no Meteor entity", false);
    expect_error("weather.meteor_level()", "weather.meteor_level: this world has no weather", false, false);
    expect_error("weather.set_meteor{}", "weather.set_meteor: this world has no weather", false, false);
    MeteorRig r("function update()\n    if mce.tick() == 1 then weather.set_meteor{enabled = 0} end\nend\n", false);
    r.step(2);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    usize n = 0;
    r.w.query<kit::Meteor>().each([&](Entity, kit::Meteor&) { ++n; });
    CHECK(n == 0);
}

TEST_CASE("meteor api: bad arguments raise an error that names the call and write nothing") {
    expect_error("weather.set_meteor{bogus = 1}", "weather.set_meteor: unknown key 'bogus'");
    expect_error("weather.set_meteor{min_darkness_x = 1}", "weather.set_meteor: unknown key 'min_darkness_x'");
    expect_error("weather.set_meteor{rate = 'high'}", "weather.set_meteor: 'rate' is not a number");
    expect_error("weather.set_meteor{seed = 0/0}", "weather.set_meteor: 'seed' is not a number");
    expect_error("weather.set_meteor{enabled = true}", "weather.set_meteor: 'enabled' is not a number");
    expect_error("weather.set_meteor{[1] = 0.5}", "weather.set_meteor: the keys are names");
    expect_error("weather.set_meteor(0.5)", "table expected");
    expect_error("weather.set_meteor()", "table expected");
    const kit::Meteor def;
    for (const char* body : {"weather.set_meteor{rate = 9, speed = 'x'}", "weather.set_meteor{speed = 'x', rate = 9}",
                             "weather.set_meteor{rate = 9, nope = 1}", "weather.set_meteor{enabled = 0, night_only = 'x'}",
                             "weather.set_meteor{seed = 'x', enabled = 0, rate = 9}"}) {
        const std::string src = std::string("function update()\n    if mce.tick() == 1 then ") + body + " end\nend\n";
        MeteorRig r(src.c_str());
        r.step(2);
        INFO(body);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
        CHECK(r.meteor().rate == def.rate);
        CHECK(r.meteor().speed == def.speed);
        CHECK(r.meteor().seed == def.seed);
        CHECK(r.meteor().night_only == def.night_only);
        CHECK(r.meteor().enabled == def.enabled);
    }
}
