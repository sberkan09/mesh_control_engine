#pragma once
// mce_script (ADR-0041, M-SCRIPT): Luau scripts as ECS systems.
//
// A ScriptHost owns one sandboxed Luau VM. Each script is loaded from bytecode (mce_cook writes it
// into CHNK_SCRIPT; mce/script/compiler.h compiles source for tools and tests, and the player never
// links the compiler) and its top level runs once in its own environment; it must define
// `function update()`. install() registers one system in a phase that calls every script's
// update(), in the order the scripts were added, once per tick. Scenes name their scripts with
// kit::ScriptSystem components, which mce/script/scene_scripts.h runs in the stock hosts.
//
// The rule that keeps determinism, saves and replays intact: scripts hold no state between ticks.
// Everything a script remembers lives in reflected components it reads and writes through the
// `mce` table, so World::state_hash covers it. Globals are read-only after the top level ran
// (writes fail); state in upvalues is not caught yet (ADR-0041, open question 2).
//
// The script API (global `mce`, read-only):
//   mce.each("CompA", "CompB", ..., fn)   calls fn(e) for every entity with all of them, in table
//                                         then row order (the same order a Query visits)
//   mce.get(e, "Comp", "field")           number, boolean or vector (Vec2/Vec3); Vec4/Quat/Color
//                                         come back as four numbers
//   mce.set(e, "Comp", "field", v...)     writes the field and marks the component changed
//   mce.dt, mce.tick()                    fixed step and the current tick
//   mce.random()                          [0, 1): a hash of (seed, tick, script, call index), so
//                                         it carries no state between ticks
//   mce.log(msg)                          engine log, info level
//
// Sandbox: base, table, string, math, utf8, bit32, buffer and vector libraries only (no os, io,
// debug, coroutine, print, collectgarbage, getfenv/setfenv, math.random); every script gets a
// step budget per tick (interrupts at calls and loop back edges, which are deterministic) and the
// VM a memory cap. A script that errors or exceeds its budget is disabled and its error kept.

#include "mce/ecs/world.h"
#include "mce/result.h"

struct lua_State;

namespace mce::script {

struct HostAccess;

struct ScriptBudget {
    u32 steps_per_tick = 200000;               // per script and tick; exceeding it disables the script
    usize memory_bytes = 32u * 1024u * 1024u;  // the whole VM
};

enum class ScriptState : u8 { Running, Failed };

struct ScriptInfo {
    FixedString<64> name;
    ScriptState state = ScriptState::Running;
    u32 last_steps = 0;      // interrupts counted in the last update()
    FixedString<256> error;  // "<name>:<line>: message" once Failed
};

class ScriptHost {
public:
    explicit ScriptHost(const ScriptBudget& budget = {}, u64 seed = 0x5C121D7ull);
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // Adds a script from bytecode; its top level runs now (with the step budget). Returns its
    // index. `group` picks which run(f, group) calls it (scene scripts: one group per phase).
    Result<u32> add_bytecode(StringView name, Span<const u8> bytecode, u8 group = 0);

    // Registers the "script.update" system in `phase` and points the World at this host. The host
    // must outlive every tick of `w`. One host per World.
    void install(World& w, Phase phase = Phase::FixedUpdate_Post);

    // Runs every running script's update() once (what the installed system does).
    void run(Frame& f);
    // Runs the running scripts of one group, in the order they were added.
    void run(Frame& f, u8 group);

    Span<const ScriptInfo> scripts() const { return infos_.span(); }
    usize memory_used() const { return mem_used_; }

private:
    friend struct HostAccess;
    static void* alloc_fn(void* ud, void* ptr, usize osize, usize nsize);
    static void interrupt_fn(lua_State* L, int gc);
    void fail(u32 index, const char* message);
    void run_scripts(Frame& f, bool all, u8 group);

    lua_State* L_ = nullptr;
    ScriptBudget budget_;
    u64 seed_;
    usize mem_used_ = 0;
    Array<ScriptInfo> infos_;
    Array<int> update_refs_;  // lua_ref of each script's update function
    Array<u8> groups_;
    // Per-call context the C functions read (valid only inside run()).
    Frame* frame_ = nullptr;
    u32 current_ = 0;
    u32 steps_ = 0;
    u32 random_calls_ = 0;
};

}  // namespace mce::script
