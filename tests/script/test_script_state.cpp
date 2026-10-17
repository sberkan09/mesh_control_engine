// M-SCRIPT 3: no hidden state. The compiler rejects state kept outside components (writes from a
// function into top-level locals and their tables) and object keys it can see; pairs/next refuse
// object keys at run time. A script that passes keeps the state hash across a reload.

#include <doctest/doctest.h>

#include <string>

#include "mce/refl/macros.h"
#include "mce/script/compiler.h"

using namespace mce;
using script::ScriptHost;
using script::ScriptState;

namespace scstate {

struct Score {
    i32 points = 0;
    i32 combo = 0;
};
MCE_COMPONENT(Score, points, combo)

}  // namespace scstate

using scstate::Score;

namespace {

struct Rejected {
    std::string message;
    u32 line = 0;
    u32 col = 0;
};

// The compile error of `source`, or an empty message when it compiles.
Rejected compile_error(const char* source) {
    script::CompileError e;
    Result<Array<u8>> r = script::compile("s", source, &e);
    if (r) return {};
    return Rejected{e.message.c_str(), e.line, e.col};
}

bool compiles(const char* source) {
    const Rejected r = compile_error(source);
    INFO(source << "\n-> " << r.message);
    return r.message.empty();
}

}  // namespace

TEST_CASE("script state: a function may not assign a top-level local") {
    const Rejected r = compile_error(R"(local count = 0
function update()
    count += 1
end
)");
    CHECK(r.line == 3);
    CHECK(r.col == 5);
    CHECK(r.message.find("assigns top-level local 'count'") != std::string::npos);

    Result<Array<u8>> full = script::compile("scripts/count.luau", "local n = 0\nfunction update() n = n + 1 end\n");
    REQUIRE_FALSE(full);
    CHECK(std::string(full.error().path.c_str()).rfind("scripts/count.luau:2:19: ", 0) == 0);
}

TEST_CASE("script state: a function may not change a table a top-level local holds") {
    CHECK(compile_error("local seen = {}\nfunction update() seen.x = 1 end").message.find("writes into the table in top-level local 'seen'") !=
          std::string::npos);
    CHECK_FALSE(compiles("local seen = {}\nfunction update() seen[mce.tick()] = true end"));
    CHECK_FALSE(compiles("local t = {a = {}}\nfunction update() t.a.b = 1 end"));
    CHECK_FALSE(compiles("local log = {}\nfunction update() table.insert(log, 1) end"));
    CHECK_FALSE(compiles("local log = {}\nfunction update() table.clear(log) end"));
    CHECK_FALSE(compiles("local a, b = {}, {}\nfunction update() table.move(a, 1, 1, 1, b) end"));
    CHECK_FALSE(compiles("local t = {}\nfunction update() rawset(t, 'k', 1) end"));
    CHECK_FALSE(compiles("local t = {}\nfunction update() setmetatable(t, {}) end"));
    CHECK_FALSE(compiles("local b = buffer.create(4)\nfunction update() buffer.writei32(b, 0, 1) end"));
    CHECK_FALSE(compiles("local M = {}\nfunction update() function M.f() end end"));
    CHECK(compile_error("local log = {}\nfunction update() table.insert(log, 1) end").message.find("changes the table in top-level local 'log'") !=
          std::string::npos);
}

TEST_CASE("script state: what stays allowed") {
    // Top-level code runs once, at load: constants, handles, tables it fills.
    CHECK(compiles(R"(
local LIMITS = { max = 3 }
LIMITS.min = 1
table.insert(LIMITS, 5)
local P = mce.field("Score", "points")
function update()
    -- locals of update() live for one call, so closures may change them
    local total = 0
    mce.each("Score", function(e) total += mce.get(e, P) end)
    local scratch = {}
    table.insert(scratch, total)
    scratch.n = #scratch
    local LIMITS = { max = 1 }  -- a shadowing local is not the top-level one
    LIMITS.max = 2
    for k, v in pairs(LIMITS) do total += v end
end
)"));
    // A function parameter that happens to be passed a top-level table is not tracked (no types yet).
    CHECK(compiles("local t = {}\nlocal function fill(x) x.a = 1 end\nfunction update() local u = {} fill(u) end"));
}

TEST_CASE("script state: object keys the source shows are rejected") {
    CHECK(compile_error("local t = { [{}] = 1 }\nfunction update() end").message.find("table or function used as a table key") !=
          std::string::npos);
    CHECK_FALSE(compiles("local k = {}\nlocal t = { [k] = 1 }\nfunction update() end"));
    CHECK_FALSE(compiles("local function f() end\nfunction update() local seen = {} seen[f] = true end"));
    CHECK_FALSE(compiles("function update() local seen = {} seen[function() end] = 1 end"));
    CHECK(compiles("function update() local seen = {} seen[mce.tick()] = 1 seen['a'] = 2 seen[vector.create(1, 2, 3)] = 3 end"));
}

TEST_CASE("script state: pairs and next refuse object keys at run time") {
    World w;
    w.register_components<Score>();
    const Entity e = w.spawn_with(Score{});
    ScriptHost host;
    REQUIRE(script::add_source(host, "ok", R"(
function update()
    local t = { a = 1, b = 2, [3] = 4 }
    local sum = 0
    for k, v in pairs(t) do sum += v end
    local k, v = next(t)
    assert(k ~= nil)
    for k, v in pairs({}) do error("empty") end
    mce.each("Score", function(e) mce.set(e, "Score", "points", sum) end)
end
)"));
    REQUIRE(script::add_source(host, "keyed", R"(
local function make() return {} end
function update()
    local t = {}
    t[make()] = 1  -- a key the compiler cannot see is a table
    for k, v in pairs(t) do end
end
)"));
    host.install(w);
    w.tick(1.0 / 60, 1);
    CHECK(host.scripts()[0].state == ScriptState::Running);
    CHECK(w.try_get<Score>(e)->points == 7);
    REQUIRE(host.scripts()[1].state == ScriptState::Failed);
    CHECK(std::string(host.scripts()[1].error.c_str()).find("pairs over a table keyed by a table") != std::string::npos);
}

TEST_CASE("script state: a reload between two ticks keeps the hash") {
    constexpr const char* kScore = R"(
local POINTS = mce.field("Score", "points")
local COMBO = mce.field("Score", "combo")
function update()
    mce.each("Score", function(e)
        local p, c = mce.get(e, POINTS, COMBO)
        if mce.random() < 0.5 then c += 1 else c = 0 end
        mce.set(e, POINTS, p + c, COMBO, c)
    end)
end
)";
    auto run = [&](u32 reload_at) {
        World w;
        w.register_components<Score>();
        for (i32 i = 0; i < 8; ++i) w.spawn_with(Score{i, 0});
        ScriptHost first;
        ScriptHost second;
        static ScriptHost* host = nullptr;
        host = &first;
        REQUIRE(script::add_source(*host, "score", kScore));
        w.add_system(Phase::FixedUpdate_Post, "score", [](Frame& f) { host->run(f); });
        for (u32 t = 1; t <= 40; ++t) {
            if (t == reload_at) {  // what a hot reload or loading a save does: a fresh VM
                host = &second;
                REQUIRE(script::add_source(*host, "score", kScore));
            }
            w.tick(1.0 / 60, t);
        }
        CHECK(host->scripts()[0].state == ScriptState::Running);
        return w.state_hash();
    };
    const u64 straight = run(0);
    CHECK(run(17) == straight);
    CHECK(run(2) == straight);
}

// M-SCRIPT3 step 2: what the compiler cannot see, the host catches at run time. After the top level
// every table update() reaches is frozen (a write fails at its line), and its upvalues and buffers
// are compared after each update().

namespace {

struct Hidden {
    const char* name;
    const char* source;
    const char* error;  // the start of the script's error after the first tick
};

// Loads `source` (which must pass the static check) next to an entity with a Score, ticks once.
std::string first_tick_error(const char* name, const char* source, const script::ScriptBudget& budget = {}, u32 ticks = 1) {
    World w;
    w.register_components<Score>();
    w.spawn_with(Score{});
    ScriptHost host(budget);
    Result<u32> r = script::add_source(host, name, source);
    REQUIRE_MESSAGE(r, (r ? "" : r.error().path.c_str()));
    REQUIRE(host.scripts()[0].state == ScriptState::Running);  // loading is fine: the change comes later
    host.install(w);
    for (u32 t = 1; t <= ticks; ++t) w.tick(1.0 / 60, t);
    return host.scripts()[0].state == ScriptState::Failed ? std::string(host.scripts()[0].error.c_str()) : std::string();
}

const Hidden kHidden[] = {
    {"alias", R"(local t = { n = 0 }
function update()
    local a = t
    a.n += 1
end
)",
     "alias:4: attempt to modify a readonly table (tables reachable from update() are frozen after the top level; keep state in a "
     "component)"},
    {"param", R"(local t = { n = 0 }
local function fill(x) x.n += 1 end
function update() fill(t) end
)",
     "param:2: attempt to modify a readonly table (tables reachable"},
    {"factory", R"(local function counter()
    local n = 0
    return function() n += 1 return n end
end
local inc = counter()
local P = mce.field("Score", "points")
function update()
    local v = inc()
    mce.each("Score", function(e) mce.set(e, P, v) end)
end
)",
     "factory:3: update() kept state between ticks: upvalue 'n' of the function defined at line 3 changed (reached from upvalue 'inc' of "
     "update()); keep it in a component"},
    {"global", R"(STATE = { n = 0 }
function update() STATE.n += 1 end
)",
     "global:2: attempt to modify a readonly table (tables reachable"},
    {"proxy", R"(local P = newproxy(true)
function update()
    local mt = getmetatable(P)
    mt.n = (mt.n or 0) + 1
end
)",
     "proxy:4: attempt to modify a readonly table (tables reachable"},
    {"buffer", R"(B = buffer.create(4)
function update() buffer.writei32(B, 0, mce.tick()) end
)",
     "buffer:2: update() kept state between ticks: a buffer reached from global 'B' changed; keep it in a component"},
    {"gmatch", R"(local words = string.gmatch("a b c d", "%a")
function update() local w = words() end
)",
     "gmatch:2: update() kept state between ticks: upvalue #3 of a library function changed (reached from upvalue 'words' of update())"},
};

}  // namespace

TEST_CASE("script state: hidden state the compiler misses fails the script on the tick it happens") {
    for (const Hidden& h : kHidden) {
        INFO(h.name);
        CHECK(compiles(h.source));  // the static check does not see these
        const std::string error = first_tick_error(h.name, h.source);
        CHECK_MESSAGE(error.rfind(h.error, 0) == 0, error);
    }
}

TEST_CASE("script state: check_state = false lets the same scripts run") {
    script::ScriptBudget off;
    off.check_state = false;
    for (const Hidden& h : kHidden) {
        INFO(h.name);
        CHECK(first_tick_error(h.name, h.source, off, 3).empty());
    }
}

TEST_CASE("script state: the run-time check leaves state kept in components alone") {
    // Constant top-level tables and functions read, locals and closures inside update(), handles,
    // mce.random, mce.dt changing every tick, a proxy made per call: 100 ticks Running.
    constexpr const char* kAllowed = R"(
local LIMITS = { max = 3, names = { "a", "b" } }
LIMITS.min = 1
NAMES = { "x", "y" }
local P = mce.field("Score", "points")
local C = mce.field("Score", "combo")
local SPAWN = vector.create(1, 2, 3)
local NAN = 0 / 0
local function clamp(x) return math.min(x, LIMITS.max) end
function update()
    local total = 0
    mce.each("Score", function(e) total += mce.get(e, P) end)
    local scratch = {}
    table.insert(scratch, total)
    local mt = getmetatable(newproxy(true))
    mt.seen = true
    for i, n in ipairs(NAMES) do scratch[n] = i end
    assert(NAN ~= NAN and SPAWN.x == 1)
    mce.each("Score", function(e)
        local p = mce.get(e, P)
        local step = clamp(#LIMITS.names + #NAMES) + (mce.random() < 0.5 and 1 or 0)
        mce.set(e, P, p + step, C, math.floor(mce.dt * 1000 + 0.5))
    end)
end
)";
    auto run = [&](bool check) {
        World w;
        w.register_components<Score>();
        const Entity e = w.spawn_with(Score{});
        script::ScriptBudget budget;
        budget.check_state = check;
        ScriptHost host(budget);
        REQUIRE(compiles(kAllowed));
        REQUIRE(script::add_source(host, "allowed", kAllowed));
        // A second script loaded after the walk froze the first one's tables still works.
        REQUIRE(script::add_source(host, "second", "local K = { 2 }\nfunction update() assert(K[1] == 2) end\n"));
        host.install(w);
        for (u32 t = 1; t <= 100; ++t) w.tick(t % 2 ? 1.0 / 60 : 1.0 / 30, t);
        for (const script::ScriptInfo& info : host.scripts()) {
            INFO(info.error.c_str());
            CHECK(info.state == ScriptState::Running);
        }
        CHECK(w.try_get<Score>(e)->combo == 33);  // mce.dt of the last tick: the host still writes mce
        return w.state_hash();
    };
    CHECK(run(true) == run(false));
}

TEST_CASE("script state: the state check fails a load it cannot watch") {
    ScriptHost host;
    Result<u32> r = script::add_source(host, "big", "B = buffer.create(300000)\nfunction update() end\n");
    REQUIRE_FALSE(r);
    REQUIRE(host.scripts()[0].state == ScriptState::Failed);
    CHECK(std::string(host.scripts()[0].error.c_str()).find("big: state check: the buffers update() reaches hold more than 256 KiB") == 0);
    // The host carries on: the next script loads and is watched.
    CHECK(script::add_source(host, "next", "local t = {}\nfunction update() end\n"));
    CHECK(host.scripts()[1].state == ScriptState::Running);
}

TEST_CASE("script state: each script is watched on its own (several scripts, one fails)") {
    ScriptHost host;
    // 0: stateless; 1: hidden state in an upvalue; 2: stateless again, loaded after the failing one; NaN as a real upvalue.
    REQUIRE(script::add_source(host, "calm", "local K = { 1 }\nfunction update() assert(K[1] == 1) end\n"));
    REQUIRE(script::add_source(host, "counter",
                               "local function make() local n = 0 return function() n += 1 return n end end\n"
                               "local inc = make()\nfunction update() inc() end\n"));
    REQUIRE(script::add_source(host, "nan",
                               "local function make() local x = 0 / 0 return function() return x end end\n"
                               "local get = make()\nfunction update() assert(get() ~= get()) end\n"));
    World w;
    host.install(w);
    for (u32 t = 1; t <= 5; ++t) w.tick(1.0 / 60, t);
    CHECK(host.scripts()[0].state == ScriptState::Running);  // another script's watch is not charged to it
    CHECK(host.scripts()[1].state == ScriptState::Failed);
    CHECK(std::string(host.scripts()[1].error.c_str()).find("kept state between ticks") != std::string::npos);
    CHECK(host.scripts()[2].state == ScriptState::Running);  // a NaN upvalue is equal to itself, bit for bit
}
