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
