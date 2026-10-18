// M-WEATHER 4a: the `weather` Luau global over the kit's Weather. Exact numbers: a 10 s transition is 600 ticks; a
// script that calls weather.set('storm') on its tick 1 (after the weather system of that tick) sees the kind switch
// on tick 2 and the intensity (N - 1) / 600 after N ticks. A world without a Weather, an unknown kind and bad
// arguments raise a Luau error that names the call and disables only that script.

#include <doctest/doctest.h>

#include <bit>
#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/weather.h"
#include "mce/script/compiler.h"

using namespace mce;
using script::ScriptHost;
using script::ScriptState;

namespace {

// A World with the weather system (and a Weather, 10 s transitions) and one script in FixedUpdate_Post. `probe` is an
// entity whose LocalTransform the script may write.
struct WRig {
    World w;
    ScriptHost host;
    u64 tick = 0;
    Entity wx_e, probe;

    explicit WRig(const char* source, bool with_weather = true, bool install = true) {
        w.register_components<LocalTransform>();
        if (install) kit::install_weather(w, nullptr);
        INFO(source);
        // Unchecked: the error tests pass what the type check would reject; the VM must still refuse it by name.
        Result<Array<u8>> bc = script::compile_unchecked("w", source);
        REQUIRE(bc);
        REQUIRE(host.add_bytecode("w", bc.value().span()));
        host.install(w);
        probe = w.spawn_with(LocalTransform{});
        if (with_weather) {
            kit::Weather c;
            c.transition_seconds = 10.0f;
            wx_e = w.spawn_with(c);
        }
    }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
    kit::Weather& wx() { return *w.try_get_mut<kit::Weather>(wx_e); }
    const LocalTransform& p() { return *w.try_get<LocalTransform>(probe); }
    const ScriptHost& h() const { return host; }
};

std::string err(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

// A script that runs `body` once on tick 1 and must fail with `needle`.
void expect_error(const char* body, const char* needle, bool with_weather = true, bool install = true) {
    const std::string src = std::string("function update()\n    if mce.tick() == 1 then\n        ") + body + "\n    end\nend\n";
    WRig r(src.c_str(), with_weather, install);
    r.step(2);
    INFO(body);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    INFO(err(r.h()));
    CHECK(err(r.h()).find(needle) != std::string::npos);
}

u32 bits(f32 v) { return std::bit_cast<u32>(v); }

}  // namespace

TEST_CASE("weather api: set storm, the kinds and the numbers at pinned ticks") {
    WRig r(R"(
function update()
    local t = mce.tick()
    if t == 1 then
        assert(weather.kind() == 'clear' and weather.target() == 'clear')
        assert(weather.intensity() == 0 and weather.wetness() == 0)
        weather.set('storm')
        assert(weather.target() == 'storm' and weather.kind() == 'clear')
    end
    if t == 301 then
        assert(weather.kind() == 'storm' and weather.target() == 'storm')
        mce.each("LocalTransform", function(e)
            mce.set(e, "LocalTransform", "position", vector.create(weather.intensity(), weather.wind(), weather.wetness()))
        end)
    end
end
)");
    r.step(1);
    CHECK(r.wx().target == kit::WeatherKind::Storm);
    CHECK(r.wx().current == kit::WeatherKind::Clear);
    CHECK(r.wx().transition_seconds == 10.0f);  // set without seconds leaves it alone
    r.step(300);                                // N = 301: fade 300 of 600
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.wx().current == kit::WeatherKind::Storm);
    CHECK(bits(r.wx().intensity) == bits(0.5f));
    CHECK(bits(r.p().position.x) == bits(0.5f));
    CHECK(r.p().position.y == doctest::Approx(0.55f).epsilon(1e-6));  // 0.1 + (1.0 - 0.1) * 0.5
    CHECK(bits(r.p().position.y) == bits(r.wx().wind));
    CHECK(r.p().position.z > 0.0f);
    CHECK(bits(r.p().position.z) == bits(r.wx().wetness));
    r.step(300);  // N = 601: full
    CHECK(bits(r.wx().intensity) == bits(1.0f));
    CHECK(r.wx().wind == doctest::Approx(1.0f).epsilon(1e-6));
}

TEST_CASE("weather api: set with seconds writes the transition, and a change between kinds goes through clear") {
    WRig r(R"(
function update()
    local t = mce.tick()
    if t == 1 then weather.set('rain', 2) end
    if t == 121 then
        assert(weather.kind() == 'rain' and weather.intensity() == 1)
        weather.set('snow', 1)
    end
    if t == 122 then assert(weather.kind() == 'rain' and weather.target() == 'snow') end
    if t == 300 then assert(weather.kind() == 'snow') end
end
)");
    r.step(1);
    CHECK(r.wx().transition_seconds == 2.0f);
    r.step(120);  // N = 121: 2 s is 120 ticks, rain from tick 2 reaches 1 on tick 121
    CHECK(bits(r.wx().intensity) == bits(1.0f));
    CHECK(r.wx().transition_seconds == 1.0f);
    r.step(179);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.wx().current == kit::WeatherKind::Snow);
}

TEST_CASE("weather api: every kind name round-trips") {
    WRig r(R"(
function update()
    local t = mce.tick()
    if t == 1 then weather.set('overcast', 0.1) end
    if t == 3 then assert(weather.kind() == 'overcast') weather.set('rain') end
    if t == 7 then assert(weather.kind() == 'rain') weather.set('storm') end
    if t == 11 then assert(weather.kind() == 'storm') weather.set('snow') end
    if t == 15 then assert(weather.kind() == 'snow') weather.set('clear') end
    if t == 20 then assert(weather.kind() == 'clear' and weather.target() == 'clear') end
end
)");
    // transition 0.1 s = 6 ticks: tick 2 switches to overcast (fade 1 of 6) ... the asserts only need the switch.
    r.step(20);
    INFO(err(r.h()));
    CHECK(r.h().scripts()[0].state == ScriptState::Running);
}

TEST_CASE("weather api: a world without a Weather raises a named error") {
    expect_error("weather.kind()", "weather.kind: the scene has no Weather entity", false);
    expect_error("weather.target()", "weather.target: the scene has no Weather entity", false);
    expect_error("weather.set('rain')", "weather.set: the scene has no Weather entity", false);
    expect_error("weather.intensity()", "weather.intensity: the scene has no Weather entity", false);
    expect_error("weather.wind()", "weather.wind: the scene has no Weather entity", false);
    expect_error("weather.wetness()", "weather.wetness: the scene has no Weather entity", false);
    expect_error("weather.kind()", "weather.kind: this world has no weather", false, false);
}

TEST_CASE("weather api: bad arguments raise an error that names the call") {
    expect_error("weather.set('hail')", "weather.set: unknown weather 'hail'");
    expect_error("weather.set('')", "weather.set: unknown weather ''");
    expect_error("weather.set('Rain')", "weather.set: unknown weather 'Rain'");
    expect_error("weather.set('rain', 0/0)", "weather.set: the transition seconds are not a number");
    expect_error("weather.set(3)", "weather.set: unknown weather '3'");
    expect_error("weather.set('rain', 'slow')", "number expected");
    expect_error("weather.set()", "string expected");
}
