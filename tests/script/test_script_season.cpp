// M-SEASONS step 19c: the season functions of the `weather` Luau table over the first kit::Season. Getters, clamping,
// season_phase against the kit helper over days and hours, named errors, nothing written on error, and a missing Season
// raises (the setter never adds one).

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/season.h"
#include "mce/kit/weather.h"
#include "mce/kit/world_clock.h"
#include "mce/script/compiler.h"

using namespace mce;
using script::ScriptHost;
using script::ScriptState;

namespace {

struct SeasonRig {
    World w;
    ScriptHost host;
    u64 tick = 0;
    Entity season_e, probe, clock_e;

    explicit SeasonRig(const char* source, bool with_season = true, bool install = true) {
        w.register_components<LocalTransform>();
        if (install) kit::install_weather(w, nullptr);
        INFO(source);
        Result<Array<u8>> bc = script::compile_unchecked("a", source);
        REQUIRE(bc);
        REQUIRE(host.add_bytecode("a", bc.value().span()));
        host.install(w);
        probe = w.spawn_with(LocalTransform{});
        if (with_season) season_e = w.spawn_with(kit::Season{});
    }
    kit::Season& season() { return *w.try_get_mut<kit::Season>(season_e); }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
    const LocalTransform& p() { return *w.try_get<LocalTransform>(probe); }
    const ScriptHost& h() const { return host; }
};

std::string err(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

void expect_error(const char* body, const char* needle, bool with_season = true, bool install = true) {
    const std::string src = std::string("function update()\n    if mce.tick() == 1 then\n        ") + body + "\n    end\nend\n";
    SeasonRig r(src.c_str(), with_season, install);
    r.step(2);
    INFO(body);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    INFO(err(r.h()));
    CHECK(err(r.h()).find(needle) != std::string::npos);
}

u32 bits(f32 v) { return std::bit_cast<u32>(v); }

}  // namespace

TEST_CASE("season api: the getters read the first Season, the setter writes and clamps to the Range") {
    SeasonRig r(R"(
function update()
    local t = mce.tick()
    if t == 1 then
        assert(weather.season_enabled() == 1 and weather.season_year_days() == 12)
        assert(math.abs(weather.season_start_phase() - 0.1) < 1e-6 and math.abs(weather.season_winter_density() - 0.4) < 1e-6)
        assert(weather.season_snow_cover_bias() == 0.5 and weather.season_cold_bias() == 0.25 and weather.season_strength() == 1)
        local r, g, b = weather.season_spring_tint()
        assert(r == 0.75 and math.abs(g - 1.05) < 1e-6 and math.abs(b - 0.7) < 1e-6)
        r, g, b = weather.season_autumn_tint()
        assert(math.abs(r - 1.35) < 1e-6 and math.abs(g - 0.7) < 1e-6 and math.abs(b - 0.35) < 1e-6)
        r, g, b = weather.season_winter_tint()
        assert(math.abs(r - 0.7) < 1e-6 and math.abs(g - 0.72) < 1e-6 and b == 0.75)
        weather.set_season{year_days = 30, start_phase = 0.5, winter_density = 0.25, snow_cover_bias = 0.75, cold_bias = 0.5,
                           strength = 0.5, spring_tint = {1, 0.5, 0.25}, autumn_tint = {2, 3, 0.125}}
    end
    if t == 2 then
        assert(weather.season_year_days() == 30 and weather.season_start_phase() == 0.5 and weather.season_winter_density() == 0.25)
        assert(weather.season_snow_cover_bias() == 0.75 and weather.season_cold_bias() == 0.5 and weather.season_strength() == 0.5)
        local r, g, b = weather.season_spring_tint()
        assert(r == 1 and g == 0.5 and b == 0.25)
        r, g, b = weather.season_autumn_tint()
        assert(r == 2 and g == 3 and b == 0.125)
        r, g, b = weather.season_winter_tint()
        assert(r > 0.69 and r < 0.71)  -- not given, unchanged
        weather.set_season{enabled = 0}
    end
    if t == 3 then
        assert(weather.season_enabled() == 0 and weather.season_year_days() == 30)  -- only the given keys changed
        weather.set_season{enabled = 7, year_days = 1e9, start_phase = 1e9, winter_density = 1e9, snow_cover_bias = 1e9,
                           cold_bias = 1e9, strength = 1e9, spring_tint = {1e9, -1, 2}, winter_tint = {-5, 9, 4}}
    end
    if t == 4 then
        assert(weather.season_enabled() == 1 and weather.season_year_days() == 3650 and weather.season_start_phase() == 1)
        assert(weather.season_winter_density() == 1 and weather.season_snow_cover_bias() == 1 and weather.season_cold_bias() == 1)
        assert(weather.season_strength() == 1)
        local r, g, b = weather.season_spring_tint()
        assert(r == 4 and g == 0 and b == 2)
        r, g, b = weather.season_winter_tint()
        assert(r == 0 and g == 4 and b == 4)
        weather.set_season{enabled = 0, year_days = -1, start_phase = -1, winter_density = -1, snow_cover_bias = -1, cold_bias = -1,
                           strength = -1}
        weather.set_season{}  -- nothing given, nothing changes
    end
    if t == 5 then
        assert(weather.season_enabled() == 0 and weather.season_year_days() == 1 and weather.season_start_phase() == 0)
        assert(weather.season_winter_density() == 0 and weather.season_snow_cover_bias() == 0 and weather.season_cold_bias() == 0)
        assert(weather.season_strength() == 0)
    end
end
)");
    r.step(6);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.season().year_days == 1.0f);
    CHECK(r.season().enabled == 0);
    CHECK(r.season().pad_[0] == 0);
    CHECK(r.season().pad_[1] == 0);
    CHECK(r.season().pad_[2] == 0);
}

TEST_CASE("season api: season_phase is the kit helper over days and hours") {
    // Every tick the script writes the phase to position.x.
    SeasonRig r(R"(
function update()
    mce.each("LocalTransform", function(e)
        mce.set(e, "LocalTransform", "position", vector.create(weather.season_phase(), 0, 0))
    end)
end
)");
    kit::WorldClock c;
    c.paused = 1;
    c.time_scale = 0.0f;
    r.clock_e = r.w.spawn_with(c);
    u32 distinct = 0;
    f32 last = -1.0f;
    for (f32 year_days : {12.0f, 1.0f, 365.0f}) {
        for (f32 start : {0.0f, 0.1f, 0.9f}) {
            r.season().year_days = year_days;
            r.season().start_phase = start;
            for (u32 day : {0u, 1u, 5u, 11u, 12u, 400u, 100000u}) {
                for (f32 hour : {0.0f, 5.0f, 12.0f, 23.5f}) {
                    kit::WorldClock& clk = *r.w.try_get_mut<kit::WorldClock>(r.clock_e);
                    clk.day = day;
                    clk.hour = hour;
                    r.step(1);
                    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
                    // the clock system ran before the script this tick: compare against what the script saw
                    const kit::WorldClock& after = *r.w.try_get<kit::WorldClock>(r.clock_e);
                    const f32 expected = kit::season_phase(r.season(), after.day, after.hour);
                    INFO("year_days " << year_days << " start " << start << " day " << day << " hour " << hour);
                    CHECK(bits(r.p().position.x) == bits(expected));
                    CHECK(r.p().position.x >= 0.0f);
                    CHECK(r.p().position.x < 1.0f);
                    if (r.p().position.x != last) ++distinct;
                    last = r.p().position.x;
                }
            }
        }
    }
    CHECK(distinct > 100);
    // The phase is authored data even when the season is disabled.
    r.season().enabled = 0;
    r.step(1);
    const kit::WorldClock& after = *r.w.try_get<kit::WorldClock>(r.clock_e);
    CHECK(bits(r.p().position.x) == bits(kit::season_phase(r.season(), after.day, after.hour)));
}

TEST_CASE("season api: with no clock the phase is the fixed start_phase") {
    SeasonRig r(R"(
function update()
    mce.each("LocalTransform", function(e)
        mce.set(e, "LocalTransform", "position", vector.create(weather.season_phase(), 0, 0))
    end)
end
)");
    for (f32 start : {0.0f, 0.1f, 0.3f, 0.625f, 0.95f}) {
        r.season().start_phase = start;
        r.step(3);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
        CHECK(bits(r.p().position.x) == bits(kit::season_phase(r.season(), 0, 0.0f)));
        CHECK(r.p().position.x == start);
    }
}

TEST_CASE("season api: a world without a Season raises a named error and the setter never adds one") {
    for (const char* name : {"season_enabled", "season_year_days", "season_start_phase", "season_winter_density",
                             "season_snow_cover_bias", "season_cold_bias", "season_strength", "season_spring_tint",
                             "season_autumn_tint", "season_winter_tint", "season_phase"})
        expect_error((std::string("weather.") + name + "()").c_str(),
                     (std::string("weather.") + name + ": the scene has no Season entity").c_str(), false);
    expect_error("weather.set_season{}", "weather.set_season: the scene has no Season entity", false);
    expect_error("weather.set_season{strength = 0}", "weather.set_season: the scene has no Season entity", false);
    expect_error("weather.season_phase()", "weather.season_phase: this world has no weather", false, false);
    expect_error("weather.set_season{}", "weather.set_season: this world has no weather", false, false);
    SeasonRig r("function update()\n    if mce.tick() == 1 then weather.set_season{enabled = 0} end\nend\n", false);
    r.step(2);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    usize n = 0;
    r.w.query<kit::Season>().each([&](Entity, kit::Season&) { ++n; });
    CHECK(n == 0);
}

TEST_CASE("season api: bad arguments raise an error that names the call and write nothing") {
    expect_error("weather.set_season{bogus = 1}", "weather.set_season: unknown key 'bogus'");
    expect_error("weather.set_season{tint = 1}", "weather.set_season: unknown key 'tint'");
    expect_error("weather.set_season{year_days = 'long'}", "weather.set_season: 'year_days' is not a number");
    expect_error("weather.set_season{strength = 0/0}", "weather.set_season: 'strength' is not a number");
    expect_error("weather.set_season{enabled = true}", "weather.set_season: 'enabled' is not a number");
    expect_error("weather.set_season{spring_tint = 1}", "weather.set_season: 'spring_tint' is not a table of three numbers");
    expect_error("weather.set_season{autumn_tint = {1, 2}}", "weather.set_season: 'autumn_tint' is not a table of three numbers");
    expect_error("weather.set_season{winter_tint = {1, 'x', 3}}", "weather.set_season: 'winter_tint' is not a table of three numbers");
    expect_error("weather.set_season{[1] = 0.5}", "weather.set_season: the keys are names");
    expect_error("weather.set_season(0.5)", "table expected");
    expect_error("weather.set_season()", "table expected");
    const kit::Season def;
    for (const char* body : {"weather.set_season{year_days = 9, strength = 'x'}", "weather.set_season{strength = 'x', year_days = 9}",
                             "weather.set_season{year_days = 9, nope = 1}", "weather.set_season{enabled = 0, cold_bias = 'x'}",
                             "weather.set_season{spring_tint = {2, 2, 2}, winter_tint = {1, 2}}",
                             "weather.set_season{winter_tint = {1, 2}, spring_tint = {2, 2, 2}, enabled = 0}"}) {
        const std::string src = std::string("function update()\n    if mce.tick() == 1 then ") + body + " end\nend\n";
        SeasonRig r(src.c_str());
        r.step(2);
        INFO(body);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
        CHECK(r.season().year_days == def.year_days);
        CHECK(r.season().strength == def.strength);
        CHECK(r.season().cold_bias == def.cold_bias);
        CHECK(r.season().enabled == def.enabled);
        CHECK(r.season().spring_tint.x == def.spring_tint.x);
        CHECK(r.season().winter_tint.y == def.winter_tint.y);
    }
}
