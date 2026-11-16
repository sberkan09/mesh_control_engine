// M-BUTTERFLY step 27c: the butterfly functions of the `weather` Luau table over the first kit::Butterfly. Getters, clamping,
// butterfly_flying against the kit helper over the season, the sun and the rain, named errors, nothing written on error, and a
// missing Butterfly raises (the setter never adds one).

#include <doctest/doctest.h>

#include <bit>
#include <algorithm>
#include <cmath>
#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/butterfly.h"
#include "mce/kit/kit3d.h"
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

struct ButterflyRig {
    World w;
    ScriptHost host;
    u64 tick = 0;
    Entity bf_e, probe, weather_e;

    explicit ButterflyRig(const char* source, bool with_bf = true, bool install = true) {
        w.register_components<LocalTransform>();
        if (install) kit::install_weather(w, nullptr);
        INFO(source);
        Result<Array<u8>> bc = script::compile_unchecked("a", source);
        REQUIRE(bc);
        REQUIRE(host.add_bytecode("a", bc.value().span()));
        host.install(w);
        probe = w.spawn_with(LocalTransform{});
        if (with_bf) bf_e = w.spawn_with(kit::Butterfly{});
    }
    kit::Butterfly& bf() { return *w.try_get_mut<kit::Butterfly>(bf_e); }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
    const LocalTransform& p() { return *w.try_get<LocalTransform>(probe); }
    const ScriptHost& h() const { return host; }
};

std::string err(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

void expect_error(const char* body, const char* needle, bool with_bf = true, bool install = true) {
    const std::string src = std::string("function update()\n    if mce.tick() == 1 then\n        ") + body + "\n    end\nend\n";
    ButterflyRig r(src.c_str(), with_bf, install);
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
        mce.set(e, "LocalTransform", "position", vector.create(weather.butterfly_flying(), 0, 0))
    end)
end
)";

}  // namespace

TEST_CASE("butterfly api: the getters read the first Butterfly, the setter writes and clamps to the Range") {
    ButterflyRig r(R"(
function update()
    local t = mce.tick()
    local function near(a, b) return math.abs(a - b) < 1e-6 end
    if t == 1 then
        assert(weather.butterfly_enabled() == 1 and weather.butterfly_manual() == 0 and weather.butterfly_strength() == 1)
        assert(near(weather.butterfly_window_full(), 0.07) and near(weather.butterfly_window_end(), 0.14))
        assert(near(weather.butterfly_min_daylight(), 0.6) and near(weather.butterfly_wind_calm(), 0.25))
        assert(near(weather.butterfly_wind_still(), 0.7) and weather.butterfly_rate() == 5)
        assert(weather.butterfly_box_xz_m() == 18 and weather.butterfly_height_m() == 3)
        assert(near(weather.butterfly_size_m(), 0.12) and weather.butterfly_lifetime_s() == 8)
        assert(near(weather.butterfly_speed(), 0.8) and near(weather.butterfly_lift(), 0.2))
        assert(weather.butterfly_max_particles() == 48 and weather.butterfly_seed() == 1)
        local r, g, b = weather.butterfly_color()
        assert(near(r, 1) and near(g, 0.75) and near(b, 0.2))
        weather.set_butterfly{window_full = 0.1, window_end = 0.2, manual = 0.5, min_daylight = 0.5, wind_calm = 0.5,
                              wind_still = 0.75, rate = 30, box_xz_m = 100, height_m = 20, size_m = 0.5, lifetime_s = 20,
                              speed = 3, lift = 1.5, max_particles = 512, seed = 77, strength = 0.75, color = {1, 0.5, 0.25}}
    end
    if t == 2 then
        assert(near(weather.butterfly_window_full(), 0.1) and near(weather.butterfly_window_end(), 0.2))
        assert(weather.butterfly_manual() == 0.5 and weather.butterfly_min_daylight() == 0.5)
        assert(weather.butterfly_wind_calm() == 0.5 and weather.butterfly_wind_still() == 0.75 and weather.butterfly_rate() == 30)
        assert(weather.butterfly_box_xz_m() == 100 and weather.butterfly_height_m() == 20)
        assert(weather.butterfly_size_m() == 0.5 and weather.butterfly_lifetime_s() == 20 and weather.butterfly_speed() == 3)
        assert(weather.butterfly_lift() == 1.5)
        assert(weather.butterfly_max_particles() == 512 and weather.butterfly_seed() == 77 and weather.butterfly_strength() == 0.75)
        local r, g, b = weather.butterfly_color()
        assert(r == 1 and g == 0.5 and b == 0.25)
        weather.set_butterfly{enabled = 0}
    end
    if t == 3 then
        assert(weather.butterfly_enabled() == 0 and weather.butterfly_rate() == 30)  -- only the given keys changed
        weather.set_butterfly{enabled = 7, window_full = 1e9, window_end = 1e9, manual = 1e9, min_daylight = 1e9,
                              wind_calm = 1e9, wind_still = 1e9, rate = 1e9, box_xz_m = 1e9, height_m = 1e9, size_m = 1e9,
                              lifetime_s = 1e9, speed = 1e9, lift = 1e9, max_particles = 1e9, seed = 1e12, strength = 1e9,
                              color = {1e9, -1, 0.5}}
    end
    if t == 4 then
        assert(weather.butterfly_enabled() == 1 and weather.butterfly_window_full() == 0.5 and weather.butterfly_window_end() == 0.5)
        assert(weather.butterfly_manual() == 1 and weather.butterfly_min_daylight() == 1 and weather.butterfly_wind_calm() == 1)
        assert(weather.butterfly_wind_still() == 1 and weather.butterfly_rate() == 60)
        assert(weather.butterfly_box_xz_m() == 100 and weather.butterfly_height_m() == 20 and weather.butterfly_size_m() == 1)
        assert(weather.butterfly_lifetime_s() == 30 and weather.butterfly_speed() == 4 and weather.butterfly_lift() == 2)
        assert(weather.butterfly_max_particles() == 512 and weather.butterfly_seed() == 4294967295 and weather.butterfly_strength() == 1)
        local r, g, b = weather.butterfly_color()
        assert(r == 1 and g == 0 and b == 0.5)
        weather.set_butterfly{enabled = 0, window_full = -1, window_end = -1, manual = -1, min_daylight = -1, wind_calm = -1,
                              wind_still = -1, rate = -1, box_xz_m = -1, height_m = -1, size_m = -1, lifetime_s = -1,
                              speed = -1, lift = -1, max_particles = -1, seed = -1, strength = -1}
        weather.set_butterfly{}  -- nothing given, nothing changes
    end
    if t == 5 then
        assert(weather.butterfly_enabled() == 0 and weather.butterfly_window_full() == 0 and weather.butterfly_window_end() == 0)
        assert(weather.butterfly_manual() == 0 and weather.butterfly_min_daylight() == 0 and weather.butterfly_wind_calm() == 0)
        assert(weather.butterfly_wind_still() == 0 and weather.butterfly_rate() == 0)
        assert(weather.butterfly_box_xz_m() == 1 and weather.butterfly_height_m() == 1 and near(weather.butterfly_size_m(), 0.02))
        assert(weather.butterfly_lifetime_s() == 1 and weather.butterfly_speed() == 0 and weather.butterfly_lift() == 0)
        assert(weather.butterfly_max_particles() == 8 and weather.butterfly_seed() == 0 and weather.butterfly_strength() == 0)
    end
end
)");
    r.step(6);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.bf().strength == 0.0f);
    CHECK(r.bf().enabled == 0);
    CHECK(r.bf().pad_[0] == 0);
    CHECK(r.bf().pad_[1] == 0);
    CHECK(r.bf().pad_[2] == 0);
}

TEST_CASE("butterfly api: butterfly_flying is the kit helper over the season, the night, the wind and the rain") {
    ButterflyRig r(kProbeSource);
    kit::Season season;
    season.year_days = 10.0f;
    const Entity season_e = r.w.spawn_with(season);
    kit::WorldClock clock;  // held still and not driving the sun: the test sets the day, the hour and Environment3D itself
    clock.paused = 1;
    clock.drive_environment = 0;
    const Entity clock_e = r.w.spawn_with(clock);
    // the Environment3D resource (the one the extract and the script read first)
    kit::Environment3D& e = r.w.set_resource(kit::Environment3D{});
    r.weather_e = r.w.spawn_with(kit::Weather{});
    u32 distinct = 0, zero = 0, nonzero = 0;
    f32 last = -1.0f;
    for (bool with_season : {true, false}) {
        r.w.try_get_mut<kit::Season>(season_e)->enabled = with_season ? 1 : 0;
        for (u32 day : {0u, 3u, 4u, 7u}) {
            r.w.try_get_mut<kit::WorldClock>(clock_e)->day = day;
            r.w.try_get_mut<kit::WorldClock>(clock_e)->hour = 6.0f;
            for (f32 manual : {0.0f, 0.3f}) {
                r.bf().manual = manual;
                // the night fade: no clock (time_of_day below 0) and the day clock at night, dawn and noon
                for (f32 tod : {-1.0f, 0.0f, 5.5f, 6.5f, 12.0f, 18.5f}) {
                    e.time_of_day = tod;
                    for (kit::WeatherKind kind : {kit::WeatherKind::Clear, kit::WeatherKind::Rain, kit::WeatherKind::Storm, kit::WeatherKind::Snow}) {
                        kit::Weather& wx = *r.w.try_get_mut<kit::Weather>(r.weather_e);
                        wx.current = wx.target = kind;
                        wx.intensity = 0.6f;
                        r.step(2);
                        REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
                        const kit::Weather& after = *r.w.try_get<kit::Weather>(r.weather_e);
                        const bool raining = after.current == kit::WeatherKind::Rain || after.current == kit::WeatherKind::Storm;
                        f32 night_fade = 0.0f;
                        if (tod >= 0.0f) {
                            const f32 elev = -render::sun_direction_at(tod).y;
                            const f32 t = std::clamp((elev + 0.1f) / 0.2f, 0.0f, 1.0f);
                            night_fade = 1.0f - t * t * (3.0f - 2.0f * t);
                        }
                        const f32 wind = kit::effective_wind(after, nullptr, kit::kGustSway);
                        const kit::Season* sp = r.w.try_get<kit::Season>(season_e);
                        const f32 phase = kit::season_phase(*sp, day, 6.0f);
                        const f32 expected = kit::butterfly_amount(r.bf(), sp, phase, night_fade, wind, raining ? after.intensity : 0.0f);
                        INFO("season " << with_season << " day " << day << " manual " << manual << " tod " << tod << " kind " << int(kind));
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
    r.bf() = kit::Butterfly{};
    r.bf().manual = 0.5f;
    r.bf().enabled = 0;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
    r.bf().enabled = 1;
    r.bf().strength = 0.0f;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
}

TEST_CASE("butterfly api: with no Season, Weather or Environment3D only the manual floor and the default gates show") {
    ButterflyRig q(kProbeSource);
    q.step(2);
    REQUIRE(q.h().scripts()[0].state == ScriptState::Running);
    // no Environment3D: the default time_of_day gives the night fade; no Weather is calm and dry
    const kit::Environment3D def;
    f32 night_fade = 0.0f;
    if (def.time_of_day >= 0.0f) {
        const f32 elev = -render::sun_direction_at(def.time_of_day).y;
        const f32 t = std::clamp((elev + 0.1f) / 0.2f, 0.0f, 1.0f);
        night_fade = 1.0f - t * t * (3.0f - 2.0f * t);
    }
    CHECK(bits(q.p().position.x) == bits(kit::butterfly_amount(q.bf(), nullptr, 0.0f, night_fade, 0.0f, 0.0f)));
    CHECK(q.p().position.x == 0.0f);  // no season and manual 0
    q.bf().manual = 0.625f;
    q.bf().min_daylight = 0.0f;
    q.step(1);
    CHECK(bits(q.p().position.x) == bits(kit::butterfly_amount(q.bf(), nullptr, 0.0f, night_fade, 0.0f, 0.0f)));
    CHECK(q.p().position.x <= 0.625f);
}

TEST_CASE("butterfly api: a world without a Butterfly raises a named error and the setter never adds one") {
    for (const char* name : {"butterfly_enabled", "butterfly_window_full", "butterfly_window_end", "butterfly_manual",
                             "butterfly_min_daylight", "butterfly_wind_calm", "butterfly_wind_still", "butterfly_rate",
                             "butterfly_box_xz_m", "butterfly_height_m", "butterfly_size_m", "butterfly_lifetime_s",
                             "butterfly_speed", "butterfly_lift", "butterfly_color", "butterfly_max_particles", "butterfly_seed",
                             "butterfly_strength"})
        expect_error((std::string("weather.") + name + "()").c_str(),
                     (std::string("weather.") + name + ": the scene has no Butterfly entity").c_str(), false);
    expect_error("weather.butterfly_flying()", "weather.butterfly_flying: the scene has no Butterfly entity", false);
    expect_error("weather.set_butterfly{}", "weather.set_butterfly: the scene has no Butterfly entity", false);
    expect_error("weather.set_butterfly{strength = 0}", "weather.set_butterfly: the scene has no Butterfly entity", false);
    expect_error("weather.butterfly_flying()", "weather.butterfly_flying: this world has no weather", false, false);
    expect_error("weather.set_butterfly{}", "weather.set_butterfly: this world has no weather", false, false);
    ButterflyRig r("function update()\n    if mce.tick() == 1 then weather.set_butterfly{enabled = 0} end\nend\n", false);
    r.step(2);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    usize n = 0;
    r.w.query<kit::Butterfly>().each([&](Entity, kit::Butterfly&) { ++n; });
    CHECK(n == 0);
}

TEST_CASE("butterfly api: bad arguments raise an error that names the call and write nothing") {
    expect_error("weather.set_butterfly{bogus = 1}", "weather.set_butterfly: unknown key 'bogus'");
    expect_error("weather.set_butterfly{migration_full = 1}", "weather.set_butterfly: unknown key 'migration_full'");
    expect_error("weather.set_butterfly{rate = 'fast'}", "weather.set_butterfly: 'rate' is not a number");
    expect_error("weather.set_butterfly{strength = 0/0}", "weather.set_butterfly: 'strength' is not a number");
    expect_error("weather.set_butterfly{enabled = true}", "weather.set_butterfly: 'enabled' is not a number");
    expect_error("weather.set_butterfly{lift = 'x'}", "weather.set_butterfly: 'lift' is not a number");
    expect_error("weather.set_butterfly{seed = 'x'}", "weather.set_butterfly: 'seed' is not a number");
    expect_error("weather.set_butterfly{color = 1}", "weather.set_butterfly: 'color' is not a table of three numbers");
    expect_error("weather.set_butterfly{color = {1, 2}}", "weather.set_butterfly: 'color' is not a table of three numbers");
    expect_error("weather.set_butterfly{color = {1, 'x', 3}}", "weather.set_butterfly: 'color' is not a table of three numbers");
    expect_error("weather.set_butterfly{[1] = 0.5}", "weather.set_butterfly: the keys are names");
    expect_error("weather.set_butterfly(0.5)", "table expected");
    expect_error("weather.set_butterfly()", "table expected");
    const kit::Butterfly def;
    for (const char* body : {"weather.set_butterfly{manual = 0.9, strength = 'x'}", "weather.set_butterfly{strength = 'x', manual = 0.9}",
                             "weather.set_butterfly{manual = 0.9, nope = 1}", "weather.set_butterfly{enabled = 0, rate = 'x'}",
                             "weather.set_butterfly{color = {0, 0, 0}, height_m = 'x'}",
                             "weather.set_butterfly{color = {1, 2}, manual = 0.9, enabled = 0}"}) {
        const std::string src = std::string("function update()\n    if mce.tick() == 1 then ") + body + " end\nend\n";
        ButterflyRig r(src.c_str());
        r.step(2);
        INFO(body);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
        CHECK(r.bf().manual == def.manual);
        CHECK(r.bf().strength == def.strength);
        CHECK(r.bf().rate == def.rate);
        CHECK(r.bf().enabled == def.enabled);
        CHECK(r.bf().speed == def.speed);
        CHECK(r.bf().height_m == def.height_m);
        CHECK(r.bf().color.x == def.color.x);
        CHECK(r.bf().color.z == def.color.z);
    }
}
