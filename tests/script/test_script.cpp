// mce_script spike (ADR-0041): Luau scripts as systems over reflected components.

#include <doctest/doctest.h>

#include <cstring>

#include "mce/refl/macros.h"
#include "mce/script/compiler.h"

using namespace mce;
using mce::math::vec3;
using script::ScriptHost;
using script::ScriptState;

namespace sctest {

struct Mover {
    vec3 position;
    vec3 velocity;
    f32 bounce = 1.0f;
};
MCE_COMPONENT(Mover, position, velocity, bounce)

struct Counter {
    i32 n = 0;
    bool done = false;
    u8 pad[3]{};  // state_hash hashes column bytes: no uninitialized padding
};
MCE_COMPONENT(Counter, n, done)

}  // namespace sctest

using sctest::Counter;
using sctest::Mover;

namespace {

constexpr const char* kMover = R"(
function update()
    mce.each("Mover", function(e)
        local p = mce.get(e, "Mover", "position")
        local v = mce.get(e, "Mover", "velocity")
        p += v * mce.dt
        if p.x < 0 or p.x > 10 then
            v = vector.create(-v.x, v.y, v.z) * mce.get(e, "Mover", "bounce")
            mce.set(e, "Mover", "velocity", v)
        end
        mce.set(e, "Mover", "position", p)
    end)
end
)";

// Bytecode without the compile-time checks: tests of what the run-time sandbox stops by itself.
Result<u32> add_unchecked(ScriptHost& host, const char* name, const char* source) {
    Result<Array<u8>> bc = script::compile_unchecked(name, source);
    if (!bc) return bc.error();
    return host.add_bytecode(name, bc.value().span());
}

void spawn_movers(World& w, u32 n) {
    w.register_components<Mover, Counter>();
    for (u32 i = 0; i < n; ++i) {
        Mover m;
        m.position = {static_cast<f32>(i % 10), 0, 0};
        m.velocity = {3.0f + static_cast<f32>(i % 7), 1.0f, 0};
        m.bounce = 0.9f;
        w.spawn_with(m);
    }
}

u64 run_movers(u32 ticks) {
    World w;
    spawn_movers(w, 64);
    ScriptHost host;
    REQUIRE(script::add_source(host, "mover", kMover));
    host.install(w);
    for (u32 t = 1; t <= ticks; ++t) w.tick(1.0 / 60, t);
    CHECK(host.scripts()[0].state == ScriptState::Running);
    return w.state_hash();
}

}  // namespace

TEST_CASE("script: a Luau system moves entities through reflected fields") {
    World w;
    w.register_components<Mover, Counter>();
    Mover m;
    m.velocity = {6.0f, 0, 0};
    const Entity e = w.spawn_with(m);
    ScriptHost host;
    REQUIRE(script::add_source(host, "mover", kMover));
    host.install(w);
    for (u32 t = 1; t <= 60; ++t) w.tick(1.0 / 60, t);
    const Mover* got = w.try_get<Mover>(e);
    REQUIRE(got);
    CHECK(got->position.x == doctest::Approx(6.0f).epsilon(0.001));
    CHECK(host.scripts()[0].last_steps > 0);
}

TEST_CASE("script: the same inputs give the same state hash (FR-RUN-040)") {
    const u64 a = run_movers(600);
    const u64 b = run_movers(600);
    CHECK(a == b);
}

TEST_CASE("script: writes mark the component changed") {
    World w;
    w.register_components<Mover, Counter>();
    const Entity e = w.spawn_with(Counter{});
    ScriptHost host;
    REQUIRE(script::add_source(host, "count", R"(
function update()
    mce.each("Counter", function(e)
        mce.set(e, "Counter", "n", mce.get(e, "Counter", "n") + 1)
        mce.set(e, "Counter", "done", mce.tick() >= 3)
    end)
end
)"));
    host.install(w);
    w.tick(1.0 / 60, 1);
    const u32 before = w.change_tick();
    w.tick(1.0 / 60, 2);
    w.tick(1.0 / 60, 3);
    CHECK(w.try_get<Counter>(e)->n == 3);
    CHECK(w.try_get<Counter>(e)->done);
    CHECK(w.is_changed<Counter>(e, before));
}

TEST_CASE("script: one mce.get / mce.set call spans fields of several components (M-WORKFLOW 7)") {
    World w;
    w.register_components<Mover, Counter>();
    Mover m;
    m.velocity = vec3{1, 0, 0};
    const Entity e = w.spawn_with(Counter{}, m);
    ScriptHost host;
    REQUIRE(script::add_source(host, "mixed", R"(
function update()
    mce.each("Counter", "Mover", function(e)
        local n, v = mce.get(e, "Counter", "n", "Mover", "velocity")
        mce.set(e, "Counter", "n", n + 1, "Mover", "velocity", v * 2)
    end)
end
)"));
    host.install(w);
    w.tick(1.0 / 60, 1);
    w.tick(1.0 / 60, 2);
    CHECK(host.scripts()[0].state == ScriptState::Running);
    CHECK(w.try_get<Counter>(e)->n == 2);
    CHECK(w.try_get<Mover>(e)->velocity.x == doctest::Approx(4.0f));
}

TEST_CASE("script: the sandbox hides the clock, the OS and unseeded randomness") {
    World w;
    w.register_components<Mover, Counter>();
    w.spawn_with(Counter{});
    ScriptHost host;
    // The cook rejects these names (test_script_check.cpp); this is what the VM does on its own.
    REQUIRE(add_unchecked(host, "probe", R"(
assert(os == nil and io == nil and debug == nil and coroutine == nil)
assert(print == nil and collectgarbage == nil and getfenv == nil and setfenv == nil)
assert(math.random == nil and math.randomseed == nil)
function update()
    mce.each("Counter", function(e)
        local r = mce.random()
        assert(r >= 0 and r < 1)
        mce.set(e, "Counter", "n", math.floor(r * 1000000))
    end)
end
)"));
    host.install(w);
    w.tick(1.0 / 60, 1);
    CHECK(host.scripts()[0].state == ScriptState::Running);
    // mce.random is a hash of (seed, tick, script, call): a second world sees the same numbers.
    World w2;
    w2.register_components<Mover, Counter>();
    w2.spawn_with(Counter{});
    ScriptHost host2;
    REQUIRE(script::add_source(host2, "probe", R"(
function update()
    mce.each("Counter", function(e) mce.set(e, "Counter", "n", math.floor(mce.random() * 1000000)) end)
end
)"));
    host2.install(w2);
    w2.tick(1.0 / 60, 1);
    CHECK(w.state_hash() == w2.state_hash());
}

TEST_CASE("script: globals and built-in libraries are read-only") {
    World w;
    w.register_components<Mover, Counter>();
    ScriptHost host;
    REQUIRE(add_unchecked(host, "globals", "function update() leaked = 1 end"));
    REQUIRE(add_unchecked(host, "stdlib", "function update() string.len = nil end"));
    REQUIRE(add_unchecked(host, "api", "function update() mce.get = nil end"));
    host.install(w);
    w.tick(1.0 / 60, 1);
    for (const auto& s : host.scripts()) {
        CHECK(s.state == ScriptState::Failed);
        CHECK(std::strstr(s.error.c_str(), "readonly") != nullptr);
    }
}

TEST_CASE("script: the step budget stops a runaway script, the others keep running") {
    World w;
    w.register_components<Mover, Counter>();
    const Entity e = w.spawn_with(Counter{});
    script::ScriptBudget budget;
    budget.steps_per_tick = 10000;
    ScriptHost host(budget);
    REQUIRE(script::add_source(host, "spin", "function update() while true do end end"));
    REQUIRE(script::add_source(host, "count", R"(
function update()
    mce.each("Counter", function(e) mce.set(e, "Counter", "n", mce.get(e, "Counter", "n") + 1) end)
end
)"));
    host.install(w);
    w.tick(1.0 / 60, 1);
    w.tick(1.0 / 60, 2);
    CHECK(host.scripts()[0].state == ScriptState::Failed);
    CHECK(std::strstr(host.scripts()[0].error.c_str(), "step budget") != nullptr);
    CHECK(host.scripts()[1].state == ScriptState::Running);
    CHECK(w.try_get<Counter>(e)->n == 2);
}

TEST_CASE("script: the memory cap stops an allocating script") {
    World w;
    w.register_components<Mover, Counter>();
    script::ScriptBudget budget;
    budget.memory_bytes = 2u * 1024u * 1024u;
    budget.steps_per_tick = 100000000;
    ScriptHost host(budget);
    REQUIRE(script::add_source(host, "hog", R"(
function update()
    local t = {}
    for i = 1, 10000000 do t[i] = i end
end
)"));
    host.install(w);
    w.tick(1.0 / 60, 1);
    CHECK(host.scripts()[0].state == ScriptState::Failed);
    CHECK(std::strstr(host.scripts()[0].error.c_str(), "memory") != nullptr);
    CHECK(host.memory_used() <= budget.memory_bytes);
}

TEST_CASE("script: errors name the script and the line") {
    auto bad = script::compile("broken", "function update()\n  local x = = 1\nend");
    REQUIRE(!bad);
    CHECK(bad.error().code == Errc::ParseError);
    CHECK(std::strstr(bad.error().path.c_str(), "broken:2:") != nullptr);

    World w;
    w.register_components<Mover, Counter>();
    w.spawn_with(Counter{});
    ScriptHost host;
    // Literal names fail the compile (test_script_check.cpp); these are the run-time errors.
    REQUIRE(add_unchecked(host, "typo", "function update()\n  mce.each(\"Countr\", function(e) end)\nend"));
    REQUIRE(add_unchecked(host,
        "field", "function update()\n  mce.each(\"Counter\", function(e)\n mce.get(e, \"Counter\", \"m\") end)\nend"));
    REQUIRE(!script::add_source(host, "noupdate", "local x = 1"));
    host.install(w);
    w.tick(1.0 / 60, 1);
    CHECK(std::strstr(host.scripts()[0].error.c_str(), "typo:2:") != nullptr);
    CHECK(std::strstr(host.scripts()[0].error.c_str(), "unknown component 'Countr'") != nullptr);
    CHECK(std::strstr(host.scripts()[1].error.c_str(), "has no field 'm'") != nullptr);
    CHECK(host.scripts()[2].state == ScriptState::Failed);
}
