// M-MIDGES step 33c: the midge functions of the `weather` Luau table over the first kit::Midge. Getters, clamping,
// midge_active against the kit helper over the season, the clock, the sun, the wind and the rain, named errors, nothing
// written on error, and a missing Midge raises (the setter never adds one).

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/midge.h"
#include "mce/kit/season.h"
#include "mce/kit/weather.h"
#include "mce/kit/wind_gust.h"
#include "mce/kit/world_clock.h"
#include "mce/render/atmosphere.h"
#include "mce/script/compiler.h"

using namespace mce;
using script::ScriptHost;
using script::ScriptState;

namespace {

struct MidgeRig {
    World w;
    ScriptHost host;
    u64 tick = 0;
    Entity mg_e, probe, weather_e, season_e, clock_e, gust_e;

    explicit MidgeRig(const char* source, bool with_midge = true, bool install = true) {
        w.register_components<LocalTransform>();
        if (install) kit::install_weather(w, nullptr);
        INFO(source);
        Result<Array<u8>> bc = script::compile_unchecked("a", source);
        REQUIRE(bc);
        REQUIRE(host.add_bytecode("a", bc.value().span()));
        host.install(w);
        probe = w.spawn_with(LocalTransform{});
        if (with_midge) mg_e = w.spawn_with(kit::Midge{});
    }
    kit::Midge& mg() { return *w.try_get_mut<kit::Midge>(mg_e); }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
    const LocalTransform& p() { return *w.try_get<LocalTransform>(probe); }
    const ScriptHost& h() const { return host; }
};

std::string err(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

void expect_error(const char* body, const char* needle, bool with_midge = true, bool install = true) {
    const std::string src = std::string("function update()\n    if mce.tick() == 1 then\n        ") + body + "\n    end\nend\n";
    MidgeRig r(src.c_str(), with_midge, install);
    r.step(2);
    INFO(body);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    INFO(err(r.h()));
    CHECK(err(r.h()).find(needle) != std::string::npos);
}

u32 bits(f32 v) { return std::bit_cast<u32>(v); }

const char* const kProbeSource = R"(
function update()
    mce.each("LocalTransform", function(e)
        mce.set(e, "LocalTransform", "position", vector.create(weather.midge_active(), 0, 0))
    end)
end
)";

// The inputs of the extract's midge block, recomputed here from the world state (the sun elevation signed, as the extract keeps it).
f32 expected_amount(MidgeRig& r, f32 tod) {
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
    f32 sun = 0.0f;
    if (tod >= 0.0f) sun = std::clamp(-render::sun_direction_at(tod).y, -1.0f, 1.0f);
    const kit::Weather* wx = nullptr;
    r.w.query<const kit::Weather&>().each([&](const kit::Weather& c) {
        if (!wx) wx = &c;
    });
    const kit::WindGust* gust = nullptr;
    r.w.query<const kit::WindGust&>().each([&](const kit::WindGust& c) {
        if (!gust) gust = &c;
    });
    const bool raining = wx && (wx->current == kit::WeatherKind::Rain || wx->current == kit::WeatherKind::Storm);
    const f32 rain = raining ? wx->intensity : 0.0f;
    const f32 wind = wx ? kit::effective_wind(*wx, gust, kit::kGustSway) : 0.0f;
    return kit::midge_amount(r.mg(), season, phase, sun, wind, rain);
}

}  // namespace

TEST_CASE("midge api: the getters read the first Midge, the setter writes and clamps to the Range") {
    MidgeRig r(R"(
function update()
    local t = mce.tick()
    local function near(a, b) return math.abs(a - b) < 1e-6 end
    if t == 1 then
        assert(weather.midge_enabled() == 1 and weather.midge_manual() == 0 and weather.midge_strength() == 1)
        assert(near(weather.midge_window_full(), 0.12) and near(weather.midge_window_end(), 0.2))
        assert(near(weather.midge_twilight_low(), -0.12) and near(weather.midge_twilight_high(), 0.25))
        assert(near(weather.midge_wind_calm(), 0.2) and near(weather.midge_wind_still(), 0.6))
        assert(weather.midge_rate() == 30 and weather.midge_box_xz_m() == 8)
        assert(near(weather.midge_height_m(), 2.5) and near(weather.midge_size_m(), 0.025))
        assert(weather.midge_lifetime_s() == 3 and near(weather.midge_speed(), 0.35))
        assert(weather.midge_max_particles() == 192 and weather.midge_seed() == 1)
        local r, g, b = weather.midge_color()
        assert(near(r, 0.08) and near(g, 0.07) and near(b, 0.06))
        weather.set_midge{window_full = 0.1, window_end = 0.3, manual = 0.5, twilight_low = -0.5, twilight_high = 0.75,
                          wind_calm = 0.25, wind_still = 0.75, rate = 100, box_xz_m = 100, height_m = 20, size_m = 0.5,
                          lifetime_s = 20, speed = 4, max_particles = 512, seed = 77, strength = 0.75, color = {1, 0.5, 0.25}}
    end
    if t == 2 then
        assert(near(weather.midge_window_full(), 0.1) and near(weather.midge_window_end(), 0.3))
        assert(weather.midge_manual() == 0.5 and weather.midge_twilight_low() == -0.5 and weather.midge_twilight_high() == 0.75)
        assert(weather.midge_wind_calm() == 0.25 and weather.midge_wind_still() == 0.75)
        assert(weather.midge_rate() == 100 and weather.midge_box_xz_m() == 100 and weather.midge_height_m() == 20)
        assert(weather.midge_size_m() == 0.5 and weather.midge_lifetime_s() == 20 and weather.midge_speed() == 4)
        assert(weather.midge_max_particles() == 512 and weather.midge_seed() == 77 and weather.midge_strength() == 0.75)
        local r, g, b = weather.midge_color()
        assert(r == 1 and g == 0.5 and b == 0.25)
        weather.set_midge{enabled = 0}
    end
    if t == 3 then
        assert(weather.midge_enabled() == 0 and weather.midge_rate() == 100)  -- only the given keys changed
        weather.set_midge{enabled = 7, window_full = 1e9, window_end = 1e9, manual = 1e9, twilight_low = 1e9, twilight_high = 1e9,
                          wind_calm = 1e9, wind_still = 1e9, rate = 1e9, box_xz_m = 1e9, height_m = 1e9, size_m = 1e9,
                          lifetime_s = 1e9, speed = 1e9, max_particles = 1e9, seed = 1e12, strength = 1e9, color = {1e9, -1, 0.5}}
    end
    if t == 4 then
        assert(weather.midge_enabled() == 1 and weather.midge_window_full() == 0.5 and weather.midge_window_end() == 0.5)
        assert(weather.midge_manual() == 1 and weather.midge_twilight_low() == 0 and weather.midge_twilight_high() == 1)
        assert(weather.midge_wind_calm() == 1 and weather.midge_wind_still() == 1)
        assert(weather.midge_rate() == 200 and weather.midge_box_xz_m() == 100 and weather.midge_height_m() == 20)
        assert(weather.midge_size_m() == 0.5 and weather.midge_lifetime_s() == 20 and weather.midge_speed() == 4)
        assert(weather.midge_max_particles() == 512 and weather.midge_seed() == 4294967295 and weather.midge_strength() == 1)
        local r, g, b = weather.midge_color()
        assert(r == 1 and g == 0 and b == 0.5)
        weather.set_midge{enabled = 0, window_full = -1, window_end = -1, manual = -1, twilight_low = -1e9, twilight_high = -1,
                          wind_calm = -1, wind_still = -1, rate = -1, box_xz_m = -1, height_m = -1, size_m = -1,
                          lifetime_s = -1, speed = -1, max_particles = -1, seed = -1, strength = -1}
        weather.set_midge{}  -- nothing given, nothing changes
    end
    if t == 5 then
        assert(weather.midge_enabled() == 0 and weather.midge_window_full() == 0 and weather.midge_window_end() == 0)
        assert(weather.midge_manual() == 0 and weather.midge_twilight_low() == -1 and weather.midge_twilight_high() == 0)
        assert(weather.midge_wind_calm() == 0 and weather.midge_wind_still() == 0)
        assert(weather.midge_rate() == 0 and weather.midge_box_xz_m() == 1 and near(weather.midge_height_m(), 0.3))
        assert(near(weather.midge_size_m(), 0.005) and weather.midge_lifetime_s() == 0.5 and weather.midge_speed() == 0)
        assert(weather.midge_max_particles() == 8 and weather.midge_seed() == 0 and weather.midge_strength() == 0)
    end
end
)");
    r.step(6);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.mg().strength == 0.0f);
    CHECK(r.mg().enabled == 0);
    CHECK(r.mg().pad_[0] == 0);
    CHECK(r.mg().pad_[1] == 0);
    CHECK(r.mg().pad_[2] == 0);
}

TEST_CASE("midge api: midge_active is the kit helper over the season, the clock, the sun, the wind and the rain") {
    MidgeRig r(kProbeSource);
    kit::Season season;
    season.start_phase = 0.375f;
    r.season_e = r.w.spawn_with(season);
    kit::WorldClock clock;
    clock.paused = 1;
    clock.drive_environment = 0;
    r.clock_e = r.w.spawn_with(clock);
    kit::Environment3D& e = r.w.set_resource(kit::Environment3D{});
    r.weather_e = r.w.spawn_with(kit::Weather{});
    r.gust_e = r.w.spawn_with(kit::WindGust{});
    u32 distinct = 0, zero = 0, nonzero = 0;
    f32 last = -1.0f;
    for (f32 start : {0.375f, 0.5f, 0.8f}) {
        r.w.try_get_mut<kit::Season>(r.season_e)->start_phase = start;
        for (f32 hour : {0.0f, 6.0f, 19.0f}) {
            r.w.try_get_mut<kit::WorldClock>(r.clock_e)->hour = hour;
            for (f32 manual : {0.0f, 0.3f}) {
                r.mg().manual = manual;
                for (f32 tod : {-1.0f, 0.0f, 5.0f, 5.8f, 6.2f, 8.0f, 12.0f, 17.8f, 18.4f, 19.5f, 23.0f}) {
                    e.time_of_day = tod;
                    for (kit::WeatherKind kind : {kit::WeatherKind::Clear, kit::WeatherKind::Rain, kit::WeatherKind::Storm, kit::WeatherKind::Snow}) {
                        kit::Weather& wx = *r.w.try_get_mut<kit::Weather>(r.weather_e);
                        wx.current = wx.target = kind;
                        wx.intensity = 0.5f;
                        wx.wind = kind == kit::WeatherKind::Clear ? 0.1f : 0.5f;
                        r.step(2);
                        REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
                        const f32 expected = expected_amount(r, tod);
                        INFO("start " << start << " hour " << hour << " manual " << manual << " tod " << tod << " kind " << int(kind));
                        CHECK(bits(r.p().position.x) == bits(expected));
                        CHECK(r.p().position.x >= 0.0f);
                        CHECK(r.p().position.x <= 1.0f);
                        if (r.p().position.x > 0.0f) ++nonzero; else ++zero;
                        if (r.p().position.x != last) ++distinct;
                        last = r.p().position.x;
                    }
                }
            }
        }
    }
    CHECK(distinct > 8);
    CHECK(nonzero > 20);
    CHECK(zero > 20);
    // enabled 0 and strength 0 are exactly 0
    r.mg() = kit::Midge{};
    r.mg().manual = 0.5f;
    r.mg().enabled = 0;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
    r.mg().enabled = 1;
    r.mg().strength = 0.0f;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
}

TEST_CASE("midge api: the twilight gate sees the sun below the horizon (signed elevation), and with no Season only the manual floor shows") {
    MidgeRig q(kProbeSource);
    kit::Season season;
    season.start_phase = 0.375f;  // the summer centre: the window is fully open
    q.season_e = q.w.spawn_with(season);
    kit::Environment3D& env = q.w.set_resource(kit::Environment3D{});
    // find an hour with the sun well below the horizon and one with it a little above
    f32 night = -1.0f, dusk = -1.0f;
    for (f32 h = 0.0f; h < 24.0f; h += 0.25f) {
        const f32 s = -render::sun_direction_at(h).y;
        if (night < 0.0f && s < -0.5f) night = h;
        if (dusk < 0.0f && s > 0.0f && s < 0.1f) dusk = h;
    }
    REQUIRE(night >= 0.0f);
    REQUIRE(dusk >= 0.0f);
    env.time_of_day = night;
    q.step(2);
    REQUIRE(q.h().scripts()[0].state == ScriptState::Running);
    CHECK(q.p().position.x == 0.0f);  // deep night: the twilight gate is closed (an unsigned elevation would read the horizon)
    env.time_of_day = dusk;
    q.step(1);
    CHECK(q.p().position.x > 0.0f);
    // no clock: the elevation is 0 (the horizon), the twilight gate is open
    env.time_of_day = -1.0f;
    q.step(1);
    CHECK(bits(q.p().position.x) == bits(expected_amount(q, -1.0f)));
    CHECK(q.p().position.x > 0.0f);
    // no Season: the manual floor only
    q.w.destroy(q.season_e);
    q.step(1);
    CHECK(q.p().position.x == 0.0f);
    q.mg().manual = 0.625f;
    q.step(1);
    CHECK(q.p().position.x == 0.625f);  // calm, dry: the floor passes whole
    CHECK(bits(q.p().position.x) == bits(expected_amount(q, -1.0f)));
}

TEST_CASE("midge api: a world without a Midge raises a named error and the setter never adds one") {
    for (const char* name : {"midge_enabled", "midge_window_full", "midge_window_end", "midge_manual", "midge_twilight_low",
                             "midge_twilight_high", "midge_wind_calm", "midge_wind_still", "midge_rate", "midge_box_xz_m",
                             "midge_height_m", "midge_size_m", "midge_lifetime_s", "midge_speed", "midge_color",
                             "midge_max_particles", "midge_seed", "midge_strength", "midge_active"})
        expect_error((std::string("weather.") + name + "()").c_str(),
                     (std::string("weather.") + name + ": the scene has no Midge entity").c_str(), false);
    expect_error("weather.set_midge{}", "weather.set_midge: the scene has no Midge entity", false);
    expect_error("weather.set_midge{strength = 0}", "weather.set_midge: the scene has no Midge entity", false);
    expect_error("weather.midge_active()", "weather.midge_active: this world has no weather", false, false);
    expect_error("weather.set_midge{}", "weather.set_midge: this world has no weather", false, false);
    MidgeRig r("function update()\n    if mce.tick() == 1 then weather.set_midge{enabled = 0} end\nend\n", false);
    r.step(2);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    usize n = 0;
    r.w.query<kit::Midge>().each([&](Entity, kit::Midge&) { ++n; });
    CHECK(n == 0);
}

TEST_CASE("midge api: bad arguments raise an error that names the call and write nothing") {
    expect_error("weather.set_midge{bogus = 1}", "weather.set_midge: unknown key 'bogus'");
    expect_error("weather.set_midge{cover_low = 1}", "weather.set_midge: unknown key 'cover_low'");
    expect_error("weather.set_midge{rate = 'fast'}", "weather.set_midge: 'rate' is not a number");
    expect_error("weather.set_midge{strength = 0/0}", "weather.set_midge: 'strength' is not a number");
    expect_error("weather.set_midge{enabled = true}", "weather.set_midge: 'enabled' is not a number");
    expect_error("weather.set_midge{twilight_low = 'x'}", "weather.set_midge: 'twilight_low' is not a number");
    expect_error("weather.set_midge{speed = 'x'}", "weather.set_midge: 'speed' is not a number");
    expect_error("weather.set_midge{seed = 'x'}", "weather.set_midge: 'seed' is not a number");
    expect_error("weather.set_midge{color = 1}", "weather.set_midge: 'color' is not a table of three numbers");
    expect_error("weather.set_midge{color = {1, 2}}", "weather.set_midge: 'color' is not a table of three numbers");
    expect_error("weather.set_midge{color = {1, 'x', 3}}", "weather.set_midge: 'color' is not a table of three numbers");
    expect_error("weather.set_midge{[1] = 0.5}", "weather.set_midge: the keys are names");
    expect_error("weather.set_midge(0.5)", "table expected");
    expect_error("weather.set_midge()", "table expected");
    const kit::Midge def;
    for (const char* body : {"weather.set_midge{manual = 0.9, strength = 'x'}", "weather.set_midge{strength = 'x', manual = 0.9}",
                             "weather.set_midge{manual = 0.9, nope = 1}", "weather.set_midge{enabled = 0, rate = 'x'}",
                             "weather.set_midge{color = {0, 0, 0}, height_m = 'x'}",
                             "weather.set_midge{color = {1, 2}, manual = 0.9, enabled = 0}"}) {
        const std::string src = std::string("function update()\n    if mce.tick() == 1 then ") + body + " end\nend\n";
        MidgeRig r(src.c_str());
        r.step(2);
        INFO(body);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
        CHECK(r.mg().manual == def.manual);
        CHECK(r.mg().strength == def.strength);
        CHECK(r.mg().rate == def.rate);
        CHECK(r.mg().enabled == def.enabled);
        CHECK(r.mg().height_m == def.height_m);
        CHECK(r.mg().color.x == def.color.x);
        CHECK(r.mg().color.z == def.color.z);
    }
}
