// mce_script (ADR-0041, M-SCRIPT): the Luau host and the core of the `mce` API (fields, each,
// tick, random, log). See mce/script/script.h for the contract; api.h for the shared helpers.

#include "mce/script/script.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "api.h"
#include "mce/asset/guid.h"
#include "mce/asset/handle.h"
#include "mce/hash.h"
#include "mce/log.h"
#include "mce/scene/scene.h"

namespace mce::script {

namespace detail {

Frame& frame_of(lua_State* L) {
    Frame* f = HostAccess::frame(HostAccess::host(L));
    if (!f) luaL_error(L, "the mce API is only available inside update()");
    return *f;
}

void push_entity(lua_State* L, Entity e) {
    if (e.is_null()) {
        lua_pushnil(L);
        return;
    }
    lua_pushnumber(L, static_cast<double>(e.index) + static_cast<double>(e.generation) * 4294967296.0);
}

Entity check_entity(lua_State* L, int idx) {
    const double v = luaL_checknumber(L, idx);
    if (!(v >= 0.0 && v < 18446744073709551616.0)) luaL_error(L, "argument #%d is not an entity", idx);
    const auto bits = static_cast<u64>(v);
    return Entity{static_cast<u32>(bits & 0xFFFFFFFFu), static_cast<u32>(bits >> 32)};
}

Entity opt_entity(lua_State* L, int idx) { return lua_isnoneornil(L, idx) ? Entity::null : check_entity(L, idx); }

const refl::TypeRecord& check_type(lua_State* L, int idx, u32 want) {
    const char* name = luaL_checkstring(L, idx);
    const refl::TypeRecord* type = refl::find_type(StringId(name));
    if (!type || (type->flags & want) == 0)
        luaL_error(L, "unknown %s '%s'", want == refl::TypeFlags::Event ? "event" : "component", name);
    return *type;
}

ecs::ComponentId component_id(World& w, const refl::TypeRecord& type) { return scene::component_for(w, type); }

ecs::ComponentId check_component(lua_State* L, World& w, int idx) {
    const refl::TypeRecord& type = check_type(L, idx, refl::TypeFlags::Component);
    const ecs::ComponentId c = w.find_component(type.type_hash);
    if (c == ecs::kInvalidComponent) luaL_error(L, "component '%s' is not registered in this world", type.name);
    return c;
}

namespace {

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

i64 load_enum(const refl::FieldRecord& f, const u8* p) {
    switch (f.size) {
        case 1: return load<u8>(p);
        case 2: return load<u16>(p);
        case 4: return load<i32>(p);
        default: return load<i64>(p);
    }
}

void store_enum(const refl::FieldRecord& f, u8* p, i64 v) {
    switch (f.size) {
        case 1: store<u8>(p, static_cast<u8>(v)); return;
        case 2: store<u16>(p, static_cast<u16>(v)); return;
        case 4: store<i32>(p, static_cast<i32>(v)); return;
        default: store<i64>(p, v); return;
    }
}

}  // namespace

int push_value(lua_State* L, const refl::FieldRecord& f, const u8* p) {
    using K = refl::FieldKind;
    if (f.is_array()) {
        lua_createtable(L, static_cast<int>(f.count), 0);
        refl::FieldRecord one = f;
        one.count = 1;
        for (u32 i = 0; i < f.count; ++i) {
            if (push_value(L, one, p + usize(i) * f.size) != 1) {
                lua_pop(L, 1);
                luaL_error(L, "'%s' is an array of a kind scripts read one field at a time", f.name_str);
            }
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }
    switch (f.kind) {
        case K::Bool: lua_pushboolean(L, load<bool>(p)); return 1;
        case K::I8: lua_pushnumber(L, load<i8>(p)); return 1;
        case K::I16: lua_pushnumber(L, load<i16>(p)); return 1;
        case K::I32: lua_pushnumber(L, load<i32>(p)); return 1;
        case K::I64: lua_pushnumber(L, static_cast<double>(load<i64>(p))); return 1;
        case K::U8: lua_pushnumber(L, load<u8>(p)); return 1;
        case K::U16: lua_pushnumber(L, load<u16>(p)); return 1;
        case K::U32: lua_pushnumber(L, load<u32>(p)); return 1;
        case K::U64: lua_pushnumber(L, static_cast<double>(load<u64>(p))); return 1;
        case K::F32: lua_pushnumber(L, load<f32>(p)); return 1;
        case K::F64: lua_pushnumber(L, load<f64>(p)); return 1;
        case K::String: {
            const StringView s = f.string_ops->get(p);
            lua_pushlstring(L, s.data(), s.size());
            return 1;
        }
        case K::Enum: {
            const i64 v = load_enum(f, p);
            if (const refl::EnumRecord* e = refl::find_enum(f.type_id))
                for (const refl::EnumValue& ev : e->values)
                    if (ev.value == v) {
                        lua_pushstring(L, ev.name_str);
                        return 1;
                    }
            lua_pushnumber(L, static_cast<double>(v));
            return 1;
        }
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
        case K::EntityRef: push_entity(L, load<Entity>(p)); return 1;
        case K::AssetRef: {
            const auto g = load<asset::Guid>(p);
            if (g.is_null()) {
                lua_pushnil(L);
                return 1;
            }
            const asset::GuidString s = asset::to_string(g);
            lua_pushfstring(L, "guid:%s", s.c_str());
            return 1;
        }
        case K::Struct: {
            const refl::TypeRecord* t = refl::find_type(f.type_id);
            if (!t) luaL_error(L, "'%s' has an unreflected struct type", f.name_str);
            push_struct(L, *t, p);
            return 1;
        }
    }
    luaL_error(L, "field '%s' has kind %s, which scripts cannot read", f.name_str, refl::field_kind_name(f.kind));
    return 0;
}

void write_value(lua_State* L, const refl::FieldRecord& f, u8* p, int arg) {
    using K = refl::FieldKind;
    if (f.is_array()) {
        luaL_checktype(L, arg, LUA_TTABLE);
        refl::FieldRecord one = f;
        one.count = 1;
        for (u32 i = 0; i < f.count; ++i) {
            lua_rawgeti(L, arg, static_cast<int>(i + 1));
            if (!lua_isnil(L, -1)) write_value(L, one, p + usize(i) * f.size, lua_gettop(L));
            lua_pop(L, 1);
        }
        return;
    }
    switch (f.kind) {
        case K::Bool:
            luaL_checktype(L, arg, LUA_TBOOLEAN);
            store<bool>(p, lua_toboolean(L, arg) != 0);
            return;
        case K::I8: store<i8>(p, static_cast<i8>(luaL_checkinteger(L, arg))); return;
        case K::I16: store<i16>(p, static_cast<i16>(luaL_checkinteger(L, arg))); return;
        case K::I32: store<i32>(p, static_cast<i32>(luaL_checkinteger(L, arg))); return;
        case K::I64: store<i64>(p, static_cast<i64>(luaL_checknumber(L, arg))); return;
        case K::U8: store<u8>(p, static_cast<u8>(luaL_checkunsigned(L, arg))); return;
        case K::U16: store<u16>(p, static_cast<u16>(luaL_checkunsigned(L, arg))); return;
        case K::U32: store<u32>(p, static_cast<u32>(luaL_checkunsigned(L, arg))); return;
        case K::U64: store<u64>(p, static_cast<u64>(luaL_checknumber(L, arg))); return;
        case K::F32: store<f32>(p, static_cast<f32>(luaL_checknumber(L, arg))); return;
        case K::F64: store<f64>(p, luaL_checknumber(L, arg)); return;
        case K::String: {
            usize len = 0;
            const char* s = luaL_checklstring(L, arg, &len);
            f.string_ops->set(p, StringView(s, len));
            return;
        }
        case K::Enum: {
            if (lua_type(L, arg) == LUA_TSTRING) {
                const char* name = lua_tostring(L, arg);
                if (const refl::EnumRecord* e = refl::find_enum(f.type_id))
                    for (const refl::EnumValue& ev : e->values)
                        if (std::strcmp(ev.name_str, name) == 0) {
                            store_enum(f, p, ev.value);
                            return;
                        }
                luaL_error(L, "'%s' has no value '%s'", f.name_str, name);
            }
            store_enum(f, p, static_cast<i64>(luaL_checknumber(L, arg)));
            return;
        }
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
            if (lua_istable(L, arg)) {  // {x, y, z, w} from a table (struct and spawn tables)
                f32 c[4];
                for (int i = 0; i < 4; ++i) {
                    lua_rawgeti(L, arg, i + 1);
                    c[i] = static_cast<f32>(luaL_checknumber(L, -1));
                    lua_pop(L, 1);
                }
                store(p, math::vec4{c[0], c[1], c[2], c[3]});
                return;
            }
            store(p, math::vec4{static_cast<f32>(luaL_checknumber(L, arg)), static_cast<f32>(luaL_checknumber(L, arg + 1)),
                                static_cast<f32>(luaL_checknumber(L, arg + 2)),
                                static_cast<f32>(luaL_checknumber(L, arg + 3))});
            return;
        case K::EntityRef: store(p, opt_entity(L, arg)); return;
        case K::AssetRef: {
            asset::Guid g;
            if (!lua_isnoneornil(L, arg)) {
                const char* s = luaL_checkstring(L, arg);
                const StringView text(s);
                const StringView body = text.substr(0, 5) == StringView("guid:") ? text.substr(5) : text;
                Result<asset::Guid> parsed = asset::parse_guid(body);
                if (!parsed) luaL_error(L, "'%s' is not a GUID", s);
                g = parsed.value();
            }
            // The GUID; the runtime handle is stamped by whoever loads it (a fresh, unloaded ref).
            u8 zero[24] = {};
            std::memcpy(zero, &g, sizeof(g));
            std::memcpy(p, zero, f.size < sizeof(zero) ? f.size : sizeof(zero));
            return;
        }
        case K::Struct: {
            const refl::TypeRecord* t = refl::find_type(f.type_id);
            if (!t) luaL_error(L, "'%s' has an unreflected struct type", f.name_str);
            luaL_checktype(L, arg, LUA_TTABLE);
            write_struct(L, *t, p, arg);
            return;
        }
    }
    luaL_error(L, "field '%s' has kind %s, which scripts cannot write", f.name_str, refl::field_kind_name(f.kind));
}

void push_struct(lua_State* L, const refl::TypeRecord& type, const u8* p) {
    lua_createtable(L, 0, static_cast<int>(type.fields.size()));
    for (const refl::FieldRecord& f : type.fields) {
        const int n = push_value(L, f, p + f.offset);
        if (n == 4) {  // Vec4/Quat/Color: a {x, y, z, w} array inside a table
            lua_createtable(L, 4, 0);
            for (int i = 4; i >= 1; --i) {
                lua_pushvalue(L, -1 - (5 - i));
                lua_rawseti(L, -2, i);
            }
            lua_replace(L, -5);
            lua_pop(L, 3);
        }
        lua_setfield(L, -2, f.name_str);
    }
}

void write_struct(lua_State* L, const refl::TypeRecord& type, u8* p, int table_idx) {
    if (table_idx < 0) table_idx = lua_gettop(L) + table_idx + 1;
    // Walk the type's fields (a fixed order), never the table's keys (pairs order is the table's).
    for (const refl::FieldRecord& f : type.fields) {
        lua_getfield(L, table_idx, f.name_str);
        if (!lua_isnil(L, -1)) write_value(L, f, p + f.offset, lua_gettop(L));
        lua_pop(L, 1);
    }
    // A key that names no field is an error, not a silent drop.
    lua_pushnil(L);
    while (lua_next(L, table_idx) != 0) {
        lua_pop(L, 1);
        if (lua_type(L, -1) != LUA_TSTRING || !type.find_field(StringId(lua_tostring(L, -1))))
            luaL_error(L, "'%s' has no field '%s'", type.name, lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "?");
    }
}

u8* scratch_default(ScriptHost& host, const refl::TypeRecord& type, usize offset) {
    Array<u8>& s = HostAccess::scratch(host);
    if (s.size() < offset + type.size + 16) s.resize(offset + type.size + 16);
    u8* p = s.data() + offset;
    if (type.construct_default) type.construct_default(p);
    else std::memset(p, 0, type.size);
    return p;
}

}  // namespace detail

namespace {

using namespace detail;

// ---- fields --------------------------------------------------------------------------------

// The field an mce.get / mce.set call names, as a handle or as ("Comp", "field"). `arg` is the
// first of them; `next` gets the index of the first value argument.
struct FieldAt {
    const refl::FieldRecord* field;
    ecs::ComponentId comp;
    const char* comp_name;
};

// mce.field handles are light userdata with this tag holding index + 1: opaque to scripts, and
// told apart from other values in one call.
constexpr int kFieldHandleTag = 1;

FieldAt check_field(lua_State* L, World& w, int arg, int& next) {
    if (void* p = lua_tolightuserdatatagged(L, arg, kFieldHandleTag)) {
        Array<HostAccess::FieldHandle>& hs = HostAccess::handles(HostAccess::host(L));
        const auto i = reinterpret_cast<uintptr_t>(p);
        if (i > hs.size()) luaL_error(L, "argument #%d is not a field handle", arg);
        HostAccess::FieldHandle& h = hs[static_cast<usize>(i - 1)];
        if (h.world != &w) {
            h.world = &w;
            h.comp = w.find_component(h.type->type_hash);
        }
        if (h.comp == ecs::kInvalidComponent) {
            h.world = nullptr;  // look again next call: the component may be registered later
            luaL_error(L, "component '%s' is not registered in this world", h.type->name);
        }
        next = arg + 1;
        return FieldAt{h.field, h.comp, h.type->name};
    }
    const refl::TypeRecord& type = check_type(L, arg, refl::TypeFlags::Component);
    const char* field_name = luaL_checkstring(L, arg + 1);
    const refl::FieldRecord* f = type.find_field(StringId(field_name));
    if (!f) luaL_error(L, "component '%s' has no field '%s'", type.name, field_name);
    const ecs::ComponentId c = w.find_component(type.type_hash);
    if (c == ecs::kInvalidComponent) luaL_error(L, "component '%s' is not registered in this world", type.name);
    next = arg + 2;
    return FieldAt{f, c, type.name};
}

int l_field(lua_State* L) {
    ScriptHost& host = HostAccess::host(L);
    const refl::TypeRecord& type = check_type(L, 1, refl::TypeFlags::Component);
    const char* field_name = luaL_checkstring(L, 2);
    const refl::FieldRecord* f = type.find_field(StringId(field_name));
    if (!f) luaL_error(L, "component '%s' has no field '%s'", type.name, field_name);
    Array<HostAccess::FieldHandle>& hs = HostAccess::handles(host);
    usize index = 0;
    while (index < hs.size() && hs[index].field != f) ++index;
    if (index == hs.size()) {
        HostAccess::FieldHandle h;
        h.type = &type;
        h.field = f;
        hs.push_back(h);
    }
    lua_pushlightuserdatatagged(L, reinterpret_cast<void*>(static_cast<uintptr_t>(index + 1)), kFieldHandleTag);
    return 1;
}

// mce.get(e, field [, field...]): one value per field (four for a Vec4/Quat/Color), so a script
// reads several fields of one entity in one call.
int l_get(lua_State* L) {
    World& w = world_of(L);
    const Entity e = check_entity(L, 1);
    const int top = lua_gettop(L);
    int pushed = 0;
    int arg = 2;
    ecs::ComponentId row_comp = ecs::kInvalidComponent;  // fields of one component share its row
    const void* row = nullptr;
    do {
        int next = 0;
        const FieldAt at = check_field(L, w, arg, next);
        if (at.comp != row_comp) {
            row = w.get_raw(e, at.comp);
            row_comp = at.comp;
        }
        if (!row) luaL_error(L, "entity has no '%s'", at.comp_name);
        pushed += push_value(L, *at.field, static_cast<const u8*>(row) + at.field->offset);
        arg = next;
    } while (arg <= top);
    return pushed;
}

// How many arguments a field's value takes in mce.set: four numbers for Vec4/Quat/Color (or one
// table), one otherwise.
int value_args(lua_State* L, const refl::FieldRecord& f, int arg) {
    using K = refl::FieldKind;
    const bool four = !f.is_array() && (f.kind == K::Vec4 || f.kind == K::Quat || f.kind == K::Color);
    return four && !lua_istable(L, arg) ? 4 : 1;
}

// mce.set(e, field, value [, field, value...])
int l_set(lua_State* L) {
    World& w = world_of(L);
    const Entity e = check_entity(L, 1);
    const int top = lua_gettop(L);
    int arg = 2;
    ecs::ComponentId row_comp = ecs::kInvalidComponent;  // fields of one component share its row
    void* row = nullptr;
    do {
        int next = 0;
        const FieldAt at = check_field(L, w, arg, next);
        if (at.field->has(refl::FieldFlags::ReadOnly)) luaL_error(L, "'%s.%s' is read-only", at.comp_name, at.field->name_str);
        if (at.comp != row_comp) {
            row = w.get_raw_mut(e, at.comp);
            row_comp = at.comp;
            if (row) w.touch(e, at.comp);  // marked before the write: a raised error leaves it marked, harmlessly
        }
        if (!row) luaL_error(L, "entity has no '%s'", at.comp_name);
        write_value(L, *at.field, static_cast<u8*>(row) + at.field->offset, next);
        arg = next + value_args(L, *at.field, next);
    } while (arg <= top);
    return 0;
}

int l_each(lua_State* L) {
    World& w = world_of(L);
    const int n = lua_gettop(L);
    if (n < 2) luaL_error(L, "mce.each(\"Comp\", ..., fn)");
    luaL_checktype(L, n, LUA_TFUNCTION);
    ecs::ComponentMask need;
    for (int i = 1; i < n; ++i) {
        const refl::TypeRecord& type = check_type(L, i, refl::TypeFlags::Component);
        const ecs::ComponentId c = w.find_component(type.type_hash);
        if (c == ecs::kInvalidComponent) return 0;  // no entity can have it
        need.set(c);
    }
    // Tables are created in a deterministic order and rows only move at sync points, so this is the
    // order a Query visits. The callback may record structural changes (they go through Commands).
    const usize table_count = w.tables().size();
    for (usize t = 0; t < table_count; ++t) {
        const ecs::detail::Table& table = *w.tables()[t];
        if (!table.mask().contains_all(need)) continue;
        for (u32 row = 0; row < table.size(); ++row) {
            lua_pushvalue(L, n);
            push_entity(L, table.entities()[row]);
            lua_call(L, 1, 0);
        }
    }
    return 0;
}

int l_tick(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(frame_of(L).tick));
    return 1;
}

int l_random(lua_State* L) {
    ScriptHost& host = HostAccess::host(L);
    const Frame& f = frame_of(L);
    u64 h = mix64(HostAccess::seed(host) ^ f.tick);
    h = mix64(h ^ (u64(HostAccess::current(host)) << 32 | HostAccess::next_random(host)));
    lua_pushnumber(L, static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0));
    return 1;
}

int l_log(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    MCE_LOG_INFO("script: {}", msg);
    return 0;
}

// ---- iteration -------------------------------------------------------------------------------

// pairs() and next() refuse keys whose hash follows their address (tables, functions, buffers,
// threads, userdata): their order differs between runs (M-SCRIPT 3). pairs hands the loop this
// sentinel as the first key, so the VM calls checked_next instead of its built-in traversal.
constexpr int kPairsStartTag = 2;

int checked_next(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 2);
    if (lua_tolightuserdatatagged(L, 2, kPairsStartTag)) {
        lua_pushnil(L);
        lua_replace(L, 2);
    }
    if (lua_next(L, 1) == 0) return 0;
    switch (lua_type(L, -2)) {
        case LUA_TTABLE:
        case LUA_TFUNCTION:
        case LUA_TUSERDATA:
        case LUA_TTHREAD:
        case LUA_TBUFFER:
            luaL_error(L, "pairs over a table keyed by a %s: the order differs between runs; key by entity, number or string",
                       luaL_typename(L, -2));
            return 0;
        default: return 2;
    }
}

int checked_pairs(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushcfunction(L, checked_next, "next");
    lua_pushvalue(L, 1);
    lua_pushlightuserdatatagged(L, reinterpret_cast<void*>(uintptr_t{1}), kPairsStartTag);
    return 3;
}

const luaL_Reg kMceLib[] = {
    {"field", l_field}, {"get", l_get},       {"set", l_set}, {"each", l_each},
    {"tick", l_tick},   {"random", l_random}, {"log", l_log}, {nullptr, nullptr},
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

ScriptHost::ScriptHost(const ScriptBudget& budget, u64 seed) : budget_(budget), seed_(seed) {
    L_ = lua_newstate(&ScriptHost::alloc_fn, this);
    MCE_VERIFY(L_ != nullptr);
    lua_callbacks(L_)->userdata = this;
    for (const luaL_Reg* lib = kLibs; lib->func; ++lib) {
        lua_pushcfunction(L_, lib->func, nullptr);
        lua_pushstring(L_, lib->name);
        lua_call(L_, 1, 0);
    }
    for (const char* g : kRemovedGlobals) {
        lua_pushnil(L_);
        lua_setglobal(L_, g);
    }
    lua_pushcfunction(L_, checked_pairs, "pairs");
    lua_setglobal(L_, "pairs");
    lua_pushcfunction(L_, checked_next, "next");
    lua_setglobal(L_, "next");
    lua_getglobal(L_, "math");  // math.random is seeded from the clock: scripts use mce.random
    lua_pushnil(L_);
    lua_setfield(L_, -2, "random");
    lua_pushnil(L_);
    lua_setfield(L_, -2, "randomseed");
    lua_pop(L_, 1);

    lua_newtable(L_);
    luaL_register(L_, nullptr, kMceLib);
    detail::register_world_api(L_);
    lua_setglobal(L_, "mce");

    lua_callbacks(L_)->interrupt = &ScriptHost::interrupt_fn;
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
    MCE_LOG_ERROR("script failed: {}", info.error.c_str());  // the error names the script and line
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
