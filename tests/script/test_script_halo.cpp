// M-HALO step 54c: the halo functions of the `weather` Luau table over the first kit::Halo. Getters, clamping, the
// read-only halo_level against the kit helper over weather and clock, named errors, nothing written on an error, and a
// missing Halo raises (the setter never adds one).

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/cirrus.h"
#include "mce/kit/halo.h"
#include "mce/kit/weather.h"
#include "mce/kit/world_clock.h"
#include "mce/script/compiler.h"

using namespace mce;
using script::ScriptHost;
using script::ScriptState;

namespace {

struct HaloRig {
    World w;
    ScriptHost host;
    u64 tick = 0;
    Entity halo_e, probe, weather_e, cirrus_e;

    explicit HaloRig(const char* source, bool with_halo = true, bool install = true) {
        w.register_components<LocalTransform>();
        if (install) kit::install_weather(w, nullptr);
        INFO(source);
        Result<Array<u8>> bc = script::compile_unchecked("a", source);
        REQUIRE(bc);
        REQUIRE(host.add_bytecode("a", bc.value().span()));
        host.install(w);
        probe = w.spawn_with(LocalTransform{});
        if (with_halo) halo_e = w.spawn_with(kit::Halo{});
    }
    kit::Halo& halo() { return *w.try_get_mut<kit::Halo>(halo_e); }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
    const LocalTransform& p() { return *w.try_get<LocalTransform>(probe); }
    const ScriptHost& h() const { return host; }
};

std::string err(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

void expect_error(const char* body, const char* needle, bool with_halo = true, bool install = true) {
    const std::string src = std::string("function update()\n    if mce.tick() == 1 then\n        ") + body + "\n    end\nend\n";
    HaloRig r(src.c_str(), with_halo, install);
    r.step(2);
    INFO(body);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    INFO(err(r.h()));
    CHECK(err(r.h()).find(needle) != std::string::npos);
}

u32 bits(f32 v) { return std::bit_cast<u32>(v); }

}  // namespace

TEST_CASE("halo api: the getters read the first Halo, the setter writes and clamps to the Range") {
    HaloRig r(R"(
function update()
    local t = mce.tick()
    if t == 1 then
        assert(weather.halo_strength() == 1 and weather.halo_sun_dogs() == 1)
        assert(weather.halo_moon() == 0.5 and weather.halo_dispersion() == 1)
        weather.set_halo{strength = 0.5, sun_dogs = 0.25, moon = 0.75, dispersion = 0.125}
    end
    if t == 2 then
        assert(weather.halo_strength() == 0.5 and weather.halo_sun_dogs() == 0.25)
        assert(weather.halo_moon() == 0.75 and weather.halo_dispersion() == 0.125)
        weather.set_halo{moon = 0.5}
    end
    if t == 3 then
        assert(weather.halo_moon() == 0.5 and weather.halo_strength() == 0.5 and weather.halo_dispersion() == 0.125)  -- only the given key changed
        weather.set_halo{strength = 1e9, sun_dogs = 1e9, moon = 1e9, dispersion = 1e9}
    end
    if t == 4 then
        assert(weather.halo_strength() == 1 and weather.halo_sun_dogs() == 1 and weather.halo_moon() == 1 and weather.halo_dispersion() == 1)
        weather.set_halo{strength = -1, sun_dogs = -1, moon = -1, dispersion = -1}
        weather.set_halo{}  -- nothing given, nothing changes
    end
    if t == 5 then
        assert(weather.halo_strength() == 0 and weather.halo_sun_dogs() == 0 and weather.halo_moon() == 0 and weather.halo_dispersion() == 0)
    end
end
)");
    r.step(6);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.halo().strength == 0.0f);
    CHECK(r.halo().dispersion == 0.0f);
}

TEST_CASE("halo api: halo_level is the kit helper over the cirrus cover of weather and clock") {
    // Every tick the script writes the level to position.x.
    HaloRig r(R"(
function update()
    mce.each("LocalTransform", function(e)
        mce.set(e, "LocalTransform", "position", vector.create(weather.halo_level(), 0, 0))
    end)
end
)");
    r.w.register_components<kit::WorldClock>();
    kit::WorldClock clock;
    const Entity clock_e = r.w.spawn_with(clock);
    r.weather_e = r.w.spawn_with(kit::Weather{});
    r.cirrus_e = r.w.spawn_with(kit::Cirrus{});

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
                    wx.target = kind == 3 ? kit::WeatherKind::Clear : wx.current;
                    wx.intensity = inten;
                    r.step(1);
                    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
                    // the system ran before the script this tick, so its post-tick state is what the script read
                    const f32 cover = kit::cirrus_coverage(*r.w.try_get<kit::Cirrus>(r.cirrus_e), wx,
                                                           kit::cirrus_day_fraction(k.day, k.hour));
                    const f32 expected = kit::halo_level(r.halo(), cover);
                    INFO("hour " << hour << " day " << day << " intensity " << inten << " kind " << kind);
                    CHECK(bits(r.p().position.x) == bits(expected));
                    if (expected > 0.0f) ++nonzero;
                }
            }
        }
    }
    CHECK(nonzero > 20);

    // a thin veil (cover at most 0.1) shows no ring
    r.w.try_get_mut<kit::Cirrus>(r.cirrus_e)->strength = 0.1f;
    r.w.try_get_mut<kit::Cirrus>(r.cirrus_e)->fair_coverage = 0.5f;
    r.w.try_get_mut<kit::Cirrus>(r.cirrus_e)->front_coverage = 0.7f;
    r.step(1);
    CHECK(bits(r.p().position.x) == 0u);

    // strength 0 is exactly 0, even under a full veil
    r.w.try_get_mut<kit::Cirrus>(r.cirrus_e)->strength = 1.0f;
    r.step(1);
    CHECK(r.p().position.x > 0.0f);
    r.halo().strength = 0.0f;
    r.step(1);
    CHECK(bits(r.p().position.x) == 0u);
}

TEST_CASE("halo api: without a WorldClock the clock is day 0, hour 0; without a Weather or a Cirrus it raises") {
    HaloRig r(R"(
function update()
    mce.each("LocalTransform", function(e)
        mce.set(e, "LocalTransform", "position", vector.create(weather.halo_level(), 0, 0))
    end)
end
)");
    r.weather_e = r.w.spawn_with(kit::Weather{});
    r.cirrus_e = r.w.spawn_with(kit::Cirrus{});
    r.w.try_get_mut<kit::Cirrus>(r.cirrus_e)->fair_coverage = 0.8f;  // a veil at day 0 whatever the hash says
    r.w.try_get_mut<kit::Cirrus>(r.cirrus_e)->front_coverage = 0.9f;
    r.step(1);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    const f32 cover = kit::cirrus_coverage(*r.w.try_get<kit::Cirrus>(r.cirrus_e), *r.w.try_get<kit::Weather>(r.weather_e), 0.0f);
    CHECK(bits(r.p().position.x) == bits(kit::halo_level(r.halo(), cover)));
    CHECK(r.p().position.x > 0.0f);

    expect_error("weather.halo_level()", "weather.halo_level: the scene has no Cirrus entity");
}

TEST_CASE("halo api: a missing Halo raises, the setter never adds one") {
    for (const char* name : {"halo_strength", "halo_sun_dogs", "halo_moon", "halo_dispersion", "halo_level"})
        expect_error((std::string("weather.") + name + "()").c_str(),
                     (std::string("weather.") + name + ": the scene has no Halo entity").c_str(), false);
    expect_error("weather.set_halo{}", "weather.set_halo: the scene has no Halo entity", false);
    expect_error("weather.set_halo{moon = 0.2}", "weather.set_halo: the scene has no Halo entity", false);
    expect_error("weather.halo_level()", "weather.halo_level: this world has no weather", false, false);
    expect_error("weather.set_halo{}", "weather.set_halo: this world has no weather", false, false);
    HaloRig r("function update()\n    if mce.tick() == 1 then weather.set_halo{moon = 0.2} end\nend\n", false);
    r.step(2);
    CHECK(r.h().scripts()[0].state == ScriptState::Failed);
    u32 n = 0;
    r.w.query<const kit::Halo&>().each([&](const kit::Halo&) { ++n; });
    CHECK(n == 0);
}

TEST_CASE("halo api: named errors and nothing is written on an error") {
    expect_error("weather.set_halo{bogus = 1}", "weather.set_halo: unknown key 'bogus'");
    expect_error("weather.set_halo{sun_dog = 1}", "weather.set_halo: unknown key 'sun_dog'");
    expect_error("weather.set_halo{moon = 'high'}", "weather.set_halo: 'moon' is not a number");
    expect_error("weather.set_halo{dispersion = 0/0}", "weather.set_halo: 'dispersion' is not a number");
    expect_error("weather.set_halo{strength = true}", "weather.set_halo: 'strength' is not a number");
    expect_error("weather.set_halo{[1] = 0.5}", "weather.set_halo: the keys are names");
    expect_error("weather.set_halo(0.5)", "table expected");
    expect_error("weather.set_halo()", "table expected");
    const kit::Halo def;
    HaloRig r(R"(
function update()
    if mce.tick() == 1 then
        weather.set_halo{strength = 0.5, moon = 0.9, bogus = 2}
    end
end
)");
    r.step(2);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    CHECK(r.halo().strength == def.strength);  // the keys before the bad one were not written
    CHECK(r.halo().moon == def.moon);
    HaloRig bad(R"(
function update()
    if mce.tick() == 1 then
        weather.set_halo{strength = 0.5, moon = 'x'}
    end
end
)");
    bad.step(2);
    REQUIRE(bad.h().scripts()[0].state == ScriptState::Failed);
    CHECK(bad.halo().strength == def.strength);
    CHECK(bad.halo().moon == def.moon);
}
