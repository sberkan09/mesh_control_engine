// M-COLDBREATH step 34c: the cold breath functions of the `weather` Luau table over the first kit::ColdBreath. Getters,
// clamping, cold_breath_active against the kit helper over the season, the snow cover, the wind and the weather kind, named
// errors, nothing written on error, and a missing ColdBreath raises (the setter never adds one).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/cold_breath.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/season.h"
#include "mce/kit/snow_cover.h"
#include "mce/kit/weather.h"
#include "mce/kit/wind_gust.h"
#include "mce/kit/world_clock.h"
#include "mce/script/compiler.h"

using namespace mce;
using script::ScriptHost;
using script::ScriptState;

namespace {

constexpr u32 kSnowMax = 1u << 24;  // the numerator range of SnowCover.snow_q (cover = snow_q / 2^24)

struct BreathRig {
    World w;
    ScriptHost host;
    u64 tick = 0;
    Entity cb_e, probe, weather_e, season_e, clock_e, gust_e, snow_e;

    explicit BreathRig(const char* source, bool with_breath = true, bool install = true) {
        w.register_components<LocalTransform>();
        if (install) kit::install_weather(w, nullptr);
        INFO(source);
        Result<Array<u8>> bc = script::compile_unchecked("a", source);
        REQUIRE(bc);
        REQUIRE(host.add_bytecode("a", bc.value().span()));
        host.install(w);
        probe = w.spawn_with(LocalTransform{});
        if (with_breath) cb_e = w.spawn_with(kit::ColdBreath{});
    }
    kit::ColdBreath& cb() { return *w.try_get_mut<kit::ColdBreath>(cb_e); }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
    const LocalTransform& p() { return *w.try_get<LocalTransform>(probe); }
    const ScriptHost& h() const { return host; }
};

std::string err(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

void expect_error(const char* body, const char* needle, bool with_breath = true, bool install = true) {
    const std::string src = std::string("function update()\n    if mce.tick() == 1 then\n        ") + body + "\n    end\nend\n";
    BreathRig r(src.c_str(), with_breath, install);
    r.step(2);
    INFO(body);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    INFO(err(r.h()));
    CHECK(err(r.h()).find(needle) != std::string::npos);
}

const char* const kProbeSource = R"(
function update()
    mce.each("LocalTransform", function(e)
        mce.set(e, "LocalTransform", "position", vector.create(weather.cold_breath_active(), 0, 0))
    end)
end
)";

// The inputs of the extract's cold breath block, recomputed here from the world state. The snow cover system moves the cover
// by at most about 1e-6 per tick (build and melt seconds are set to their maximum by the sweep), so the comparison below
// is a tolerance, not bit equality.
f32 expected_amount(BreathRig& r) {
    const kit::Season* season = nullptr;
    r.w.query<const kit::Season&>().each([&](const kit::Season& c) {
        if (!season) season = &c;
    });
    u32 day = 0;
    f32 hour = 0.0f;
    bool have_clock = false;
    r.w.query<const kit::WorldClock&>().each([&](const kit::WorldClock& c) {
        if (have_clock) return;
        have_clock = true;
        day = c.day;
        hour = c.hour;
    });
    const f32 phase = season ? kit::season_phase(*season, day, hour) : 0.0f;
    const kit::SnowCover* sc = nullptr;
    r.w.query<const kit::SnowCover&>().each([&](const kit::SnowCover& c) {
        if (!sc) sc = &c;
    });
    const f32 cover = sc ? std::clamp(sc->cover * sc->strength, 0.0f, 1.0f) : 0.0f;
    const kit::Weather* wx = nullptr;
    r.w.query<const kit::Weather&>().each([&](const kit::Weather& c) {
        if (!wx) wx = &c;
    });
    const kit::WindGust* gust = nullptr;
    r.w.query<const kit::WindGust&>().each([&](const kit::WindGust& c) {
        if (!gust) gust = &c;
    });
    const f32 wind = wx ? kit::effective_wind(*wx, gust, kit::kGustSway) : 0.0f;
    return kit::cold_breath_amount(r.cb(), season, phase, cover, wind);
}

}  // namespace

TEST_CASE("cold breath api: the getters read the first ColdBreath, the setter writes and clamps to the Range") {
    BreathRig r(R"(
function update()
    local t = mce.tick()
    local function near(a, b) return math.abs(a - b) < 1e-6 end
    if t == 1 then
        assert(weather.cold_breath_enabled() == 1 and weather.cold_breath_manual() == 0 and weather.cold_breath_strength() == 1)
        assert(near(weather.cold_breath_window_full(), 0.08) and near(weather.cold_breath_window_end(), 0.16))
        assert(near(weather.cold_breath_cover_low(), 0.15) and near(weather.cold_breath_cover_full(), 0.6))
        assert(near(weather.cold_breath_wind_calm(), 0.5) and near(weather.cold_breath_wind_still(), 0.95))
        assert(near(weather.cold_breath_rate(), 0.8) and near(weather.cold_breath_mouth_forward_m(), 0.35))
        assert(near(weather.cold_breath_mouth_down_m(), 0.25) and near(weather.cold_breath_size_m(), 0.12))
        assert(near(weather.cold_breath_grow(), 2.5) and near(weather.cold_breath_lifetime_s(), 1.2))
        assert(near(weather.cold_breath_speed(), 0.35) and near(weather.cold_breath_lift(), 0.15))
        assert(weather.cold_breath_max_particles() == 24 and weather.cold_breath_seed() == 1)
        local r, g, b = weather.cold_breath_color()
        assert(near(r, 0.9) and near(g, 0.93) and near(b, 0.97))
        weather.set_cold_breath{window_full = 0.1, window_end = 0.3, manual = 0.5, cover_low = 0.25, cover_full = 0.75,
                                wind_calm = 0.25, wind_still = 0.75, rate = 10, mouth_forward_m = 2, mouth_down_m = 1,
                                size_m = 1, grow = 8, lifetime_s = 5, speed = 3, lift = 2, max_particles = 128, seed = 77,
                                strength = 0.75, color = {1, 0.5, 0.25}}
    end
    if t == 2 then
        assert(near(weather.cold_breath_window_full(), 0.1) and near(weather.cold_breath_window_end(), 0.3))
        assert(weather.cold_breath_manual() == 0.5 and weather.cold_breath_cover_low() == 0.25 and weather.cold_breath_cover_full() == 0.75)
        assert(weather.cold_breath_wind_calm() == 0.25 and weather.cold_breath_wind_still() == 0.75)
        assert(weather.cold_breath_rate() == 10 and weather.cold_breath_mouth_forward_m() == 2 and weather.cold_breath_mouth_down_m() == 1)
        assert(weather.cold_breath_size_m() == 1 and weather.cold_breath_grow() == 8 and weather.cold_breath_lifetime_s() == 5)
        assert(weather.cold_breath_speed() == 3 and weather.cold_breath_lift() == 2)
        assert(weather.cold_breath_max_particles() == 128 and weather.cold_breath_seed() == 77 and weather.cold_breath_strength() == 0.75)
        local r, g, b = weather.cold_breath_color()
        assert(r == 1 and g == 0.5 and b == 0.25)
        weather.set_cold_breath{enabled = 0}
    end
    if t == 3 then
        assert(weather.cold_breath_enabled() == 0 and weather.cold_breath_rate() == 10)  -- only the given keys changed
        weather.set_cold_breath{enabled = 7, window_full = 1e9, window_end = 1e9, manual = 1e9, cover_low = 1e9, cover_full = 1e9,
                                wind_calm = 1e9, wind_still = 1e9, rate = 1e9, mouth_forward_m = 1e9, mouth_down_m = 1e9,
                                size_m = 1e9, grow = 1e9, lifetime_s = 1e9, speed = 1e9, lift = 1e9, max_particles = 1e9,
                                seed = 1e12, strength = 1e9, color = {1e9, -1, 0.5}}
    end
    if t == 4 then
        assert(weather.cold_breath_enabled() == 1 and weather.cold_breath_window_full() == 0.5 and weather.cold_breath_window_end() == 0.5)
        assert(weather.cold_breath_manual() == 1 and weather.cold_breath_cover_low() == 1 and weather.cold_breath_cover_full() == 1)
        assert(weather.cold_breath_wind_calm() == 1 and weather.cold_breath_wind_still() == 1)
        assert(weather.cold_breath_rate() == 10 and weather.cold_breath_mouth_forward_m() == 2 and weather.cold_breath_mouth_down_m() == 2)
        assert(weather.cold_breath_size_m() == 1 and weather.cold_breath_grow() == 8 and weather.cold_breath_lifetime_s() == 5)
        assert(weather.cold_breath_speed() == 3 and weather.cold_breath_lift() == 2)
        assert(weather.cold_breath_max_particles() == 128 and weather.cold_breath_seed() == 4294967295 and weather.cold_breath_strength() == 1)
        local r, g, b = weather.cold_breath_color()
        assert(r == 1 and g == 0 and b == 0.5)
        weather.set_cold_breath{enabled = 0, window_full = -1, window_end = -1, manual = -1, cover_low = -1, cover_full = -1,
                                wind_calm = -1, wind_still = -1, rate = -1, mouth_forward_m = -1, mouth_down_m = -1,
                                size_m = -1, grow = -1, lifetime_s = -1, speed = -1, lift = -1, max_particles = -1,
                                seed = -1, strength = -1}
        weather.set_cold_breath{}  -- nothing given, nothing changes
    end
    if t == 5 then
        assert(weather.cold_breath_enabled() == 0 and weather.cold_breath_window_full() == 0 and weather.cold_breath_window_end() == 0)
        assert(weather.cold_breath_manual() == 0 and weather.cold_breath_cover_low() == 0 and weather.cold_breath_cover_full() == 0)
        assert(weather.cold_breath_wind_calm() == 0 and weather.cold_breath_wind_still() == 0)
        assert(weather.cold_breath_rate() == 0 and weather.cold_breath_mouth_forward_m() == 0 and weather.cold_breath_mouth_down_m() == 0)
        assert(near(weather.cold_breath_size_m(), 0.01) and weather.cold_breath_grow() == 1 and near(weather.cold_breath_lifetime_s(), 0.3))
        assert(weather.cold_breath_speed() == 0 and weather.cold_breath_lift() == 0)
        assert(weather.cold_breath_max_particles() == 4 and weather.cold_breath_seed() == 0 and weather.cold_breath_strength() == 0)
    end
end
)");
    r.step(6);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.cb().strength == 0.0f);
    CHECK(r.cb().enabled == 0);
    CHECK(r.cb().pad_[0] == 0);
    CHECK(r.cb().pad_[1] == 0);
    CHECK(r.cb().pad_[2] == 0);
}

TEST_CASE("cold breath api: cold_breath_active is the kit helper over the season, the snow cover, the wind and the weather kind") {
    BreathRig r(kProbeSource);
    r.season_e = r.w.spawn_with(kit::Season{});
    kit::WorldClock clock;
    clock.paused = 1;
    clock.drive_environment = 0;
    r.clock_e = r.w.spawn_with(clock);
    r.weather_e = r.w.spawn_with(kit::Weather{});
    r.gust_e = r.w.spawn_with(kit::WindGust{});
    kit::SnowCover snow;
    snow.build_seconds = 86400.0f;  // the cover barely moves during the sweep
    snow.melt_seconds = 86400.0f;
    r.snow_e = r.w.spawn_with(snow);
    u32 distinct = 0, zero = 0, nonzero = 0, full = 0;
    f32 last = -1.0f;
    for (f32 start : {0.1f, 0.375f, 0.8f, 0.875f}) {
        r.w.try_get_mut<kit::Season>(r.season_e)->start_phase = start;
        for (f32 cover : {0.0f, 0.3f, 0.9f}) {
            for (f32 manual : {0.0f, 0.3f}) {
                r.cb().manual = manual;
                for (kit::WeatherKind kind : {kit::WeatherKind::Clear, kit::WeatherKind::Rain, kit::WeatherKind::Storm, kit::WeatherKind::Snow}) {
                    for (f32 wind : {0.1f, 0.6f, 0.99f}) {
                        r.w.try_get_mut<kit::SnowCover>(r.snow_e)->snow_q = static_cast<u32>(cover * static_cast<f32>(kSnowMax));
                        kit::Weather& wx = *r.w.try_get_mut<kit::Weather>(r.weather_e);
                        wx.current = wx.target = kind;
                        wx.intensity = 0.5f;
                        wx.wind = wind;
                        r.step(2);
                        REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
                        const f32 expected = expected_amount(r);
                        INFO("start " << start << " cover " << cover << " manual " << manual << " kind " << int(kind) << " wind " << wind);
                        CHECK(std::fabs(r.p().position.x - expected) < 1e-5f);
                        CHECK(r.p().position.x >= 0.0f);
                        CHECK(r.p().position.x <= 1.0f);
                        if (r.p().position.x > 0.0f) ++nonzero; else ++zero;
                        if (r.p().position.x > 0.99f) ++full;
                        if (std::fabs(r.p().position.x - last) > 1e-4f) ++distinct;
                        last = r.p().position.x;
                    }
                }
            }
        }
    }
    CHECK(distinct > 8);
    CHECK(nonzero > 20);
    CHECK(zero > 5);
    CHECK(full > 0);
    // the season window alone (winter centre, no cover, calm): full strength
    r.w.try_get_mut<kit::Season>(r.season_e)->start_phase = 0.875f;
    r.w.try_get_mut<kit::SnowCover>(r.snow_e)->snow_q = 0;
    r.w.try_get_mut<kit::Weather>(r.weather_e)->current = r.w.try_get_mut<kit::Weather>(r.weather_e)->target = kit::WeatherKind::Clear;
    r.w.try_get_mut<kit::Weather>(r.weather_e)->wind = 0.0f;
    r.cb().manual = 0.0f;
    r.step(2);
    CHECK(r.p().position.x > 0.99f);
    // enabled 0 and strength 0 are exactly 0
    r.cb() = kit::ColdBreath{};
    r.cb().manual = 0.5f;
    r.cb().enabled = 0;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
    r.cb().enabled = 1;
    r.cb().strength = 0.0f;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
}

TEST_CASE("cold breath api: with no Season, no SnowCover or no Weather only the remaining inputs count") {
    BreathRig q(kProbeSource);  // no Weather entity: calm; no Season: no window; no SnowCover: cover 0
    q.step(2);
    REQUIRE(q.h().scripts()[0].state == ScriptState::Running);
    CHECK(q.p().position.x == 0.0f);
    q.cb().manual = 0.625f;
    q.step(1);
    CHECK(q.p().position.x == 0.625f);  // calm: the floor passes whole
    CHECK(q.p().position.x == expected_amount(q));
    // a Season in the winter window alone brings the breath
    kit::Season season;
    season.start_phase = 0.875f;
    q.season_e = q.w.spawn_with(season);
    q.cb().manual = 0.0f;
    q.step(1);
    CHECK(q.p().position.x > 0.99f);
    CHECK(q.p().position.x == expected_amount(q));
    // out of the window and bare: nothing; a settled SnowCover brings it back (cover * strength is what counts)
    q.w.try_get_mut<kit::Season>(q.season_e)->start_phase = 0.375f;
    q.step(1);
    CHECK(q.p().position.x == 0.0f);
    kit::SnowCover snow;
    snow.snow_q = kSnowMax;
    snow.melt_seconds = 86400.0f;
    snow.strength = 0.0f;
    q.snow_e = q.w.spawn_with(snow);
    q.step(1);
    CHECK(q.p().position.x == 0.0f);  // strength 0 scales the cover away
    q.w.try_get_mut<kit::SnowCover>(q.snow_e)->strength = 1.0f;
    q.step(1);
    CHECK(q.p().position.x > 0.99f);
    CHECK(std::fabs(q.p().position.x - expected_amount(q)) < 1e-5f);
}

TEST_CASE("cold breath api: a world without a ColdBreath raises a named error and the setter never adds one") {
    for (const char* name : {"cold_breath_enabled", "cold_breath_window_full", "cold_breath_window_end", "cold_breath_manual",
                             "cold_breath_cover_low", "cold_breath_cover_full", "cold_breath_wind_calm", "cold_breath_wind_still",
                             "cold_breath_rate", "cold_breath_mouth_forward_m", "cold_breath_mouth_down_m", "cold_breath_size_m",
                             "cold_breath_grow", "cold_breath_lifetime_s", "cold_breath_speed", "cold_breath_lift",
                             "cold_breath_color", "cold_breath_max_particles", "cold_breath_seed", "cold_breath_strength",
                             "cold_breath_active"})
        expect_error((std::string("weather.") + name + "()").c_str(),
                     (std::string("weather.") + name + ": the scene has no ColdBreath entity").c_str(), false);
    expect_error("weather.set_cold_breath{}", "weather.set_cold_breath: the scene has no ColdBreath entity", false);
    expect_error("weather.set_cold_breath{strength = 0}", "weather.set_cold_breath: the scene has no ColdBreath entity", false);
    expect_error("weather.cold_breath_active()", "weather.cold_breath_active: this world has no weather", false, false);
    expect_error("weather.set_cold_breath{}", "weather.set_cold_breath: this world has no weather", false, false);
    BreathRig r("function update()\n    if mce.tick() == 1 then weather.set_cold_breath{enabled = 0} end\nend\n", false);
    r.step(2);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    usize n = 0;
    r.w.query<kit::ColdBreath>().each([&](Entity, kit::ColdBreath&) { ++n; });
    CHECK(n == 0);
}

TEST_CASE("cold breath api: bad arguments raise an error that names the call and write nothing") {
    expect_error("weather.set_cold_breath{bogus = 1}", "weather.set_cold_breath: unknown key 'bogus'");
    expect_error("weather.set_cold_breath{twilight_low = 1}", "weather.set_cold_breath: unknown key 'twilight_low'");
    expect_error("weather.set_cold_breath{rate = 'fast'}", "weather.set_cold_breath: 'rate' is not a number");
    expect_error("weather.set_cold_breath{strength = 0/0}", "weather.set_cold_breath: 'strength' is not a number");
    expect_error("weather.set_cold_breath{enabled = true}", "weather.set_cold_breath: 'enabled' is not a number");
    expect_error("weather.set_cold_breath{cover_low = 'x'}", "weather.set_cold_breath: 'cover_low' is not a number");
    expect_error("weather.set_cold_breath{grow = 'x'}", "weather.set_cold_breath: 'grow' is not a number");
    expect_error("weather.set_cold_breath{lift = 'x'}", "weather.set_cold_breath: 'lift' is not a number");
    expect_error("weather.set_cold_breath{seed = 'x'}", "weather.set_cold_breath: 'seed' is not a number");
    expect_error("weather.set_cold_breath{color = 1}", "weather.set_cold_breath: 'color' is not a table of three numbers");
    expect_error("weather.set_cold_breath{color = {1, 2}}", "weather.set_cold_breath: 'color' is not a table of three numbers");
    expect_error("weather.set_cold_breath{color = {1, 'x', 3}}", "weather.set_cold_breath: 'color' is not a table of three numbers");
    expect_error("weather.set_cold_breath{[1] = 0.5}", "weather.set_cold_breath: the keys are names");
    expect_error("weather.set_cold_breath(0.5)", "table expected");
    expect_error("weather.set_cold_breath()", "table expected");
    const kit::ColdBreath def;
    for (const char* body : {"weather.set_cold_breath{manual = 0.9, strength = 'x'}", "weather.set_cold_breath{strength = 'x', manual = 0.9}",
                             "weather.set_cold_breath{manual = 0.9, nope = 1}", "weather.set_cold_breath{enabled = 0, rate = 'x'}",
                             "weather.set_cold_breath{color = {0, 0, 0}, lift = 'x'}",
                             "weather.set_cold_breath{color = {1, 2}, manual = 0.9, enabled = 0}"}) {
        const std::string src = std::string("function update()\n    if mce.tick() == 1 then ") + body + " end\nend\n";
        BreathRig r(src.c_str());
        r.step(2);
        INFO(body);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
        CHECK(r.cb().manual == def.manual);
        CHECK(r.cb().strength == def.strength);
        CHECK(r.cb().rate == def.rate);
        CHECK(r.cb().enabled == def.enabled);
        CHECK(r.cb().lift == def.lift);
        CHECK(r.cb().color.x == def.color.x);
        CHECK(r.cb().color.z == def.color.z);
    }
}
