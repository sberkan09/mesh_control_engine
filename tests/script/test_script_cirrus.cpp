// M-CIRRUS step 53c: the cirrus functions of the `weather` Luau table over the first kit::Cirrus. Getters, clamping, the
// read-only cirrus_coverage against the kit helper over weather and clock, named errors, nothing written on an error, and
// a missing Cirrus raises (the setter never adds one).

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/cirrus.h"
#include "mce/kit/weather.h"
#include "mce/kit/world_clock.h"
#include "mce/script/compiler.h"

using namespace mce;
using script::ScriptHost;
using script::ScriptState;

namespace {

struct CirrusRig {
    World w;
    ScriptHost host;
    u64 tick = 0;
    Entity cirrus_e, probe, weather_e;

    explicit CirrusRig(const char* source, bool with_cirrus = true, bool install = true) {
        w.register_components<LocalTransform>();
        if (install) kit::install_weather(w, nullptr);
        INFO(source);
        Result<Array<u8>> bc = script::compile_unchecked("a", source);
        REQUIRE(bc);
        REQUIRE(host.add_bytecode("a", bc.value().span()));
        host.install(w);
        probe = w.spawn_with(LocalTransform{});
        if (with_cirrus) cirrus_e = w.spawn_with(kit::Cirrus{});
    }
    kit::Cirrus& cirrus() { return *w.try_get_mut<kit::Cirrus>(cirrus_e); }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
    const LocalTransform& p() { return *w.try_get<LocalTransform>(probe); }
    const ScriptHost& h() const { return host; }
};

std::string err(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

void expect_error(const char* body, const char* needle, bool with_cirrus = true, bool install = true) {
    const std::string src = std::string("function update()\n    if mce.tick() == 1 then\n        ") + body + "\n    end\nend\n";
    CirrusRig r(src.c_str(), with_cirrus, install);
    r.step(2);
    INFO(body);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    INFO(err(r.h()));
    CHECK(err(r.h()).find(needle) != std::string::npos);
}

u32 bits(f32 v) { return std::bit_cast<u32>(v); }

}  // namespace

TEST_CASE("cirrus api: the getters read the first Cirrus, the setter writes and clamps to the Range") {
    CirrusRig r(R"(
function update()
    local t = mce.tick()
    if t == 1 then
        assert(weather.cirrus_strength() == 1 and math.abs(weather.cirrus_fair_coverage() - 0.45) < 1e-6)
        assert(math.abs(weather.cirrus_front_coverage() - 0.7) < 1e-6 and math.abs(weather.cirrus_density() - 0.35) < 1e-6)
        assert(weather.cirrus_scale() == 3000 and weather.cirrus_stretch() == 6 and weather.cirrus_speed_scale() == 2)
        assert(weather.cirrus_height() == 9000)
        weather.set_cirrus{strength = 0.5, fair_coverage = 0.25, front_coverage = 0.75, density = 0.5, scale = 5000,
                           stretch = 8, speed_scale = 3, height = 12000}
    end
    if t == 2 then
        assert(weather.cirrus_strength() == 0.5 and weather.cirrus_fair_coverage() == 0.25)
        assert(weather.cirrus_front_coverage() == 0.75 and weather.cirrus_density() == 0.5)
        assert(weather.cirrus_scale() == 5000 and weather.cirrus_stretch() == 8 and weather.cirrus_speed_scale() == 3)
        assert(weather.cirrus_height() == 12000)
        weather.set_cirrus{density = 0.2}
    end
    if t == 3 then
        assert(math.abs(weather.cirrus_density() - 0.2) < 1e-6 and weather.cirrus_strength() == 0.5 and weather.cirrus_height() == 12000)  -- only the given key changed
        weather.set_cirrus{strength = 1e9, fair_coverage = 1e9, front_coverage = 1e9, density = 1e9, scale = 1e9,
                           stretch = 1e9, speed_scale = 1e9, height = 1e9}
    end
    if t == 4 then
        assert(weather.cirrus_strength() == 1 and weather.cirrus_fair_coverage() == 1 and weather.cirrus_front_coverage() == 1)
        assert(weather.cirrus_density() == 1 and weather.cirrus_scale() == 40000 and weather.cirrus_stretch() == 16)
        assert(weather.cirrus_speed_scale() == 10 and weather.cirrus_height() == 30000)
        weather.set_cirrus{strength = -1, fair_coverage = -1, front_coverage = -1, density = -1, scale = -1,
                           stretch = -1, speed_scale = -1, height = -1}
        weather.set_cirrus{}  -- nothing given, nothing changes
    end
    if t == 5 then
        assert(weather.cirrus_strength() == 0 and weather.cirrus_fair_coverage() == 0 and weather.cirrus_front_coverage() == 0)
        assert(weather.cirrus_density() == 0 and weather.cirrus_scale() == 200 and weather.cirrus_stretch() == 1)
        assert(weather.cirrus_speed_scale() == 0 and weather.cirrus_height() == 1000)
    end
end
)");
    r.step(6);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.cirrus().height_m == 1000.0f);
    CHECK(r.cirrus().scale_m == 200.0f);
}

TEST_CASE("cirrus api: cirrus_coverage is the kit helper over weather and clock") {
    // Every tick the script writes the coverage to position.x.
    CirrusRig r(R"(
function update()
    mce.each("LocalTransform", function(e)
        mce.set(e, "LocalTransform", "position", vector.create(weather.cirrus_coverage(), 0, 0))
    end)
end
)");
    r.w.register_components<kit::WorldClock>();
    kit::WorldClock clock;
    clock.day = 3;
    clock.hour = 6.5f;
    const Entity clock_e = r.w.spawn_with(clock);
    r.weather_e = r.w.spawn_with(kit::Weather{});

    u32 nonzero = 0;
    for (const f32 hour : {0.0f, 6.5f, 13.0f, 23.5f}) {
        for (const u32 day : {0u, 3u, 40u}) {
            for (const f32 inten : {0.0f, 0.4f, 1.0f}) {
                for (int kind = 0; kind < 4; ++kind) {
                    kit::WorldClock& k = *r.w.try_get_mut<kit::WorldClock>(clock_e);
                    k.day = day;
                    k.hour = hour;
                    kit::Weather& wx = *r.w.try_get_mut<kit::Weather>(r.weather_e);
                    wx.current = kind == 0 ? kit::WeatherKind::Clear
                                 : kind == 1 ? kit::WeatherKind::Overcast
                                 : kind == 2 ? kit::WeatherKind::Snow
                                             : kit::WeatherKind::Storm;
                    wx.target = kind == 3 ? kit::WeatherKind::Clear : wx.current;  // one pending change
                    wx.intensity = inten;
                    r.step(1);
                    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
                    // the system ran before the script this tick, so its post-tick state is what the script read
                    const f32 expected = kit::cirrus_coverage(r.cirrus(), wx, kit::cirrus_day_fraction(k.day, k.hour));
                    INFO("hour " << hour << " day " << day << " intensity " << inten << " kind " << kind);
                    CHECK(bits(r.p().position.x) == bits(expected));
                    if (expected > 0.0f) ++nonzero;
                }
            }
        }
    }
    CHECK(nonzero > 50);

    // strength 0 is exactly 0
    r.cirrus().strength = 0.0f;
    r.step(1);
    CHECK(bits(r.p().position.x) == 0u);
}

TEST_CASE("cirrus api: without a WorldClock the clock is day 0, hour 0; without a Weather it raises") {
    CirrusRig r(R"(
function update()
    mce.each("LocalTransform", function(e)
        mce.set(e, "LocalTransform", "position", vector.create(weather.cirrus_coverage(), 0, 0))
    end)
end
)");
    r.weather_e = r.w.spawn_with(kit::Weather{});
    r.step(1);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(bits(r.p().position.x) == bits(kit::cirrus_coverage(r.cirrus(), *r.w.try_get<kit::Weather>(r.weather_e), 0.0f)));
    CHECK(r.p().position.x > 0.0f);

    expect_error("weather.cirrus_coverage()", "weather.cirrus_coverage: the scene has no Weather entity");
}

TEST_CASE("cirrus api: a missing Cirrus raises, the setter never adds one") {
    for (const char* name : {"cirrus_strength", "cirrus_fair_coverage", "cirrus_front_coverage", "cirrus_density", "cirrus_scale",
                             "cirrus_stretch", "cirrus_speed_scale", "cirrus_height", "cirrus_coverage"})
        expect_error((std::string("weather.") + name + "()").c_str(),
                     (std::string("weather.") + name + ": the scene has no Cirrus entity").c_str(), false);
    expect_error("weather.set_cirrus{}", "weather.set_cirrus: the scene has no Cirrus entity", false);
    expect_error("weather.set_cirrus{density = 0.2}", "weather.set_cirrus: the scene has no Cirrus entity", false);
    expect_error("weather.cirrus_coverage()", "weather.cirrus_coverage: this world has no weather", false, false);
    expect_error("weather.set_cirrus{}", "weather.set_cirrus: this world has no weather", false, false);
    CirrusRig r("function update()\n    if mce.tick() == 1 then weather.set_cirrus{density = 0.2} end\nend\n", false);
    r.step(2);
    CHECK(r.h().scripts()[0].state == ScriptState::Failed);
    u32 n = 0;
    r.w.query<const kit::Cirrus&>().each([&](const kit::Cirrus&) { ++n; });
    CHECK(n == 0);
}

TEST_CASE("cirrus api: named errors and nothing is written on an error") {
    expect_error("weather.set_cirrus{bogus = 1}", "weather.set_cirrus: unknown key 'bogus'");
    expect_error("weather.set_cirrus{height_m = 1}", "weather.set_cirrus: unknown key 'height_m'");
    expect_error("weather.set_cirrus{density = 'high'}", "weather.set_cirrus: 'density' is not a number");
    expect_error("weather.set_cirrus{scale = 0/0}", "weather.set_cirrus: 'scale' is not a number");
    expect_error("weather.set_cirrus{strength = true}", "weather.set_cirrus: 'strength' is not a number");
    expect_error("weather.set_cirrus{[1] = 0.5}", "weather.set_cirrus: the keys are names");
    expect_error("weather.set_cirrus(0.5)", "table expected");
    expect_error("weather.set_cirrus()", "table expected");
    const kit::Cirrus def;
    CirrusRig r(R"(
function update()
    if mce.tick() == 1 then
        weather.set_cirrus{strength = 0.5, density = 0.9, bogus = 2}
    end
end
)");
    r.step(2);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    CHECK(r.cirrus().strength == def.strength);  // the keys before the bad one were not written
    CHECK(r.cirrus().density == def.density);
    CirrusRig bad(R"(
function update()
    if mce.tick() == 1 then
        weather.set_cirrus{strength = 0.5, density = 'x'}
    end
end
)");
    bad.step(2);
    REQUIRE(bad.h().scripts()[0].state == ScriptState::Failed);
    CHECK(bad.cirrus().strength == def.strength);
    CHECK(bad.cirrus().density == def.density);
}
