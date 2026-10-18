// M-DAYNIGHT 4a: the `clock` Luau global over the kit's WorldClock. Exact numbers: a 60 s day is 3600 ticks, so the
// hour moves 1/150 per tick; a script reads the hour and day, sets the hour, pauses and changes the speed, and the
// clock follows on its next tick. A world without a clock, and bad arguments, raise a Luau error that names the
// call and disables only that script.

#include <doctest/doctest.h>

#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/world_clock.h"
#include "mce/script/compiler.h"

using namespace mce;
using script::ScriptHost;
using script::ScriptState;

namespace {

constexpr f32 kStep = 1.0f / 150.0f;  // hours per tick of a 60 s day

// A World with the world clock (hour 6, 60 s day) and one script in FixedUpdate_Post. The script's mce.tick() is 1
// on the first w.tick. `probe` is an entity whose LocalTransform the script may write.
struct CRig {
    World w;
    ScriptHost host;
    u64 tick = 0;
    Entity clock_e, probe;

    explicit CRig(const char* source, bool with_clock = true, bool install_clock = true) {
        w.register_components<LocalTransform>();
        if (install_clock) kit::install_world_clock(w, nullptr);
        INFO(source);
        // Unchecked: the error tests pass what the type check would reject; the VM must still refuse it by name.
        Result<Array<u8>> bc = script::compile_unchecked("c", source);
        REQUIRE(bc);
        REQUIRE(host.add_bytecode("c", bc.value().span()));
        host.install(w);
        probe = w.spawn_with(LocalTransform{});
        if (with_clock) {
            kit::WorldClock c;
            c.hour = 6.0f;
            c.day_length_seconds = 60.0f;
            clock_e = w.spawn_with(c);
        }
    }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
    kit::WorldClock& clk() { return *w.try_get_mut<kit::WorldClock>(clock_e); }
    const LocalTransform& p() { return *w.try_get<LocalTransform>(probe); }
    const ScriptHost& h() const { return host; }
};

std::string err(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

// A script that runs `body` once on tick 1 and must fail with `needle`.
void expect_error(const char* body, const char* needle, bool with_clock = true, bool install_clock = true) {
    const std::string src = std::string("function update()\n    if mce.tick() == 1 then\n        ") + body + "\n    end\nend\n";
    CRig r(src.c_str(), with_clock, install_clock);
    r.step(2);
    INFO(body);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    INFO(err(r.h()));
    CHECK(err(r.h()).find(needle) != std::string::npos);
}

}  // namespace

TEST_CASE("clock api: hour, day and paused read the first WorldClock") {
    // The clock runs before the script in a tick, so on script tick 150 the hour is 6 + 150/150 = 7.
    CRig r(R"(
function update()
    if mce.tick() == 150 then
        mce.each("LocalTransform", function(e)
            if mce.has(e, "WorldClock") then return end
            mce.set(e, "LocalTransform", "position", vector.create(clock.hour(), clock.day(), clock.paused() and 1 or 0))
        end)
    end
end
)");
    r.step(150);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.p().position.x == doctest::Approx(7.0f).epsilon(1e-6));
    CHECK(r.p().position.y == 0.0f);
    CHECK(r.p().position.z == 0.0f);
    CHECK(r.clk().hour == doctest::Approx(7.0f).epsilon(1e-6));
}

TEST_CASE("clock api: set moves the hour, the clock re-seats and goes on from there") {
    // Tick 3 (Post): clock.set(12). Tick 4: the clock takes 12 and advances one tick. Tick 4 script reads it back.
    CRig r(R"(
function update()
    local t = mce.tick()
    if t == 3 then clock.set(12) end
    if t == 4 then
        mce.each("LocalTransform", function(e)
            if mce.has(e, "WorldClock") then return end
            mce.set(e, "LocalTransform", "position", vector.create(clock.hour(), 0, 0))
        end)
    end
end
)");
    r.step(3);
    CHECK(r.clk().hour == 12.0f);  // the script wrote the component; the clock re-seats on tick 4
    r.step(1);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.p().position.x == doctest::Approx(12.0f + kStep).epsilon(1e-6));
    r.step(150);
    CHECK(r.clk().hour == doctest::Approx(13.0f + kStep).epsilon(1e-6));
}

TEST_CASE("clock api: set past midnight counts the day") {
    // 23.98 then two ticks of 1/150 h = 0.0133: midnight is crossed on the second tick, day 1.
    CRig r(R"(
function update()
    if mce.tick() == 1 then clock.set(23.98) end
end
)");
    r.step(2);
    CHECK(r.clk().day == 0);
    r.step(2);  // tick 2 takes 23.98 and advances to 23.98667, tick 3 reaches 24.0 -> day 1, hour 0
    CHECK(r.clk().day == 1);
    CHECK(r.clk().hour < 0.05f);
}

TEST_CASE("clock api: pause stops the clock and resume starts it again") {
    CRig r(R"(
function update()
    local t = mce.tick()
    if t == 10 then clock.pause(true) end
    if t == 40 then
        assert(clock.paused() == true)
        clock.pause(false)
        assert(clock.paused() == false)
    end
end
)");
    r.step(10);
    CHECK(r.clk().paused == 1);
    CHECK(r.clk().hour == doctest::Approx(6.0f + 10 * kStep).epsilon(1e-6));
    r.step(30);  // paused for ticks 11..40: standing still
    CHECK(r.clk().hour == doctest::Approx(6.0f + 10 * kStep).epsilon(1e-6));
    CHECK(r.clk().paused == 0);
    r.step(5);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.clk().hour == doctest::Approx(6.0f + 15 * kStep).epsilon(1e-6));
}

TEST_CASE("clock api: speed sets time_scale, clamped to 0..100") {
    CRig r(R"(
function update()
    local t = mce.tick()
    if t == 1 then clock.speed(2) end
    if t == 31 then clock.speed(500) end
    if t == 32 then clock.speed(-3) end
end
)");
    r.step(1);
    CHECK(r.clk().time_scale == 2.0f);
    r.step(30);  // tick 1 at speed 1 (6 + 1/150), ticks 2..31 at speed 2
    CHECK(r.clk().hour == doctest::Approx(6.0f + (1 + 2 * 30) * kStep).epsilon(1e-5));
    CHECK(r.clk().time_scale == 100.0f);
    r.step(1);
    CHECK(r.clk().time_scale == 0.0f);
    const f32 frozen = r.clk().hour;
    r.step(20);
    CHECK(r.clk().hour == frozen);
}

TEST_CASE("clock api: a world without a clock raises a named error") {
    expect_error("clock.hour()", "clock.hour: the scene has no WorldClock entity", false);
    expect_error("clock.day()", "clock.day: the scene has no WorldClock entity", false);
    expect_error("clock.set(3)", "clock.set: the scene has no WorldClock entity", false);
    expect_error("clock.pause(true)", "clock.pause: the scene has no WorldClock entity", false);
    expect_error("clock.speed(2)", "clock.speed: the scene has no WorldClock entity", false);
    expect_error("clock.paused()", "clock.paused: the scene has no WorldClock entity", false);
    expect_error("clock.hour()", "clock.hour: this world has no world clock", false, false);
}

TEST_CASE("clock api: bad arguments raise an error that names the call") {
    expect_error("clock.set(25)", "clock.set: the hour 25 is not a number from 0 to 24");
    expect_error("clock.set(-1)", "clock.set: the hour -1 is not a number from 0 to 24");
    expect_error("clock.set(0/0)", "clock.set: the hour");
    expect_error("clock.set('noon')", "number expected");
    expect_error("clock.pause(1)", "boolean expected");
    expect_error("clock.speed('fast')", "number expected");
    expect_error("clock.speed(0/0)", "clock.speed: the scale is not a number");
}
