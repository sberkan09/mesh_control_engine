// mce_script (ADR-0041, M-SCRIPT): the Luau host. See mce/script/script.h for the contract.
//
// Every lua_CFunction here may longjmp out (LUA_USE_LONGJMP): no object with a destructor lives
// across a call that can raise (luaL_check*, luaL_error, lua_call).

#include "mce/script/script.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "lua.h"
#include "lualib.h"

#include "mce/hash.h"
#include "mce/log.h"
#include "mce/refl/type.h"

namespace mce::script {

namespace {

constexpr const char* kHostKey = "mce.host";

ScriptHost* host_of(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kHostKey);
    auto* h = static_cast<ScriptHost*>(lua_tolightuserdata(L, -1));
    lua_pop(L, 1);
    return h;
}

// Entity <-> Lua number: index in the low 32 bits, generation above (exact below 2^53).
void push_entity(lua_State* L, Entity e) {
    lua_pushnumber(L, static_cast<double>(e.index) + static_cast<double>(e.generation) * 4294967296.0);
}

Entity check_entity(lua_State* L, int idx) {
    const double v = luaL_checknumber(L, idx);
    const auto bits = static_cast<u64>(v);
    return Entity{static_cast<u32>(bits & 0xFFFFFFFFu), static_cast<u32>(bits >> 32)};
}

struct FieldRef {
    ecs::ComponentId comp = ecs::kInvalidComponent;
    const refl::FieldRecord* field = nullptr;
};

// Resolves "Comp" + "field" against the registry and the World. Raises a Lua error when unknown.
FieldRef check_field(lua_State* L, World& w, int comp_idx, int field_idx) {
    const char* comp_name = luaL_checkstring(L, comp_idx);
    const char* field_name = luaL_checkstring(L, field_idx);
    const refl::TypeRecord* type = refl::find_type(StringId(comp_name));
    if (!type || !type->is_component()) luaL_error(L, "unknown component '%s'", comp_name);
    FieldRef r;
    r.comp = w.find_component(type->type_hash);
    if (r.comp == ecs::kInvalidComponent) luaL_error(L, "component '%s' is not registered in this world", comp_name);
    r.field = type->find_field(StringId(field_name));
    if (!r.field) luaL_error(L, "component '%s' has no field '%s'", comp_name, field_name);
    if (r.field->is_array()) luaL_error(L, "'%s.%s' is an array (not in the spike)", comp_name, field_name);
    return r;
}

ecs::ComponentId check_component(lua_State* L, World& w, int idx) {
    const char* name = luaL_checkstring(L, idx);
    const refl::TypeRecord* type = refl::find_type(StringId(name));
    if (!type || !type->is_component()) luaL_error(L, "unknown component '%s'", name);
    const ecs::ComponentId c = w.find_component(type->type_hash);
    if (c == ecs::kInvalidComponent) luaL_error(L, "component '%s' is not registered in this world", name);
    return c;
}

template <class T>
T load(const u8* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

template <class T>
void store(u8* p, T v) {
    std::memcpy(p, &v, sizeof(T));
}

int push_field(lua_State* L, const refl::FieldRecord& f, const u8* p) {
    using K = refl::FieldKind;
    switch (f.kind) {
        case K::Bool:
            lua_pushboolean(L, load<bool>(p));
            return 1;
        case K::I8:
            lua_pushnumber(L, load<i8>(p));
            return 1;
        case K::I16:
            lua_pushnumber(L, load<i16>(p));
            return 1;
        case K::I32:
            lua_pushnumber(L, load<i32>(p));
            return 1;
        case K::I64:
            lua_pushnumber(L, static_cast<double>(load<i64>(p)));
            return 1;
        case K::U8:
            lua_pushnumber(L, load<u8>(p));
            return 1;
        case K::U16:
            lua_pushnumber(L, load<u16>(p));
            return 1;
        case K::U32:
            lua_pushnumber(L, load<u32>(p));
            return 1;
        case K::U64:
            lua_pushnumber(L, static_cast<double>(load<u64>(p)));
            return 1;
        case K::F32:
            lua_pushnumber(L, load<f32>(p));
            return 1;
        case K::F64:
            lua_pushnumber(L, load<f64>(p));
            return 1;
        case K::Enum:
            if (f.size == 1)
                lua_pushnumber(L, load<u8>(p));
            else if (f.size == 2)
                lua_pushnumber(L, load<u16>(p));
            else
                lua_pushnumber(L, load<u32>(p));
            return 1;
        case K::Vec2: {
            const auto v = load<math::vec2>(p);
            lua_pushvector(L, v.x, v.y, 0.0f);
            return 1;
        }
        case K::Vec3: {
            const auto v = load<math::vec3>(p);
            lua_pushvector(L, v.x, v.y, v.z);
            return 1;
        }
        case K::Vec4:
        case K::Quat:
        case K::Color: {
            const auto v = load<math::vec4>(p);
            lua_pushnumber(L, v.x);
            lua_pushnumber(L, v.y);
            lua_pushnumber(L, v.z);
            lua_pushnumber(L, v.w);
            return 4;
        }
        default:
            luaL_error(L, "field '%s' has kind %s (not in the spike)", f.name_str, refl::field_kind_name(f.kind));
    }
    return 0;
}

void write_field(lua_State* L, const refl::FieldRecord& f, u8* p, int arg) {
    using K = refl::FieldKind;
    switch (f.kind) {
        case K::Bool:
            luaL_checktype(L, arg, LUA_TBOOLEAN);
            store<bool>(p, lua_toboolean(L, arg) != 0);
            return;
        case K::I8:
            store<i8>(p, static_cast<i8>(luaL_checkinteger(L, arg)));
            return;
        case K::I16:
            store<i16>(p, static_cast<i16>(luaL_checkinteger(L, arg)));
            return;
        case K::I32:
            store<i32>(p, static_cast<i32>(luaL_checkinteger(L, arg)));
            return;
        case K::I64:
            store<i64>(p, static_cast<i64>(luaL_checknumber(L, arg)));
            return;
        case K::U8:
            store<u8>(p, static_cast<u8>(luaL_checkunsigned(L, arg)));
            return;
        case K::U16:
            store<u16>(p, static_cast<u16>(luaL_checkunsigned(L, arg)));
            return;
        case K::U32:
            store<u32>(p, static_cast<u32>(luaL_checkunsigned(L, arg)));
            return;
        case K::U64:
            store<u64>(p, static_cast<u64>(luaL_checknumber(L, arg)));
            return;
        case K::F32:
            store<f32>(p, static_cast<f32>(luaL_checknumber(L, arg)));
            return;
        case K::F64:
            store<f64>(p, luaL_checknumber(L, arg));
            return;
        case K::Vec2: {
            const float* v = luaL_checkvector(L, arg);
            store(p, math::vec2{v[0], v[1]});
            return;
        }
        case K::Vec3: {
            const float* v = luaL_checkvector(L, arg);
            store(p, math::vec3{v[0], v[1], v[2]});
            return;
        }
        case K::Vec4:
        case K::Quat:
        case K::Color:
            store(p,
                  math::vec4{static_cast<f32>(luaL_checknumber(L, arg)), static_cast<f32>(luaL_checknumber(L, arg + 1)),
                             static_cast<f32>(luaL_checknumber(L, arg + 2)),
                             static_cast<f32>(luaL_checknumber(L, arg + 3))});
            return;
        default:
            luaL_error(L, "field '%s' has kind %s (not writable in the spike)", f.name_str,
                       refl::field_kind_name(f.kind));
    }
}

// ---- the `mce` table ------------------------------------------------------------------------

World& world_of(lua_State* L, ScriptHost*& host_out);

int l_get(lua_State* L) {
    ScriptHost* host = nullptr;
    World& w = world_of(L, host);
    const Entity e = check_entity(L, 1);
    const FieldRef r = check_field(L, w, 2, 3);
    const void* row = w.get_raw(e, r.comp);
    if (!row) luaL_error(L, "entity has no '%s'", lua_tostring(L, 2));
    return push_field(L, *r.field, static_cast<const u8*>(row) + r.field->offset);
}

int l_set(lua_State* L) {
    ScriptHost* host = nullptr;
    World& w = world_of(L, host);
    const Entity e = check_entity(L, 1);
    const FieldRef r = check_field(L, w, 2, 3);
    if (r.field->has(refl::FieldFlags::ReadOnly))
        luaL_error(L, "'%s.%s' is read-only", lua_tostring(L, 2), lua_tostring(L, 3));
    void* row = w.get_raw_mut(e, r.comp);
    if (!row) luaL_error(L, "entity has no '%s'", lua_tostring(L, 2));
    write_field(L, *r.field, static_cast<u8*>(row) + r.field->offset, 4);
    w.touch(e, r.comp);
    return 0;
}

int l_each(lua_State* L) {
    ScriptHost* host = nullptr;
    World& w = world_of(L, host);
    const int n = lua_gettop(L);
    if (n < 2) luaL_error(L, "mce.each(\"Comp\", ..., fn)");
    luaL_checktype(L, n, LUA_TFUNCTION);
    ecs::ComponentMask need;
    for (int i = 1; i < n; ++i) need.set(check_component(L, w, i));
    // Tables are created in a deterministic order and rows only move at sync points, so this is the
    // order a Query visits. Entities are collected first: the callback may not change structure
    // (the World asserts that inside a phase), but it reads and writes freely.
    for (const auto& table : w.tables()) {
        if (!table->mask().contains_all(need)) continue;
        const Span<const Entity> ents = table->entities();
        for (usize row = 0; row < ents.size(); ++row) {
            lua_pushvalue(L, n);
            push_entity(L, ents[row]);
            lua_call(L, 1, 0);
        }
    }
    return 0;
}

int l_tick(lua_State* L);
int l_random(lua_State* L);

int l_log(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    MCE_LOG_INFO("script: {}", msg);
    return 0;
}

const luaL_Reg kMceLib[] = {
    {"get", l_get},       {"set", l_set}, {"each", l_each},   {"tick", l_tick},
    {"random", l_random}, {"log", l_log}, {nullptr, nullptr},
};

// The libraries a script may see (no os, io, debug, coroutine).
const luaL_Reg kLibs[] = {
    {"", luaopen_base},
    {LUA_TABLIBNAME, luaopen_table},
    {LUA_STRLIBNAME, luaopen_string},
    {LUA_MATHLIBNAME, luaopen_math},
    {LUA_UTF8LIBNAME, luaopen_utf8},
    {LUA_BITLIBNAME, luaopen_bit32},
    {LUA_BUFFERLIBNAME, luaopen_buffer},
    {LUA_VECLIBNAME, luaopen_vector},
    {nullptr, nullptr},
};

// Base-library globals that read the clock, the GC or the environment.
const char* const kRemovedGlobals[] = {"print", "collectgarbage", "gcinfo", "getfenv", "setfenv"};

}  // namespace

// The C functions reach the host's per-run context through this (a friend of ScriptHost).
struct HostAccess {
    static Frame* frame(ScriptHost& h) { return h.frame_; }
    static u32 current(ScriptHost& h) { return h.current_; }
    static u32 next_random(ScriptHost& h) { return h.random_calls_++; }
    static u64 seed(ScriptHost& h) { return h.seed_; }
};

namespace {

World& world_of(lua_State* L, ScriptHost*& host_out) {
    host_out = host_of(L);
    Frame* f = host_out ? HostAccess::frame(*host_out) : nullptr;
    if (!f) luaL_error(L, "the mce API is only available inside update()");
    return f->world();
}

int l_tick(lua_State* L) {
    ScriptHost* host = nullptr;
    (void)world_of(L, host);
    lua_pushnumber(L, static_cast<double>(HostAccess::frame(*host)->tick));
    return 1;
}

int l_random(lua_State* L) {
    ScriptHost* host = nullptr;
    (void)world_of(L, host);
    u64 h = mix64(HostAccess::seed(*host) ^ HostAccess::frame(*host)->tick);
    h = mix64(h ^ (u64(HostAccess::current(*host)) << 32 | HostAccess::next_random(*host)));
    lua_pushnumber(L, static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0));
    return 1;
}

}  // namespace

ScriptHost::ScriptHost(const ScriptBudget& budget, u64 seed) : budget_(budget), seed_(seed) {
    L_ = lua_newstate(&ScriptHost::alloc_fn, this);
    MCE_VERIFY(L_ != nullptr);
    for (const luaL_Reg* lib = kLibs; lib->func; ++lib) {
        lua_pushcfunction(L_, lib->func, nullptr);
        lua_pushstring(L_, lib->name);
        lua_call(L_, 1, 0);
    }
    for (const char* g : kRemovedGlobals) {
        lua_pushnil(L_);
        lua_setglobal(L_, g);
    }
    lua_getglobal(L_, "math");  // math.random is seeded from the clock: scripts use mce.random
    lua_pushnil(L_);
    lua_setfield(L_, -2, "random");
    lua_pushnil(L_);
    lua_setfield(L_, -2, "randomseed");
    lua_pop(L_, 1);

    lua_newtable(L_);
    luaL_register(L_, nullptr, kMceLib);
    lua_setglobal(L_, "mce");

    lua_pushlightuserdata(L_, this);
    lua_setfield(L_, LUA_REGISTRYINDEX, kHostKey);
    lua_callbacks(L_)->interrupt = &ScriptHost::interrupt_fn;
    lua_callbacks(L_)->userdata = this;
    luaL_sandbox(L_);  // every built-in table and the globals become read-only
}

ScriptHost::~ScriptHost() {
    if (L_) lua_close(L_);
}

void* ScriptHost::alloc_fn(void* ud, void* ptr, usize osize, usize nsize) {
    auto* h = static_cast<ScriptHost*>(ud);
    if (nsize == 0) {
        std::free(ptr);
        h->mem_used_ -= osize;
        return nullptr;
    }
    if (h->mem_used_ - osize + nsize > h->budget_.memory_bytes) return nullptr;  // Luau raises "not enough memory"
    void* p = std::realloc(ptr, nsize);
    if (p) h->mem_used_ = h->mem_used_ - osize + nsize;
    return p;
}

void ScriptHost::interrupt_fn(lua_State* L, int gc) {
    if (gc >= 0) return;  // GC safepoint, not script progress
    auto* h = static_cast<ScriptHost*>(lua_callbacks(L)->userdata);
    if (++h->steps_ > h->budget_.steps_per_tick) {
        h->steps_ = 0;  // the error handler itself may hit safepoints
        luaL_error(L, "step budget exceeded (%u per tick)", h->budget_.steps_per_tick);
    }
}

void ScriptHost::fail(u32 index, const char* message) {
    ScriptInfo& info = infos_[index];
    info.state = ScriptState::Failed;
    info.error.assign(StringView(message ? message : "unknown error"));
    MCE_LOG_ERROR("script {}: {}", info.name.c_str(), info.error.c_str());
}

Result<u32> ScriptHost::add_bytecode(StringView name, Span<const u8> bytecode, u8 group) {
    FixedString<64> chunk(name);
    FixedString<66> chunk_name("=");  // '=' : error messages say "name:line:", not [string "name"]
    chunk_name.append(chunk.view());
    // A thread with its own global table whose __index is the sandboxed globals: the script's
    // top level defines its functions there, then the table is frozen.
    lua_State* T = lua_newthread(L_);
    luaL_sandboxthread(T);
    if (luau_load(T, chunk_name.c_str(), reinterpret_cast<const char*>(bytecode.data()), bytecode.size(), 0) != 0) {
        FixedString<256> msg(lua_tostring(T, -1));
        lua_pop(L_, 1);
        return Error(Errc::ParseError, "script.load", msg.view());
    }
    const u32 index = static_cast<u32>(infos_.size());
    ScriptInfo info;
    info.name = chunk;
    infos_.push_back(info);
    update_refs_.push_back(-1);
    groups_.push_back(group);

    current_ = index;
    steps_ = 0;
    if (lua_pcall(T, 0, 0, 0) != 0) {
        fail(index, lua_tostring(T, -1));
        lua_pop(L_, 1);
        return Error(Errc::ParseError, "script.load", infos_[index].error.view());
    }
    lua_getglobal(T, "update");
    if (!lua_isfunction(T, -1)) {
        fail(index, "the script defines no update() function");
        lua_pop(L_, 1);
        return Error(Errc::NotFound, "script.load", infos_[index].error.view());
    }
    update_refs_[index] = lua_ref(T, -1);
    lua_pop(T, 1);
    lua_pushvalue(T, LUA_GLOBALSINDEX);
    lua_setreadonly(T, -1, 1);  // no new globals after the top level
    lua_pop(T, 1);
    lua_pop(L_, 1);  // the thread stays reachable through its function's environment
    return index;
}

namespace {
struct HostRef {
    ScriptHost* host = nullptr;
};
void script_update(Frame& f) { f.resource<HostRef>().host->run(f); }
}  // namespace

void ScriptHost::install(World& w, Phase phase) {
    w.set_resource(HostRef{this});
    w.add_system(phase, "script.update", &script_update);
}

void ScriptHost::run(Frame& f) { run_scripts(f, true, 0); }

void ScriptHost::run(Frame& f, u8 group) { run_scripts(f, false, group); }

void ScriptHost::run_scripts(Frame& f, bool all, u8 group) {
    lua_pop(L_, lua_gettop(L_));
    frame_ = &f;
    lua_getglobal(L_, "mce");
    lua_setreadonly(L_, -1, 0);
    lua_pushnumber(L_, f.dt);
    lua_setfield(L_, -2, "dt");
    lua_setreadonly(L_, -1, 1);
    lua_pop(L_, 1);
    for (u32 i = 0; i < infos_.size(); ++i) {
        if (infos_[i].state != ScriptState::Running || (!all && groups_[i] != group)) continue;
        current_ = i;
        steps_ = 0;
        random_calls_ = 0;
        lua_getref(L_, update_refs_[i]);
        const int rc = lua_pcall(L_, 0, 0, 0);
        infos_[i].last_steps = steps_;
        if (rc != 0) {
            fail(i, lua_tostring(L_, -1));
            lua_pop(L_, 1);
        }
    }
    frame_ = nullptr;
}

}  // namespace mce::script
