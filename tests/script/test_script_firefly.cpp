// M-FIREFLIES step 23c: the firefly functions of the `weather` Luau table over the first kit::Firefly. Getters, clamping,
// fireflies_glowing against the kit helper over days, hours, nights and rain, named errors, nothing written on error, and a
// missing Firefly raises (the setter never adds one).

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/firefly.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/season.h"
#include "mce/kit/weather.h"
#include "mce/kit/world_clock.h"
#include "mce/render/atmosphere.h"
#include "mce/script/compiler.h"

using namespace mce;
using script::ScriptHost;
using script::ScriptState;

namespace {

struct FireflyRig {
    World w;
    ScriptHost host;
    u64 tick = 0;
    Entity ff_e, probe, season_e, clock_e, weather_e;

    explicit FireflyRig(const char* source, bool with_firefly = true, bool install = true) {
        w.register_components<LocalTransform>();
        if (install) kit::install_weather(w, nullptr);
        INFO(source);
        Result<Array<u8>> bc = script::compile_unchecked("a", source);
        REQUIRE(bc);
        REQUIRE(host.add_bytecode("a", bc.value().span()));
        host.install(w);
        probe = w.spawn_with(LocalTransform{});
        if (with_firefly) ff_e = w.spawn_with(kit::Firefly{});
    }
    kit::Firefly& ff() { return *w.try_get_mut<kit::Firefly>(ff_e); }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
    const LocalTransform& p() { return *w.try_get<LocalTransform>(probe); }
    const ScriptHost& h() const { return host; }
};

std::string err(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

void expect_error(const char* body, const char* needle, bool with_firefly = true, bool install = true) {
    const std::string src = std::string("function update()\n    if mce.tick() == 1 then\n        ") + body + "\n    end\nend\n";
    FireflyRig r(src.c_str(), with_firefly, install);
    r.step(2);
    INFO(body);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    INFO(err(r.h()));
    CHECK(err(r.h()).find(needle) != std::string::npos);
}

u32 bits(f32 v) { return std::bit_cast<u32>(v); }

// The night fade the extract derives from the sun direction of the hour: 1 - smoothstep over the elevation -0.1..0.1.
f32 night_fade_of(f32 hour) {
    if (!(hour >= 0.0f)) return 0.0f;
    const f32 elevation = -render::sun_direction_at(hour).y;
    const f32 t = std::clamp((elevation + 0.1f) / 0.2f, 0.0f, 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

const char* const kProbeSource = R"(
function update()
    mce.each("LocalTransform", function(e)
        mce.set(e, "LocalTransform", "position", vector.create(weather.fireflies_glowing(), 0, 0))
    end)
end
)";

}  // namespace

TEST_CASE("firefly api: the getters read the first Firefly, the setter writes and clamps to the Range") {
    FireflyRig r(R"(
function update()
    local t = mce.tick()
    local function near(a, b) return math.abs(a - b) < 1e-6 end
    if t == 1 then
        assert(weather.firefly_enabled() == 1 and weather.firefly_manual() == 0 and weather.firefly_strength() == 1)
        assert(near(weather.firefly_summer_start(), 0.2) and near(weather.firefly_summer_full(), 0.3))
        assert(near(weather.firefly_summer_end_full(), 0.45) and near(weather.firefly_summer_end(), 0.55))
        assert(weather.firefly_night_only() == 1 and near(weather.firefly_min_darkness(), 0.6))
        assert(weather.firefly_rate() == 18 and weather.firefly_box_xz_m() == 14 and weather.firefly_height_m() == 3)
        assert(near(weather.firefly_size_m(), 0.08) and weather.firefly_lifetime_s() == 6 and near(weather.firefly_drift_speed(), 0.25))
        assert(weather.firefly_max_particles() == 256 and weather.firefly_seed() == 1)
        local r, g, b = weather.firefly_color()
        assert(near(r, 0.8) and near(g, 1.0) and near(b, 0.35))
        weather.set_firefly{summer_start = 0.25, summer_full = 0.5, summer_end_full = 0.75, summer_end = 0.875, manual = 0.5,
                            night_only = 0, min_darkness = 0.25, rate = 100, box_xz_m = 30, height_m = 12, size_m = 0.5,
                            lifetime_s = 10, drift_speed = 2, max_particles = 2048, seed = 77, strength = 0.75,
                            color = {1, 0.5, 0.25}}
    end
    if t == 2 then
        assert(weather.firefly_summer_start() == 0.25 and weather.firefly_summer_full() == 0.5)
        assert(weather.firefly_summer_end_full() == 0.75 and weather.firefly_summer_end() == 0.875 and weather.firefly_manual() == 0.5)
        assert(weather.firefly_night_only() == 0 and weather.firefly_min_darkness() == 0.25)
        assert(weather.firefly_rate() == 100 and weather.firefly_box_xz_m() == 30 and weather.firefly_height_m() == 12)
        assert(weather.firefly_size_m() == 0.5 and weather.firefly_lifetime_s() == 10 and weather.firefly_drift_speed() == 2)
        assert(weather.firefly_max_particles() == 2048 and weather.firefly_seed() == 77 and weather.firefly_strength() == 0.75)
        local r, g, b = weather.firefly_color()
        assert(r == 1 and g == 0.5 and b == 0.25)
        weather.set_firefly{enabled = 0}
    end
    if t == 3 then
        assert(weather.firefly_enabled() == 0 and weather.firefly_rate() == 100)  -- only the given keys changed
        weather.set_firefly{enabled = 7, summer_start = 1e9, summer_full = 1e9, summer_end_full = 1e9, summer_end = 1e9,
                            manual = 1e9, night_only = -3, min_darkness = 1e9, rate = 1e9, box_xz_m = 1e9, height_m = 1e9,
                            size_m = 1e9, lifetime_s = 1e9, drift_speed = 1e9, max_particles = 1e9, seed = 1e12,
                            strength = 1e9, color = {1e9, -1, 0.5}}
    end
    if t == 4 then
        assert(weather.firefly_enabled() == 1 and weather.firefly_summer_start() == 1 and weather.firefly_summer_full() == 1)
        assert(weather.firefly_summer_end_full() == 1 and weather.firefly_summer_end() == 1 and weather.firefly_manual() == 1)
        assert(weather.firefly_night_only() == 1 and weather.firefly_min_darkness() == 1)
        assert(weather.firefly_rate() == 200 and weather.firefly_box_xz_m() == 100 and weather.firefly_height_m() == 20)
        assert(weather.firefly_size_m() == 0.5 and weather.firefly_lifetime_s() == 30 and weather.firefly_drift_speed() == 2)
        assert(weather.firefly_max_particles() == 4096 and weather.firefly_seed() == 4294967295 and weather.firefly_strength() == 1)
        local r, g, b = weather.firefly_color()
        assert(r == 1 and g == 0 and b == 0.5)
        weather.set_firefly{enabled = 0, summer_start = -1, summer_full = -1, summer_end_full = -1, summer_end = -1,
                            manual = -1, night_only = 0, min_darkness = -1, rate = -1, box_xz_m = -1, height_m = -1,
                            size_m = -1, lifetime_s = -1, drift_speed = -1, max_particles = -1, seed = -1, strength = -1}
        weather.set_firefly{}  -- nothing given, nothing changes
    end
    if t == 5 then
        assert(weather.firefly_enabled() == 0 and weather.firefly_summer_start() == 0 and weather.firefly_summer_full() == 0)
        assert(weather.firefly_summer_end_full() == 0 and weather.firefly_summer_end() == 0 and weather.firefly_manual() == 0)
        assert(weather.firefly_night_only() == 0 and weather.firefly_min_darkness() == 0)
        assert(weather.firefly_rate() == 0 and weather.firefly_box_xz_m() == 1 and weather.firefly_height_m() == 1)
        assert(near(weather.firefly_size_m(), 0.02) and weather.firefly_lifetime_s() == 1 and weather.firefly_drift_speed() == 0)
        assert(weather.firefly_max_particles() == 16 and weather.firefly_seed() == 0 and weather.firefly_strength() == 0)
    end
end
)");
    r.step(6);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.ff().strength == 0.0f);
    CHECK(r.ff().enabled == 0);
    CHECK(r.ff().night_only == 0);
    CHECK(r.ff().pad_[0] == 0);
    CHECK(r.ff().pad_[1] == 0);
}

TEST_CASE("firefly api: fireflies_glowing is the kit helper over days, hours, nights and rain") {
    FireflyRig r(kProbeSource);
    r.w.set_resource(kit::Environment3D{});
    kit::Season season;
    r.season_e = r.w.spawn_with(season);
    kit::WorldClock c;
    c.paused = 1;
    c.time_scale = 0.0f;
    c.drive_environment = 0;
    r.clock_e = r.w.spawn_with(c);
    r.weather_e = r.w.spawn_with(kit::Weather{});
    u32 distinct = 0, zero = 0, nonzero = 0;
    bool saw_one = false;
    f32 last = -1.0f;
    for (f32 year_days : {12.0f, 1.0f, 365.0f}) {
        for (f32 manual : {0.0f, 0.3f}) {
            for (u8 night_only : {u8{1}, u8{0}}) {
                r.w.try_get_mut<kit::Season>(r.season_e)->year_days = year_days;
                r.ff().manual = manual;
                r.ff().night_only = night_only;
                for (u32 day : {0u, 1u, 3u, 4u, 5u, 6u, 7u, 8u, 12u, 400u, 100000u}) {
                    for (f32 hour : {0.0f, 3.0f, 5.5f, 12.0f, 21.0f, 23.5f}) {
                        for (f32 rain_i : {0.0f, 0.5f}) {
                            kit::WorldClock& clk = *r.w.try_get_mut<kit::WorldClock>(r.clock_e);
                            clk.day = day;
                            clk.hour = hour;
                            r.w.try_resource<kit::Environment3D>()->time_of_day = hour;
                            kit::Weather& wx = *r.w.try_get_mut<kit::Weather>(r.weather_e);
                            wx.current = wx.target = rain_i > 0.0f ? kit::WeatherKind::Rain : kit::WeatherKind::Clear;
                            wx.intensity = rain_i;
                            r.step(1);
                            REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
                            // the weather system ran before the script this tick, so its post-tick state is what the script read
                            const kit::WorldClock& after = *r.w.try_get<kit::WorldClock>(r.clock_e);
                            const kit::Season& s = *r.w.try_get<kit::Season>(r.season_e);
                            const bool raining = wx.current == kit::WeatherKind::Rain || wx.current == kit::WeatherKind::Storm;
                            const f32 expected = kit::firefly_amount(r.ff(), &s, kit::season_phase(s, after.day, after.hour),
                                                                     night_fade_of(r.w.try_resource<kit::Environment3D>()->time_of_day),
                                                                     0.0f, raining ? wx.intensity : 0.0f);
                            INFO("year_days " << year_days << " manual " << manual << " night_only " << int(night_only) << " day "
                                              << day << " hour " << hour << " rain " << rain_i);
                            CHECK(bits(r.p().position.x) == bits(expected));
                            CHECK(r.p().position.x >= 0.0f);
                            CHECK(r.p().position.x <= 1.0f);
                            saw_one = saw_one || r.p().position.x == 1.0f;
                            if (r.p().position.x > 0.0f) ++nonzero; else ++zero;
                            if (r.p().position.x != last) ++distinct;
                            last = r.p().position.x;
                        }
                    }
                }
            }
        }
    }
    CHECK(distinct > 10);
    CHECK(nonzero > 50);
    CHECK(zero > 50);
    CHECK(saw_one);
    // enabled 0 and strength 0 are exactly 0, the Season disabled is the manual floor (at night, dry)
    r.w.try_resource<kit::Environment3D>()->time_of_day = 0.0f;
    r.w.try_get_mut<kit::Weather>(r.weather_e)->current = kit::WeatherKind::Clear;
    r.w.try_get_mut<kit::Weather>(r.weather_e)->target = kit::WeatherKind::Clear;
    r.w.try_get_mut<kit::Weather>(r.weather_e)->intensity = 0.0f;
    r.ff() = kit::Firefly{};
    r.ff().manual = 0.3f;
    r.ff().enabled = 0;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
    r.ff().enabled = 1;
    r.ff().strength = 0.0f;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
    r.ff().strength = 1.0f;
    r.w.try_get_mut<kit::Season>(r.season_e)->enabled = 0;
    r.step(1);
    CHECK(bits(r.p().position.x) == bits(0.3f));
    // by day, night_only 1 shows nothing even with the manual floor
    r.w.try_resource<kit::Environment3D>()->time_of_day = 12.0f;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
}

TEST_CASE("firefly api: with no clock the night fade is 0 and the phase the fixed start_phase; with no Season the amount is the manual floor") {
    FireflyRig r(kProbeSource);
    // no Season, no clock, no Environment3D: manual only, never nil; night_only 1 gates it to 0
    r.step(2);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.p().position.x == 0.0f);
    r.ff().manual = 0.625f;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
    r.ff().night_only = 0;
    r.step(1);
    CHECK(r.p().position.x == 0.625f);
    // a negative time_of_day is no clock either
    r.w.set_resource(kit::Environment3D{});
    r.w.try_resource<kit::Environment3D>()->time_of_day = -1.0f;
    r.ff().night_only = 1;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
    // a clock at midnight shows the manual floor
    r.w.try_resource<kit::Environment3D>()->time_of_day = 0.0f;
    r.step(1);
    CHECK(bits(r.p().position.x) == bits(kit::firefly_amount(r.ff(), nullptr, 0.0f, night_fade_of(0.0f), 0.0f, 0.0f)));
    CHECK(r.p().position.x > 0.6f);
    // a Season and no clock (day 0, hour 0)
    r.season_e = r.w.spawn_with(kit::Season{});
    r.ff().manual = 0.0f;
    bool saw_positive = false;
    for (f32 start : {0.0f, 0.3f, 0.375f, 0.5f, 0.8f, 0.25f}) {
        r.w.try_get_mut<kit::Season>(r.season_e)->start_phase = start;
        r.step(2);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
        const kit::Season& s = *r.w.try_get<kit::Season>(r.season_e);
        INFO("start_phase " << start);
        CHECK(bits(r.p().position.x) == bits(kit::firefly_amount(r.ff(), &s, kit::season_phase(s, 0, 0.0f), night_fade_of(0.0f), 0.0f, 0.0f)));
        saw_positive = saw_positive || r.p().position.x > 0.0f;
    }
    CHECK(saw_positive);
}

TEST_CASE("firefly api: a world without a Firefly raises a named error and the setter never adds one") {
    for (const char* name : {"firefly_enabled", "firefly_summer_start", "firefly_summer_full", "firefly_summer_end_full",
                             "firefly_summer_end", "firefly_manual", "firefly_night_only", "firefly_min_darkness", "firefly_rate",
                             "firefly_box_xz_m", "firefly_height_m", "firefly_size_m", "firefly_lifetime_s", "firefly_drift_speed",
                             "firefly_color", "firefly_max_particles", "firefly_seed", "firefly_strength"})
        expect_error((std::string("weather.") + name + "()").c_str(),
                     (std::string("weather.") + name + ": the scene has no Firefly entity").c_str(), false);
    expect_error("weather.fireflies_glowing()", "weather.fireflies_glowing: the scene has no Firefly entity", false);
    expect_error("weather.set_firefly{}", "weather.set_firefly: the scene has no Firefly entity", false);
    expect_error("weather.set_firefly{strength = 0}", "weather.set_firefly: the scene has no Firefly entity", false);
    expect_error("weather.fireflies_glowing()", "weather.fireflies_glowing: this world has no weather", false, false);
    expect_error("weather.set_firefly{}", "weather.set_firefly: this world has no weather", false, false);
    FireflyRig r("function update()\n    if mce.tick() == 1 then weather.set_firefly{enabled = 0} end\nend\n", false);
    r.step(2);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    usize n = 0;
    r.w.query<kit::Firefly>().each([&](Entity, kit::Firefly&) { ++n; });
    CHECK(n == 0);
}

TEST_CASE("firefly api: bad arguments raise an error that names the call and write nothing") {
    expect_error("weather.set_firefly{bogus = 1}", "weather.set_firefly: unknown key 'bogus'");
    expect_error("weather.set_firefly{leaf_color = 1}", "weather.set_firefly: unknown key 'leaf_color'");
    expect_error("weather.set_firefly{rate = 'fast'}", "weather.set_firefly: 'rate' is not a number");
    expect_error("weather.set_firefly{strength = 0/0}", "weather.set_firefly: 'strength' is not a number");
    expect_error("weather.set_firefly{enabled = true}", "weather.set_firefly: 'enabled' is not a number");
    expect_error("weather.set_firefly{night_only = 'x'}", "weather.set_firefly: 'night_only' is not a number");
    expect_error("weather.set_firefly{seed = 'x'}", "weather.set_firefly: 'seed' is not a number");
    expect_error("weather.set_firefly{color = 1}", "weather.set_firefly: 'color' is not a table of three numbers");
    expect_error("weather.set_firefly{color = {1, 2}}", "weather.set_firefly: 'color' is not a table of three numbers");
    expect_error("weather.set_firefly{color = {1, 'x', 3}}", "weather.set_firefly: 'color' is not a table of three numbers");
    expect_error("weather.set_firefly{[1] = 0.5}", "weather.set_firefly: the keys are names");
    expect_error("weather.set_firefly(0.5)", "table expected");
    expect_error("weather.set_firefly()", "table expected");
    const kit::Firefly def;
    for (const char* body : {"weather.set_firefly{manual = 0.9, strength = 'x'}", "weather.set_firefly{strength = 'x', manual = 0.9}",
                             "weather.set_firefly{manual = 0.9, nope = 1}", "weather.set_firefly{enabled = 0, rate = 'x'}",
                             "weather.set_firefly{color = {0, 0, 0}, height_m = 'x'}",
                             "weather.set_firefly{color = {1, 2}, manual = 0.9, enabled = 0}"}) {
        const std::string src = std::string("function update()\n    if mce.tick() == 1 then ") + body + " end\nend\n";
        FireflyRig r(src.c_str());
        r.step(2);
        INFO(body);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
        CHECK(r.ff().manual == def.manual);
        CHECK(r.ff().strength == def.strength);
        CHECK(r.ff().rate == def.rate);
        CHECK(r.ff().enabled == def.enabled);
        CHECK(r.ff().night_only == def.night_only);
        CHECK(r.ff().height_m == def.height_m);
        CHECK(r.ff().color.x == def.color.x);
        CHECK(r.ff().color.z == def.color.z);
    }
}
