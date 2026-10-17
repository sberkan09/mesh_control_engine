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
// (writes fail). With ScriptBudget::check_state, every table reachable from update() and the
// script's globals is frozen too, and the upvalues and buffers it reaches are compared after each
// update(): a change fails the script (M-SCRIPT3 step 2).
//
// The script API (global `mce`, read-only; docs/user-guide/27-scripting.md has the full table):
//   fields     mce.field("Comp", "field") -> an opaque handle, resolved once (call it at the top
//              level); mce.get(e, h, ...) -> one value per field, mce.set(e, h, v, ...) writes
//              several fields in one call; ("Comp", "field") pairs work in place of handles.
//              Numbers, booleans, enums (by name), Vec2/Vec3 (vectors), Vec4/Quat/Color (four
//              numbers), entities, strings, asset refs ("guid:...") and nested structs (tables).
//   entities   mce.each(...), mce.has(e, "Comp"), mce.alive(e), mce.spawn({Comp = {...}}),
//              mce.add(e, "Comp", {...}), mce.remove(e, "Comp"), mce.destroy(e),
//              mce.instantiate("guid:<prefab>", position): structural changes go through Commands
//              and appear at the next sync point; the handles are valid at once.
//   events     mce.events("TriggerEnter") -> array of tables; mce.emit("Type", {...}) for any
//              MCE_EVENT type; mce.signal(name, e) / mce.signals(name) for the kit's named signals
//              (sequences, timelines, behavior trees); mce.play_sound("guid:...", gain_db).
//   input      mce.pressed / held / released(action), mce.axis(action), mce.axis2(action).
//   world      mce.dt, mce.tick(), mce.random(), mce.log(msg), mce.load_scene(guid_or_name),
//              mce.quit().
//   physics    mce.raycast2d(origin, dir, max) / mce.raycast3d(...) -> hit table or nil,
//              mce.overlap2d(min, max) -> entities, mce.move_and_collide(e, delta) -> result,
//              mce.teleport2d(e, position), mce.velocity2d(e), mce.set_velocity2d(e, v), mce.impulse2d(e, v, point?),
//              mce.force2d, mce.(set_)angular_velocity2d, mce.(set_)awake2d, mce.set_enabled2d,
//              mce.overlap_point2d, mce.raycast_all2d, mce.circle_cast2d / box_cast2d,
//              mce.find_path(from, to, options?) -> array of vectors or nil (a grid over the static colliders),
//              mce.save / load / has_save, mce.(set_)time_scale, mce.shake, mce.burst,
//              mce.overlap_sphere3d / overlap_box3d / sphere_cast3d, mce.(set_)velocity3d,
//              mce.(set_)angular_velocity3d, mce.impulse3d, mce.teleport3d(e, position, rotation?),
//              mce.(set_)rotation3d, mce.look_rotation3d (Euler degrees), mce.find_path3d (the baked navmesh).
//
// Sandbox: base, table, string, math, utf8, bit32, buffer and vector libraries only (no os, io,
// debug, coroutine, print, collectgarbage, getfenv/setfenv, math.random); every script gets a
// step budget per tick (interrupts at calls and loop back edges, which are deterministic) and the
// VM a memory cap. A script that errors or exceeds its budget is disabled and its error kept.

#include "mce/ecs/world.h"
#include "mce/refl/type.h"
#include "mce/result.h"

struct lua_State;

namespace mce::asset {
class AssetSystem;
}

namespace mce::script {

struct HostAccess;

struct ScriptBudget {
    u32 steps_per_tick = 200000;               // per script and tick; exceeding it disables the script
    usize memory_bytes = 32u * 1024u * 1024u;  // the whole VM
    bool check_state = true;                   // freeze what update() reaches; fail a script whose upvalues change
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
    // Bytecode is trusted: Luau's loader does not validate it. Callers pass only bytecode the game's own build
    // cooked (AssetSystem drops Script entries from mod packs); anything else goes through compile().
    Result<u32> add_bytecode(StringView name, Span<const u8> bytecode, u8 group = 0);

    // Registers the "script.update" system in `phase` and points the World at this host. The host
    // must outlive every tick of `w`. One host per World.
    void install(World& w, Phase phase = Phase::FixedUpdate_Post);

    // Runs every running script's update() once (what the installed system does).
    void run(Frame& f);
    // Runs the running scripts of one group, in the order they were added.
    void run(Frame& f, u8 group);

    // Where mce.instantiate and mce.play_sound find assets (null: they fail with an error).
    void set_assets(asset::AssetSystem* assets) { assets_ = assets; }

    Span<const ScriptInfo> scripts() const { return infos_.span(); }
    usize memory_used() const { return mem_used_; }

private:
    friend struct HostAccess;
    static void* alloc_fn(void* ud, void* ptr, usize osize, usize nsize);
    static void interrupt_fn(lua_State* L, int gc);
    void fail(u32 index, const char* message);
    void run_scripts(Frame& f, bool all, u8 group);
    bool watch_state(u32 index);      // after the top level: freeze and record the watch list
    bool state_unchanged(u32 index);  // after update(): compare the watch list (fails the script)
    static void crash_context(void* user, void (*put)(void* sink, const char* text), void* sink);

    lua_State* L_ = nullptr;
    ScriptBudget budget_;
    u64 seed_;
    usize mem_used_ = 0;
    Array<ScriptInfo> infos_;
    Array<int> update_refs_;  // lua_ref of each script's update function
    Array<u8> groups_;
    // The state check's watch list: an upvalue slot or a buffer reachable from a script, with its
    // value after the top level (script.cpp, watch_state). Script i owns watches_[watch_first_[i]..
    // watch_first_[i + 1]) (the last one runs to the end).
    struct StateWatch {
        int holder_ref = -1;     // lua_ref of the function (upvalue) or the buffer
        int base_ref = -1;       // lua_ref of the upvalue's value, unless a number or vector (bits)
        u64 bits[2] = {};        // number / vector bits; a buffer's offset and size in watch_bytes_
        u32 root = 0;            // index into watch_roots_: where the walk found it
        i32 line = 0;            // line the holding function is defined at
        u16 slot = 0;            // upvalue index (1-based); 0 for a buffer
        u8 type = 0;             // lua type of the baseline value
        bool of_update = false;  // an upvalue of update() itself
        bool of_c = false;       // an upvalue of a C closure (string.gmatch's iterator)
    };
    Array<StateWatch> watches_;
    Array<u32> watch_first_;
    Array<u8> watch_bytes_;               // buffer baselines
    Array<FixedString<64>> watch_roots_;  // "global 'STATE'", "upvalue #2 of update()"
    asset::AssetSystem* assets_ = nullptr;
    // mce.field handles: index + 1 is the handle a script holds.
    struct FieldHandle {
        const refl::TypeRecord* type = nullptr;
        const refl::FieldRecord* field = nullptr;
        const World* world = nullptr;  // the World `comp` was resolved in
        ecs::ComponentId comp = ecs::kInvalidComponent;
    };
    Array<FieldHandle> handles_;
    Array<u8> scratch_;  // component bytes for spawn/add/emit (no destructors across Luau errors)
    Array<Entity> entity_scratch_;  // query and instantiate results, for the same reason
    // Per-call context the C functions read (valid only inside run()).
    Frame* frame_ = nullptr;
    u32 current_ = 0;
    u32 steps_ = 0;
    u32 random_calls_ = 0;
    bool in_update_ = false;  // a script's update() is on the stack (the crash report writes its stack)
};

}  // namespace mce::script
