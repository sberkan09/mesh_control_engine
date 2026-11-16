// M-FREEZE step 20c: the water-ice functions of the `weather` Luau table over the first kit::WaterIce. Getters, clamping,
// water_frozen against the kit helper over days and hours, named errors, nothing written on error, and a missing WaterIce
// raises (the setter never adds one).

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/season.h"
#include "mce/kit/water_ice.h"
#include "mce/kit/weather.h"
#include "mce/kit/world_clock.h"
#include "mce/script/compiler.h"

using namespace mce;
using script::ScriptHost;
using script::ScriptState;

namespace {

struct IceRig {
    World w;
    ScriptHost host;
    u64 tick = 0;
    Entity ice_e, probe, season_e, clock_e;

    explicit IceRig(const char* source, bool with_ice = true, bool install = true) {
        w.register_components<LocalTransform>();
        if (install) kit::install_weather(w, nullptr);
        INFO(source);
        Result<Array<u8>> bc = script::compile_unchecked("a", source);
        REQUIRE(bc);
        REQUIRE(host.add_bytecode("a", bc.value().span()));
        host.install(w);
        probe = w.spawn_with(LocalTransform{});
        if (with_ice) ice_e = w.spawn_with(kit::WaterIce{});
    }
    kit::WaterIce& ice() { return *w.try_get_mut<kit::WaterIce>(ice_e); }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
    const LocalTransform& p() { return *w.try_get<LocalTransform>(probe); }
    const ScriptHost& h() const { return host; }
};

std::string err(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

void expect_error(const char* body, const char* needle, bool with_ice = true, bool install = true) {
    const std::string src = std::string("function update()\n    if mce.tick() == 1 then\n        ") + body + "\n    end\nend\n";
    IceRig r(src.c_str(), with_ice, install);
    r.step(2);
    INFO(body);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    INFO(err(r.h()));
    CHECK(err(r.h()).find(needle) != std::string::npos);
}

u32 bits(f32 v) { return std::bit_cast<u32>(v); }

}  // namespace

TEST_CASE("water ice api: the getters read the first WaterIce, the setter writes and clamps to the Range") {
    IceRig r(R"(
function update()
    local t = mce.tick()
    if t == 1 then
        assert(weather.water_ice_enabled() == 1 and weather.water_ice_manual() == 0 and weather.water_ice_strength() == 1)
        assert(math.abs(weather.water_ice_freeze_start() - 0.5) < 1e-6 and math.abs(weather.water_ice_freeze_full() - 0.9) < 1e-6)
        assert(math.abs(weather.water_ice_ice_roughness() - 0.35) < 1e-6 and math.abs(weather.water_ice_ice_reflectivity() - 0.55) < 1e-6)
        assert(math.abs(weather.water_ice_ice_depth_m() - 0.4) < 1e-6 and weather.water_ice_calm() == 1)
        local r, g, b = weather.water_ice_ice_color()
        assert(math.abs(r - 0.62) < 1e-6 and math.abs(g - 0.78) < 1e-6 and math.abs(b - 0.85) < 1e-6)
        weather.set_water_ice{freeze_start = 0.25, freeze_full = 0.75, manual = 0.5, ice_roughness = 0.125, ice_reflectivity = 0.25,
                              ice_depth_m = 2, calm = 0.5, strength = 0.75, ice_color = {1, 0.5, 0.25}}
    end
    if t == 2 then
        assert(weather.water_ice_freeze_start() == 0.25 and weather.water_ice_freeze_full() == 0.75 and weather.water_ice_manual() == 0.5)
        assert(weather.water_ice_ice_roughness() == 0.125 and weather.water_ice_ice_reflectivity() == 0.25)
        assert(weather.water_ice_ice_depth_m() == 2 and weather.water_ice_calm() == 0.5 and weather.water_ice_strength() == 0.75)
        local r, g, b = weather.water_ice_ice_color()
        assert(r == 1 and g == 0.5 and b == 0.25)
        weather.set_water_ice{enabled = 0}
    end
    if t == 3 then
        assert(weather.water_ice_enabled() == 0 and weather.water_ice_ice_depth_m() == 2)  -- only the given keys changed
        weather.set_water_ice{enabled = 7, freeze_start = 1e9, freeze_full = 1e9, manual = 1e9, ice_roughness = 1e9,
                              ice_reflectivity = 1e9, ice_depth_m = 1e9, calm = 1e9, strength = 1e9, ice_color = {1e9, -1, 0.5}}
    end
    if t == 4 then
        assert(weather.water_ice_enabled() == 1 and weather.water_ice_freeze_start() == 1 and weather.water_ice_freeze_full() == 1)
        assert(weather.water_ice_manual() == 1 and weather.water_ice_ice_roughness() == 1 and weather.water_ice_ice_reflectivity() == 1)
        assert(weather.water_ice_ice_depth_m() == 10 and weather.water_ice_calm() == 1 and weather.water_ice_strength() == 1)
        local r, g, b = weather.water_ice_ice_color()
        assert(r == 1 and g == 0 and b == 0.5)
        weather.set_water_ice{enabled = 0, freeze_start = -1, freeze_full = -1, manual = -1, ice_roughness = -1,
                              ice_reflectivity = -1, ice_depth_m = -1, calm = -1, strength = -1}
        weather.set_water_ice{}  -- nothing given, nothing changes
    end
    if t == 5 then
        assert(weather.water_ice_enabled() == 0 and weather.water_ice_freeze_start() == 0 and weather.water_ice_freeze_full() == 0)
        assert(weather.water_ice_manual() == 0 and weather.water_ice_ice_roughness() == 0 and weather.water_ice_ice_reflectivity() == 0)
        assert(math.abs(weather.water_ice_ice_depth_m() - 0.05) < 1e-6 and weather.water_ice_calm() == 0 and weather.water_ice_strength() == 0)
    end
end
)");
    r.step(6);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.ice().strength == 0.0f);
    CHECK(r.ice().enabled == 0);
    CHECK(r.ice().pad_[0] == 0);
    CHECK(r.ice().pad_[1] == 0);
    CHECK(r.ice().pad_[2] == 0);
}

TEST_CASE("water ice api: water_frozen is the kit helper over days and hours") {
    // Every tick the script writes the amount to position.x.
    IceRig r(R"(
function update()
    mce.each("LocalTransform", function(e)
        mce.set(e, "LocalTransform", "position", vector.create(weather.water_frozen(), 0, 0))
    end)
end
)");
    kit::Season season;
    r.season_e = r.w.spawn_with(season);
    kit::WorldClock c;
    c.paused = 1;
    c.time_scale = 0.0f;
    c.drive_environment = 0;
    r.clock_e = r.w.spawn_with(c);
    u32 distinct = 0;
    bool saw_zero = false, saw_one = false;
    f32 last = -1.0f;
    for (f32 year_days : {12.0f, 1.0f, 365.0f}) {
        for (f32 manual : {0.0f, 0.3f}) {
            r.w.try_get_mut<kit::Season>(r.season_e)->year_days = year_days;
            r.ice().manual = manual;
            for (u32 day : {0u, 1u, 5u, 8u, 9u, 10u, 11u, 12u, 400u, 100000u}) {
                for (f32 hour : {0.0f, 5.0f, 12.0f, 23.5f}) {
                    kit::WorldClock& clk = *r.w.try_get_mut<kit::WorldClock>(r.clock_e);
                    clk.day = day;
                    clk.hour = hour;
                    r.step(1);
                    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
                    const kit::WorldClock& after = *r.w.try_get<kit::WorldClock>(r.clock_e);
                    const kit::Season& s = *r.w.try_get<kit::Season>(r.season_e);
                    const f32 expected = kit::water_ice_amount(r.ice(), &s, kit::season_phase(s, after.day, after.hour));
                    INFO("year_days " << year_days << " manual " << manual << " day " << day << " hour " << hour);
                    CHECK(bits(r.p().position.x) == bits(expected));
                    CHECK(r.p().position.x >= manual);
                    CHECK(r.p().position.x <= 1.0f);
                    saw_zero = saw_zero || r.p().position.x == 0.0f;
                    saw_one = saw_one || r.p().position.x == 1.0f;
                    if (r.p().position.x != last) ++distinct;
                    last = r.p().position.x;
                }
            }
        }
    }
    CHECK(distinct > 10);
    CHECK(saw_zero);
    CHECK(saw_one);
    // enabled 0 and strength 0 are exactly 0, the Season disabled is the manual floor
    r.ice().manual = 0.3f;
    r.ice().enabled = 0;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
    r.ice().enabled = 1;
    r.ice().strength = 0.0f;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
    r.ice().strength = 1.0f;
    r.w.try_get_mut<kit::Season>(r.season_e)->enabled = 0;
    r.step(1);
    CHECK(bits(r.p().position.x) == bits(0.3f));
}

TEST_CASE("water ice api: with no clock the phase is the fixed start_phase, with no Season the amount is the manual floor") {
    IceRig r(R"(
function update()
    mce.each("LocalTransform", function(e)
        mce.set(e, "LocalTransform", "position", vector.create(weather.water_frozen(), 0, 0))
    end)
end
)");
    // no Season, no clock: manual only, never nil
    r.step(2);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.p().position.x == 0.0f);
    r.ice().manual = 0.625f;
    r.step(1);
    CHECK(r.p().position.x == 0.625f);
    // a Season and no clock
    r.season_e = r.w.spawn_with(kit::Season{});
    r.ice().manual = 0.0f;
    for (f32 start : {0.0f, 0.3f, 0.625f, 0.8f, 0.95f, 0.875f}) {
        r.w.try_get_mut<kit::Season>(r.season_e)->start_phase = start;
        r.step(2);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
        const kit::Season& s = *r.w.try_get<kit::Season>(r.season_e);
        INFO("start_phase " << start);
        CHECK(bits(r.p().position.x) == bits(kit::water_ice_amount(r.ice(), &s, kit::season_phase(s, 0, 0.0f))));
    }
    CHECK(r.p().position.x > 0.0f);  // 0.875 is the centre of winter
}

TEST_CASE("water ice api: a world without a WaterIce raises a named error and the setter never adds one") {
    for (const char* name : {"water_ice_enabled", "water_ice_freeze_start", "water_ice_freeze_full", "water_ice_manual",
                             "water_ice_ice_color", "water_ice_ice_roughness", "water_ice_ice_reflectivity",
                             "water_ice_ice_depth_m", "water_ice_calm", "water_ice_strength"})
        expect_error((std::string("weather.") + name + "()").c_str(),
                     (std::string("weather.") + name + ": the scene has no WaterIce entity").c_str(), false);
    expect_error("weather.water_frozen()", "weather.water_frozen: the scene has no WaterIce entity", false);
    expect_error("weather.set_water_ice{}", "weather.set_water_ice: the scene has no WaterIce entity", false);
    expect_error("weather.set_water_ice{strength = 0}", "weather.set_water_ice: the scene has no WaterIce entity", false);
    expect_error("weather.water_frozen()", "weather.water_frozen: this world has no weather", false, false);
    expect_error("weather.set_water_ice{}", "weather.set_water_ice: this world has no weather", false, false);
    IceRig r("function update()\n    if mce.tick() == 1 then weather.set_water_ice{enabled = 0} end\nend\n", false);
    r.step(2);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    usize n = 0;
    r.w.query<kit::WaterIce>().each([&](Entity, kit::WaterIce&) { ++n; });
    CHECK(n == 0);
}

TEST_CASE("water ice api: bad arguments raise an error that names the call and write nothing") {
    expect_error("weather.set_water_ice{bogus = 1}", "weather.set_water_ice: unknown key 'bogus'");
    expect_error("weather.set_water_ice{color = 1}", "weather.set_water_ice: unknown key 'color'");
    expect_error("weather.set_water_ice{calm = 'flat'}", "weather.set_water_ice: 'calm' is not a number");
    expect_error("weather.set_water_ice{strength = 0/0}", "weather.set_water_ice: 'strength' is not a number");
    expect_error("weather.set_water_ice{enabled = true}", "weather.set_water_ice: 'enabled' is not a number");
    expect_error("weather.set_water_ice{ice_color = 1}", "weather.set_water_ice: 'ice_color' is not a table of three numbers");
    expect_error("weather.set_water_ice{ice_color = {1, 2}}", "weather.set_water_ice: 'ice_color' is not a table of three numbers");
    expect_error("weather.set_water_ice{ice_color = {1, 'x', 3}}", "weather.set_water_ice: 'ice_color' is not a table of three numbers");
    expect_error("weather.set_water_ice{[1] = 0.5}", "weather.set_water_ice: the keys are names");
    expect_error("weather.set_water_ice(0.5)", "table expected");
    expect_error("weather.set_water_ice()", "table expected");
    const kit::WaterIce def;
    for (const char* body : {"weather.set_water_ice{manual = 0.9, strength = 'x'}", "weather.set_water_ice{strength = 'x', manual = 0.9}",
                             "weather.set_water_ice{manual = 0.9, nope = 1}", "weather.set_water_ice{enabled = 0, calm = 'x'}",
                             "weather.set_water_ice{ice_color = {0, 0, 0}, ice_depth_m = 'x'}",
                             "weather.set_water_ice{ice_color = {1, 2}, manual = 0.9, enabled = 0}"}) {
        const std::string src = std::string("function update()\n    if mce.tick() == 1 then ") + body + " end\nend\n";
        IceRig r(src.c_str());
        r.step(2);
        INFO(body);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
        CHECK(r.ice().manual == def.manual);
        CHECK(r.ice().strength == def.strength);
        CHECK(r.ice().calm == def.calm);
        CHECK(r.ice().enabled == def.enabled);
        CHECK(r.ice().ice_depth_m == def.ice_depth_m);
        CHECK(r.ice().ice_color.x == def.ice_color.x);
        CHECK(r.ice().ice_color.z == def.ice_color.z);
    }
}
