// M-BIRDFLOCK step 26c: the flock functions of the `weather` Luau table over the first kit::Flock. Getters, clamping,
// flock_flying against the kit helper over the season, the sun and the rain, named errors, nothing written on error, and a
// missing Flock raises (the setter never adds one).

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/flock.h"
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

struct FlockRig {
    World w;
    ScriptHost host;
    u64 tick = 0;
    Entity fl_e, probe, weather_e;

    explicit FlockRig(const char* source, bool with_flock = true, bool install = true) {
        w.register_components<LocalTransform>();
        if (install) kit::install_weather(w, nullptr);
        INFO(source);
        Result<Array<u8>> bc = script::compile_unchecked("a", source);
        REQUIRE(bc);
        REQUIRE(host.add_bytecode("a", bc.value().span()));
        host.install(w);
        probe = w.spawn_with(LocalTransform{});
        if (with_flock) fl_e = w.spawn_with(kit::Flock{});
    }
    kit::Flock& fl() { return *w.try_get_mut<kit::Flock>(fl_e); }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
    const LocalTransform& p() { return *w.try_get<LocalTransform>(probe); }
    const ScriptHost& h() const { return host; }
};

std::string err(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

void expect_error(const char* body, const char* needle, bool with_flock = true, bool install = true) {
    const std::string src = std::string("function update()\n    if mce.tick() == 1 then\n        ") + body + "\n    end\nend\n";
    FlockRig r(src.c_str(), with_flock, install);
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
        mce.set(e, "LocalTransform", "position", vector.create(weather.flock_flying(), 0, 0))
    end)
end
)";

}  // namespace

TEST_CASE("flock api: the getters read the first Flock, the setter writes and clamps to the Range") {
    FlockRig r(R"(
function update()
    local t = mce.tick()
    local function near(a, b) return math.abs(a - b) < 1e-6 end
    if t == 1 then
        assert(weather.flock_enabled() == 1 and weather.flock_manual() == 0 and weather.flock_strength() == 1)
        assert(near(weather.flock_migration_full(), 0.06) and near(weather.flock_migration_end(), 0.12))
        assert(near(weather.flock_twilight_width(), 0.25) and weather.flock_rate() == 3)
        assert(weather.flock_box_xz_m() == 60 and weather.flock_height_m() == 45)
        assert(near(weather.flock_size_m(), 0.35) and weather.flock_lifetime_s() == 14 and weather.flock_speed() == 9)
        assert(weather.flock_heading_deg() == 70)
        assert(weather.flock_max_particles() == 64 and weather.flock_seed() == 1)
        local r, g, b = weather.flock_color()
        assert(near(r, 0.08) and near(g, 0.07) and near(b, 0.07))
        weather.set_flock{migration_full = 0.1, migration_end = 0.2, manual = 0.5, twilight_width = 0.5, rate = 30,
                          box_xz_m = 100, height_m = 80, size_m = 1.5, lifetime_s = 20, speed = 25, heading_deg = 200,
                          max_particles = 512, seed = 77, strength = 0.75, color = {1, 0.5, 0.25}}
    end
    if t == 2 then
        assert(near(weather.flock_migration_full(), 0.1) and near(weather.flock_migration_end(), 0.2))
        assert(weather.flock_manual() == 0.5 and weather.flock_twilight_width() == 0.5 and weather.flock_rate() == 30)
        assert(weather.flock_box_xz_m() == 100 and weather.flock_height_m() == 80)
        assert(weather.flock_size_m() == 1.5 and weather.flock_lifetime_s() == 20 and weather.flock_speed() == 25)
        assert(weather.flock_heading_deg() == 200)
        assert(weather.flock_max_particles() == 512 and weather.flock_seed() == 77 and weather.flock_strength() == 0.75)
        local r, g, b = weather.flock_color()
        assert(r == 1 and g == 0.5 and b == 0.25)
        weather.set_flock{enabled = 0}
    end
    if t == 3 then
        assert(weather.flock_enabled() == 0 and weather.flock_rate() == 30)  -- only the given keys changed
        weather.set_flock{enabled = 7, migration_full = 1e9, migration_end = 1e9, manual = 1e9, twilight_width = 1e9,
                          rate = 1e9, box_xz_m = 1e9, height_m = 1e9, size_m = 1e9, lifetime_s = 1e9, speed = 1e9,
                          heading_deg = 1e9, max_particles = 1e9, seed = 1e12, strength = 1e9, color = {1e9, -1, 0.5}}
    end
    if t == 4 then
        assert(weather.flock_enabled() == 1 and weather.flock_migration_full() == 0.5 and weather.flock_migration_end() == 0.5)
        assert(weather.flock_manual() == 1 and weather.flock_twilight_width() == 1 and weather.flock_rate() == 60)
        assert(weather.flock_box_xz_m() == 200 and weather.flock_height_m() == 120 and weather.flock_size_m() == 2)
        assert(weather.flock_lifetime_s() == 40 and weather.flock_speed() == 40 and weather.flock_heading_deg() == 360)
        assert(weather.flock_max_particles() == 1024 and weather.flock_seed() == 4294967295 and weather.flock_strength() == 1)
        local r, g, b = weather.flock_color()
        assert(r == 1 and g == 0 and b == 0.5)
        weather.set_flock{enabled = 0, migration_full = -1, migration_end = -1, manual = -1, twilight_width = -1, rate = -1,
                          box_xz_m = -1, height_m = -1, size_m = -1, lifetime_s = -1, speed = -1, heading_deg = -1,
                          max_particles = -1, seed = -1, strength = -1}
        weather.set_flock{}  -- nothing given, nothing changes
    end
    if t == 5 then
        assert(weather.flock_enabled() == 0 and weather.flock_migration_full() == 0 and weather.flock_migration_end() == 0)
        assert(weather.flock_manual() == 0 and near(weather.flock_twilight_width(), 0.01) and weather.flock_rate() == 0)
        assert(weather.flock_box_xz_m() == 1 and weather.flock_height_m() == 1 and near(weather.flock_size_m(), 0.05))
        assert(weather.flock_lifetime_s() == 1 and weather.flock_speed() == 0 and weather.flock_heading_deg() == 0)
        assert(weather.flock_max_particles() == 8 and weather.flock_seed() == 0 and weather.flock_strength() == 0)
    end
end
)");
    r.step(6);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.fl().strength == 0.0f);
    CHECK(r.fl().enabled == 0);
    CHECK(r.fl().pad_[0] == 0);
    CHECK(r.fl().pad_[1] == 0);
    CHECK(r.fl().pad_[2] == 0);
}

TEST_CASE("flock api: flock_flying is the kit helper over the season, the sun and the rain") {
    FlockRig r(kProbeSource);
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
        for (u32 day : {0u, 1u, 5u, 6u, 7u}) {
            r.w.try_get_mut<kit::WorldClock>(clock_e)->day = day;
            r.w.try_get_mut<kit::WorldClock>(clock_e)->hour = 6.0f;
            for (f32 manual : {0.0f, 0.3f}) {
                r.fl().manual = manual;
                // the sun: a fixed direction (time_of_day below 0, unnormalised) and the day clock
                for (f32 tod : {-1.0f, 0.0f, 5.0f, 6.0f, 12.0f, 18.5f}) {
                    e.time_of_day = tod;
                    e.sun_direction = math::vec3{0.0f, -0.05f * (1.0f + static_cast<f32>(day)), 0.5f};  // a long vector: y is normalised
                    for (kit::WeatherKind kind : {kit::WeatherKind::Clear, kit::WeatherKind::Rain, kit::WeatherKind::Storm, kit::WeatherKind::Snow}) {
                        kit::Weather& wx = *r.w.try_get_mut<kit::Weather>(r.weather_e);
                        wx.current = wx.target = kind;
                        wx.intensity = 0.6f;
                        r.step(1);
                        REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
                        const kit::Weather& after = *r.w.try_get<kit::Weather>(r.weather_e);
                        const bool raining = after.current == kit::WeatherKind::Rain || after.current == kit::WeatherKind::Storm;
                        const f32 sun_y = tod >= 0.0f ? render::sun_direction_at(tod).y : math::normalize(e.sun_direction).y;
                        const kit::Season* sp = r.w.try_get<kit::Season>(season_e);
                        const f32 phase = kit::season_phase(*sp, day, 6.0f);
                        const f32 expected = kit::flock_amount(r.fl(), sp, phase, sun_y, raining ? after.intensity : 0.0f);
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
    r.fl() = kit::Flock{};
    r.fl().manual = 0.5f;
    r.fl().enabled = 0;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
    r.fl().enabled = 1;
    r.fl().strength = 0.0f;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
}

TEST_CASE("flock api: with no Season, Weather or Environment3D only the manual floor and the default sun gate show") {
    FlockRig q(kProbeSource);
    q.step(2);
    REQUIRE(q.h().scripts()[0].state == ScriptState::Running);
    // no Environment3D: the default time_of_day drives the sun; the amount is the kit helper's either way
    const kit::Environment3D def;
    const f32 sun_y = def.time_of_day >= 0.0f ? render::sun_direction_at(def.time_of_day).y : math::normalize(def.sun_direction).y;
    CHECK(bits(q.p().position.x) == bits(kit::flock_amount(q.fl(), nullptr, 0.0f, sun_y, 0.0f)));
    CHECK(q.p().position.x == 0.0f);  // no season and manual 0
    q.fl().manual = 0.625f;
    q.fl().twilight_width = 1.0f;
    q.step(1);
    CHECK(bits(q.p().position.x) == bits(kit::flock_amount(q.fl(), nullptr, 0.0f, sun_y, 0.0f)));
    CHECK(q.p().position.x <= 0.625f);
}

TEST_CASE("flock api: a world without a Flock raises a named error and the setter never adds one") {
    for (const char* name : {"flock_enabled", "flock_migration_full", "flock_migration_end", "flock_manual", "flock_twilight_width",
                             "flock_rate", "flock_box_xz_m", "flock_height_m", "flock_size_m", "flock_lifetime_s", "flock_speed",
                             "flock_heading_deg", "flock_color", "flock_max_particles", "flock_seed", "flock_strength"})
        expect_error((std::string("weather.") + name + "()").c_str(),
                     (std::string("weather.") + name + ": the scene has no Flock entity").c_str(), false);
    expect_error("weather.flock_flying()", "weather.flock_flying: the scene has no Flock entity", false);
    expect_error("weather.set_flock{}", "weather.set_flock: the scene has no Flock entity", false);
    expect_error("weather.set_flock{strength = 0}", "weather.set_flock: the scene has no Flock entity", false);
    expect_error("weather.flock_flying()", "weather.flock_flying: this world has no weather", false, false);
    expect_error("weather.set_flock{}", "weather.set_flock: this world has no weather", false, false);
    FlockRig r("function update()\n    if mce.tick() == 1 then weather.set_flock{enabled = 0} end\nend\n", false);
    r.step(2);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    usize n = 0;
    r.w.query<kit::Flock>().each([&](Entity, kit::Flock&) { ++n; });
    CHECK(n == 0);
}

TEST_CASE("flock api: bad arguments raise an error that names the call and write nothing") {
    expect_error("weather.set_flock{bogus = 1}", "weather.set_flock: unknown key 'bogus'");
    expect_error("weather.set_flock{wind_start = 1}", "weather.set_flock: unknown key 'wind_start'");
    expect_error("weather.set_flock{rate = 'fast'}", "weather.set_flock: 'rate' is not a number");
    expect_error("weather.set_flock{strength = 0/0}", "weather.set_flock: 'strength' is not a number");
    expect_error("weather.set_flock{enabled = true}", "weather.set_flock: 'enabled' is not a number");
    expect_error("weather.set_flock{heading_deg = 'x'}", "weather.set_flock: 'heading_deg' is not a number");
    expect_error("weather.set_flock{seed = 'x'}", "weather.set_flock: 'seed' is not a number");
    expect_error("weather.set_flock{color = 1}", "weather.set_flock: 'color' is not a table of three numbers");
    expect_error("weather.set_flock{color = {1, 2}}", "weather.set_flock: 'color' is not a table of three numbers");
    expect_error("weather.set_flock{color = {1, 'x', 3}}", "weather.set_flock: 'color' is not a table of three numbers");
    expect_error("weather.set_flock{[1] = 0.5}", "weather.set_flock: the keys are names");
    expect_error("weather.set_flock(0.5)", "table expected");
    expect_error("weather.set_flock()", "table expected");
    const kit::Flock def;
    for (const char* body : {"weather.set_flock{manual = 0.9, strength = 'x'}", "weather.set_flock{strength = 'x', manual = 0.9}",
                             "weather.set_flock{manual = 0.9, nope = 1}", "weather.set_flock{enabled = 0, rate = 'x'}",
                             "weather.set_flock{color = {0, 0, 0}, height_m = 'x'}",
                             "weather.set_flock{color = {1, 2}, manual = 0.9, enabled = 0}"}) {
        const std::string src = std::string("function update()\n    if mce.tick() == 1 then ") + body + " end\nend\n";
        FlockRig r(src.c_str());
        r.step(2);
        INFO(body);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
        CHECK(r.fl().manual == def.manual);
        CHECK(r.fl().strength == def.strength);
        CHECK(r.fl().rate == def.rate);
        CHECK(r.fl().enabled == def.enabled);
        CHECK(r.fl().speed == def.speed);
        CHECK(r.fl().height_m == def.height_m);
        CHECK(r.fl().color.x == def.color.x);
        CHECK(r.fl().color.z == def.color.z);
    }
}
