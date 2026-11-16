// M-POLLEN step 24c: the pollen functions of the `weather` Luau table over the first kit::Pollen. Getters, clamping,
// pollen_drifting against the kit helper over days, hours, nights and rain, named errors, nothing written on error, and a
// missing Pollen raises (the setter never adds one).

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/pollen.h"
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

struct PollenRig {
    World w;
    ScriptHost host;
    u64 tick = 0;
    Entity po_e, probe, season_e, clock_e, weather_e;

    explicit PollenRig(const char* source, bool with_pollen = true, bool install = true) {
        w.register_components<LocalTransform>();
        if (install) kit::install_weather(w, nullptr);
        INFO(source);
        Result<Array<u8>> bc = script::compile_unchecked("a", source);
        REQUIRE(bc);
        REQUIRE(host.add_bytecode("a", bc.value().span()));
        host.install(w);
        probe = w.spawn_with(LocalTransform{});
        if (with_pollen) po_e = w.spawn_with(kit::Pollen{});
    }
    kit::Pollen& po() { return *w.try_get_mut<kit::Pollen>(po_e); }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
    const LocalTransform& p() { return *w.try_get<LocalTransform>(probe); }
    const ScriptHost& h() const { return host; }
};

std::string err(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

void expect_error(const char* body, const char* needle, bool with_pollen = true, bool install = true) {
    const std::string src = std::string("function update()\n    if mce.tick() == 1 then\n        ") + body + "\n    end\nend\n";
    PollenRig r(src.c_str(), with_pollen, install);
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
        mce.set(e, "LocalTransform", "position", vector.create(weather.pollen_drifting(), 0, 0))
    end)
end
)";

}  // namespace

TEST_CASE("pollen api: the getters read the first Pollen, the setter writes and clamps to the Range") {
    PollenRig r(R"(
function update()
    local t = mce.tick()
    local function near(a, b) return math.abs(a - b) < 1e-6 end
    if t == 1 then
        assert(weather.pollen_enabled() == 1 and weather.pollen_manual() == 0 and weather.pollen_strength() == 1)
        assert(near(weather.pollen_spring_start(), 0.95) and near(weather.pollen_spring_full(), 0.02))
        assert(near(weather.pollen_spring_end_full(), 0.22) and near(weather.pollen_spring_end(), 0.32))
        assert(weather.pollen_day_only() == 1 and near(weather.pollen_min_daylight(), 0.5))
        assert(weather.pollen_rate() == 14 and weather.pollen_box_xz_m() == 18 and weather.pollen_height_m() == 5)
        assert(near(weather.pollen_size_m(), 0.06) and weather.pollen_lifetime_s() == 12 and near(weather.pollen_fall_speed(), 0.08))
        assert(near(weather.pollen_wind_follow(), 0.6))
        assert(weather.pollen_max_particles() == 512 and weather.pollen_seed() == 1)
        local r, g, b = weather.pollen_color()
        assert(near(r, 1.0) and near(g, 0.95) and near(b, 0.7))
        weather.set_pollen{spring_start = 0.25, spring_full = 0.5, spring_end_full = 0.75, spring_end = 0.875, manual = 0.5,
                            day_only = 0, min_daylight = 0.25, rate = 100, box_xz_m = 30, height_m = 12, size_m = 0.5,
                            lifetime_s = 10, fall_speed = 2, wind_follow = 1.5, max_particles = 2048, seed = 77, strength = 0.75,
                            color = {1, 0.5, 0.25}}
    end
    if t == 2 then
        assert(weather.pollen_spring_start() == 0.25 and weather.pollen_spring_full() == 0.5)
        assert(weather.pollen_spring_end_full() == 0.75 and weather.pollen_spring_end() == 0.875 and weather.pollen_manual() == 0.5)
        assert(weather.pollen_day_only() == 0 and weather.pollen_min_daylight() == 0.25)
        assert(weather.pollen_rate() == 100 and weather.pollen_box_xz_m() == 30 and weather.pollen_height_m() == 12)
        assert(weather.pollen_size_m() == 0.5 and weather.pollen_lifetime_s() == 10 and weather.pollen_fall_speed() == 2 and weather.pollen_wind_follow() == 1.5)
        assert(weather.pollen_max_particles() == 2048 and weather.pollen_seed() == 77 and weather.pollen_strength() == 0.75)
        local r, g, b = weather.pollen_color()
        assert(r == 1 and g == 0.5 and b == 0.25)
        weather.set_pollen{enabled = 0}
    end
    if t == 3 then
        assert(weather.pollen_enabled() == 0 and weather.pollen_rate() == 100)  -- only the given keys changed
        weather.set_pollen{enabled = 7, spring_start = 1e9, spring_full = 1e9, spring_end_full = 1e9, spring_end = 1e9,
                            manual = 1e9, day_only = -3, min_daylight = 1e9, rate = 1e9, box_xz_m = 1e9, height_m = 1e9,
                            size_m = 1e9, lifetime_s = 1e9, fall_speed = 1e9, wind_follow = 1e9, max_particles = 1e9, seed = 1e12,
                            strength = 1e9, color = {1e9, -1, 0.5}}
    end
    if t == 4 then
        assert(weather.pollen_enabled() == 1 and weather.pollen_spring_start() == 1 and weather.pollen_spring_full() == 1)
        assert(weather.pollen_spring_end_full() == 1 and weather.pollen_spring_end() == 1 and weather.pollen_manual() == 1)
        assert(weather.pollen_day_only() == 1 and weather.pollen_min_daylight() == 1)
        assert(weather.pollen_rate() == 200 and weather.pollen_box_xz_m() == 100 and weather.pollen_height_m() == 30)
        assert(weather.pollen_size_m() == 0.5 and weather.pollen_lifetime_s() == 30 and weather.pollen_fall_speed() == 2 and weather.pollen_wind_follow() == 2)
        assert(weather.pollen_max_particles() == 4096 and weather.pollen_seed() == 4294967295 and weather.pollen_strength() == 1)
        local r, g, b = weather.pollen_color()
        assert(r == 1 and g == 0 and b == 0.5)
        weather.set_pollen{enabled = 0, spring_start = -1, spring_full = -1, spring_end_full = -1, spring_end = -1,
                            manual = -1, day_only = 0, min_daylight = -1, rate = -1, box_xz_m = -1, height_m = -1,
                            size_m = -1, lifetime_s = -1, fall_speed = -1, wind_follow = -1, max_particles = -1, seed = -1, strength = -1}
        weather.set_pollen{}  -- nothing given, nothing changes
    end
    if t == 5 then
        assert(weather.pollen_enabled() == 0 and weather.pollen_spring_start() == 0 and weather.pollen_spring_full() == 0)
        assert(weather.pollen_spring_end_full() == 0 and weather.pollen_spring_end() == 0 and weather.pollen_manual() == 0)
        assert(weather.pollen_day_only() == 0 and weather.pollen_min_daylight() == 0)
        assert(weather.pollen_rate() == 0 and weather.pollen_box_xz_m() == 1 and weather.pollen_height_m() == 1)
        assert(near(weather.pollen_size_m(), 0.02) and weather.pollen_lifetime_s() == 1 and weather.pollen_fall_speed() == 0 and weather.pollen_wind_follow() == 0)
        assert(weather.pollen_max_particles() == 16 and weather.pollen_seed() == 0 and weather.pollen_strength() == 0)
    end
end
)");
    r.step(6);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.po().strength == 0.0f);
    CHECK(r.po().enabled == 0);
    CHECK(r.po().day_only == 0);
    CHECK(r.po().pad_[0] == 0);
    CHECK(r.po().pad_[1] == 0);
}

TEST_CASE("pollen api: pollen_drifting is the kit helper over days, hours, nights and rain") {
    PollenRig r(kProbeSource);
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
            for (u8 day_only : {u8{1}, u8{0}}) {
                r.w.try_get_mut<kit::Season>(r.season_e)->year_days = year_days;
                r.po().manual = manual;
                r.po().day_only = day_only;
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
                            const f32 expected = kit::pollen_amount(r.po(), &s, kit::season_phase(s, after.day, after.hour),
                                                                     night_fade_of(r.w.try_resource<kit::Environment3D>()->time_of_day),
                                                                     raining ? wx.intensity : 0.0f);
                            INFO("year_days " << year_days << " manual " << manual << " day_only " << int(day_only) << " day "
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
    // enabled 0 and strength 0 are exactly 0, the Season disabled is the manual floor (by day, dry)
    r.w.try_resource<kit::Environment3D>()->time_of_day = 12.0f;
    r.w.try_get_mut<kit::Weather>(r.weather_e)->current = kit::WeatherKind::Clear;
    r.w.try_get_mut<kit::Weather>(r.weather_e)->target = kit::WeatherKind::Clear;
    r.w.try_get_mut<kit::Weather>(r.weather_e)->intensity = 0.0f;
    r.po() = kit::Pollen{};
    r.po().manual = 0.3f;
    r.po().enabled = 0;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
    r.po().enabled = 1;
    r.po().strength = 0.0f;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
    r.po().strength = 1.0f;
    r.w.try_get_mut<kit::Season>(r.season_e)->enabled = 0;
    r.step(1);
    CHECK(bits(r.p().position.x) == bits(0.3f));
    // by night, day_only 1 shows nothing even with the manual floor
    r.w.try_resource<kit::Environment3D>()->time_of_day = 0.0f;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
}

TEST_CASE("pollen api: with no clock the night fade is 0 and the phase the fixed start_phase; with no Season the amount is the manual floor") {
    PollenRig r(kProbeSource);
    // no Season, no clock, no Environment3D: the manual floor only (night fade 0 is full daylight), never nil
    r.step(2);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.p().position.x == 0.0f);
    r.po().manual = 0.625f;
    r.step(1);
    CHECK(r.p().position.x == 0.625f);
    // a negative time_of_day is no clock either
    r.w.set_resource(kit::Environment3D{});
    r.w.try_resource<kit::Environment3D>()->time_of_day = -1.0f;
    r.step(1);
    CHECK(r.p().position.x == 0.625f);
    // a clock at midnight: day_only 1 gates the manual floor to 0, day_only 0 shows it
    r.w.try_resource<kit::Environment3D>()->time_of_day = 0.0f;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
    r.po().day_only = 0;
    r.step(1);
    CHECK(bits(r.p().position.x) == bits(kit::pollen_amount(r.po(), nullptr, 0.0f, night_fade_of(0.0f), 0.0f)));
    CHECK(r.p().position.x == 0.625f);
    // a Season and no clock (day 0, hour 0)
    r.season_e = r.w.spawn_with(kit::Season{});
    r.po().manual = 0.0f;
    bool saw_positive = false;
    for (f32 start : {0.0f, 0.05f, 0.125f, 0.2f, 0.5f, 0.95f}) {
        r.w.try_get_mut<kit::Season>(r.season_e)->start_phase = start;
        r.step(2);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
        const kit::Season& s = *r.w.try_get<kit::Season>(r.season_e);
        INFO("start_phase " << start);
        CHECK(bits(r.p().position.x) == bits(kit::pollen_amount(r.po(), &s, kit::season_phase(s, 0, 0.0f), night_fade_of(0.0f), 0.0f)));
        saw_positive = saw_positive || r.p().position.x > 0.0f;
    }
    CHECK(saw_positive);
}

TEST_CASE("pollen api: a world without a Pollen raises a named error and the setter never adds one") {
    for (const char* name : {"pollen_enabled", "pollen_spring_start", "pollen_spring_full", "pollen_spring_end_full",
                             "pollen_spring_end", "pollen_manual", "pollen_day_only", "pollen_min_daylight", "pollen_rate",
                             "pollen_box_xz_m", "pollen_height_m", "pollen_size_m", "pollen_lifetime_s", "pollen_fall_speed", "pollen_wind_follow",
                             "pollen_color", "pollen_max_particles", "pollen_seed", "pollen_strength"})
        expect_error((std::string("weather.") + name + "()").c_str(),
                     (std::string("weather.") + name + ": the scene has no Pollen entity").c_str(), false);
    expect_error("weather.pollen_drifting()", "weather.pollen_drifting: the scene has no Pollen entity", false);
    expect_error("weather.set_pollen{}", "weather.set_pollen: the scene has no Pollen entity", false);
    expect_error("weather.set_pollen{strength = 0}", "weather.set_pollen: the scene has no Pollen entity", false);
    expect_error("weather.pollen_drifting()", "weather.pollen_drifting: this world has no weather", false, false);
    expect_error("weather.set_pollen{}", "weather.set_pollen: this world has no weather", false, false);
    PollenRig r("function update()\n    if mce.tick() == 1 then weather.set_pollen{enabled = 0} end\nend\n", false);
    r.step(2);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    usize n = 0;
    r.w.query<kit::Pollen>().each([&](Entity, kit::Pollen&) { ++n; });
    CHECK(n == 0);
}

TEST_CASE("pollen api: bad arguments raise an error that names the call and write nothing") {
    expect_error("weather.set_pollen{bogus = 1}", "weather.set_pollen: unknown key 'bogus'");
    expect_error("weather.set_pollen{leaf_color = 1}", "weather.set_pollen: unknown key 'leaf_color'");
    expect_error("weather.set_pollen{rate = 'fast'}", "weather.set_pollen: 'rate' is not a number");
    expect_error("weather.set_pollen{strength = 0/0}", "weather.set_pollen: 'strength' is not a number");
    expect_error("weather.set_pollen{enabled = true}", "weather.set_pollen: 'enabled' is not a number");
    expect_error("weather.set_pollen{day_only = 'x'}", "weather.set_pollen: 'day_only' is not a number");
    expect_error("weather.set_pollen{seed = 'x'}", "weather.set_pollen: 'seed' is not a number");
    expect_error("weather.set_pollen{color = 1}", "weather.set_pollen: 'color' is not a table of three numbers");
    expect_error("weather.set_pollen{color = {1, 2}}", "weather.set_pollen: 'color' is not a table of three numbers");
    expect_error("weather.set_pollen{color = {1, 'x', 3}}", "weather.set_pollen: 'color' is not a table of three numbers");
    expect_error("weather.set_pollen{[1] = 0.5}", "weather.set_pollen: the keys are names");
    expect_error("weather.set_pollen(0.5)", "table expected");
    expect_error("weather.set_pollen()", "table expected");
    const kit::Pollen def;
    for (const char* body : {"weather.set_pollen{manual = 0.9, strength = 'x'}", "weather.set_pollen{strength = 'x', manual = 0.9}",
                             "weather.set_pollen{manual = 0.9, nope = 1}", "weather.set_pollen{enabled = 0, rate = 'x'}",
                             "weather.set_pollen{color = {0, 0, 0}, height_m = 'x'}",
                             "weather.set_pollen{color = {1, 2}, manual = 0.9, enabled = 0}"}) {
        const std::string src = std::string("function update()\n    if mce.tick() == 1 then ") + body + " end\nend\n";
        PollenRig r(src.c_str());
        r.step(2);
        INFO(body);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
        CHECK(r.po().manual == def.manual);
        CHECK(r.po().strength == def.strength);
        CHECK(r.po().rate == def.rate);
        CHECK(r.po().enabled == def.enabled);
        CHECK(r.po().day_only == def.day_only);
        CHECK(r.po().height_m == def.height_m);
        CHECK(r.po().color.x == def.color.x);
        CHECK(r.po().color.z == def.color.z);
    }
}
