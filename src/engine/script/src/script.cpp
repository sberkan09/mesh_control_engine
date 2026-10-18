// mce_script (ADR-0041, M-SCRIPT): the Luau host and the core of the `mce` API (fields, each,
// tick, random, log). See mce/script/script.h for the contract; api.h for the shared helpers.

#include "mce/script/script.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "api.h"
#include "mce/asset/guid.h"
#include "mce/asset/handle.h"
#include "mce/containers/hash_map.h"
#include "mce/crash.h"
#include "mce/hash.h"
#include "mce/log.h"
#include "mce/scene/scene.h"

#if defined(TRACY_ENABLE)
#include <tracy/Tracy.hpp>
// One zone per script, named after it, so the profiler shows which script costs what.
#define MCE_SCRIPT_ZONE(name) ZoneTransientN(mce_script_zone, name, true)
#else
#define MCE_SCRIPT_ZONE(name) ((void)0)
#endif

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
    u32 element = 0;  // a handle to one element of an array field (mce.field("C", "f", 3)); 0: the whole field
};

// The record and byte offset a get / set works on: the field, or its one element as a non-array field.
refl::FieldRecord field_view(const FieldAt& at, usize& offset) {
    refl::FieldRecord f = *at.field;
    offset = f.offset;
    if (at.element != 0) {
        f.count = 1;
        offset += usize(at.element - 1) * f.size;
    }
    return f;
}

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
        return FieldAt{h.field, h.comp, h.type->name, h.element};
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
    u32 element = 0;
    if (!lua_isnoneornil(L, 3)) {
        if (!f->is_array())
            luaL_error(L, "'%s.%s' is not an array, so it has no elements to name", type.name, f->name_str);
        const double e = luaL_checknumber(L, 3);
        if (!(e >= 1.0 && e <= double(f->count)) || e != std::floor(e))
            luaL_error(L, "'%s.%s' has %u elements; the element must be a whole number from 1 to %u", type.name,
                       f->name_str, f->count, f->count);
        element = static_cast<u32>(e);
    }
    Array<HostAccess::FieldHandle>& hs = HostAccess::handles(host);
    usize index = 0;
    while (index < hs.size() && !(hs[index].field == f && hs[index].element == element)) ++index;
    if (index == hs.size()) {
        HostAccess::FieldHandle h;
        h.type = &type;
        h.field = f;
        h.element = element;
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
        usize offset = 0;
        const refl::FieldRecord view = field_view(at, offset);
        pushed += push_value(L, view, static_cast<const u8*>(row) + offset);
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
        usize offset = 0;
        const refl::FieldRecord view = field_view(at, offset);
        write_value(L, view, static_cast<u8*>(row) + offset, next);
        arg = next + value_args(L, view, next);
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
    crash::add_context(&ScriptHost::crash_context, this);
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
    detail::register_mission_api(L_);
    detail::register_clock_api(L_);

    lua_callbacks(L_)->interrupt = &ScriptHost::interrupt_fn;
    luaL_sandbox(L_);  // every built-in table and the globals become read-only
}

ScriptHost::~ScriptHost() {
    crash::remove_context(this);
    if (L_) lua_close(L_);
}

// A crash while a script runs (a host function faulting) says which script and where in it. Runs in
// the signal handler: lua_getinfo only reads the call stack.
void ScriptHost::crash_context(void* user, void (*put)(void* sink, const char* text), void* sink) {
    const auto* h = static_cast<const ScriptHost*>(user);
    if (!h->in_update_ || h->current_ >= h->infos_.size()) return;
    put(sink, "  luau: update() of script ");
    put(sink, h->infos_[h->current_].name.c_str());
    put(sink, "\n");
    for (int level = 0; level < 12; ++level) {
        lua_Debug ar;
        if (!lua_getinfo(h->L_, level, "sln", &ar)) break;
        char line[16];
        int n = 0;
        for (int v = ar.currentline < 0 ? 0 : ar.currentline, d = 1000000000; d > 0; d /= 10)
            if (v / d != 0 || n > 0 || d == 1) line[n++] = static_cast<char>('0' + (v / d) % 10);
        line[n] = '\0';
        put(sink, "    at ");
        put(sink, ar.short_src ? ar.short_src : "?");
        put(sink, ":");
        put(sink, line);
        if (ar.name) {
            put(sink, " in ");
            put(sink, ar.name);
        }
        put(sink, "\n");
    }
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

// ---- the state check (M-SCRIPT3 step 2) ---------------------------------------------------------
//
// After a script's top level ran, one walk from its update() and its global table freezes every
// table it reaches (and every userdata's metatable), and records every upvalue slot of the
// functions it reaches and every buffer, with its value then. Frozen tables can only change
// through an upvalue or a buffer, so comparing those after each update() finds any state the
// script kept between ticks (iteration order over object keys excepted, which no check sees).
// The walk runs no Lua code (raw iteration, raw metatables) and lives in lua_cpcall, so running
// out of VM memory fails the load instead of unwinding the host.

namespace {

using StateWatch = HostAccess::StateWatch;

constexpr usize kWatchedBufferBytes = 256u * 1024u;  // per script: buffers are compared byte by byte

struct WalkItem {
    u32 root;  // where the walk first found it (index into watch_roots_)
    i32 line;  // line of the nearest function on the way
};

// What watch_state hands the walk. Plain pointers only: Luau errors longjmp through walk_state.
struct StateWalk {
    HashMap<const void*, u8>* visited = nullptr;
    Array<WalkItem>* items = nullptr;  // parallel to the queue table
    Array<StateWatch>* watches = nullptr;
    Array<u8>* bytes = nullptr;
    Array<FixedString<64>>* roots = nullptr;
    int update_ref = -1;
    u32 update_root = 0;  // "update()"; the script's globals are the next one
    usize buffer_bytes = 0;
};

constexpr int kWalkQueue = 2;  // stack index of the queue table inside walk_state

// Tables, functions, userdata and buffers can hold or be state; everything else is a value
// (strings, numbers, vectors, booleans, light userdata) or something scripts cannot make (threads).
bool walkable(lua_State* L, int idx) {
    switch (lua_type(L, idx)) {
        case LUA_TTABLE:
        case LUA_TFUNCTION:
        case LUA_TUSERDATA:
        case LUA_TBUFFER:
        case LUA_TCLASS:
        case LUA_TOBJECT: return true;
        default: return false;
    }
}

// Queues the value at `idx` unless it is a value or was seen (cycles end here).
void walk_push(lua_State* L, StateWalk& s, int idx, u32 root, i32 line) {
    if (!walkable(L, idx)) return;
    if (lua_type(L, idx) == LUA_TCLASS || lua_type(L, idx) == LUA_TOBJECT)
        luaL_error(L, "update() reaches a class object, which the state check cannot see into");
    if (!s.visited->try_emplace(lua_topointer(L, idx)).second) return;
    lua_pushvalue(L, idx);
    lua_rawseti(L, kWalkQueue, static_cast<int>(s.items->size() + 1));
    s.items->push_back(WalkItem{root, line});
}

// "'n'" when the bytecode names upvalues (debugLevel 2), "#1" otherwise.
void upvalue_label(char* out, usize size, const char* name, int n) {
    if (name && name[0]) std::snprintf(out, size, "'%s'", name);
    else std::snprintf(out, size, "#%d", n);
}

u32 add_root(StateWalk& s, const char* text) {
    s.roots->push_back(FixedString<64>(text));
    return static_cast<u32>(s.roots->size() - 1);
}

// The upvalue at the stack top, as a watch of the function at `fn`.
void watch_upvalue(lua_State* L, StateWalk& s, int fn, int n, u32 root, i32 line, bool of_update) {
    StateWatch w;
    w.of_c = lua_iscfunction(L, fn) != 0;
    w.root = root;
    w.line = line;
    w.slot = static_cast<u16>(n);
    w.of_update = of_update;
    w.type = static_cast<u8>(lua_type(L, -1));
    if (w.type == LUA_TNUMBER) {  // by bit pattern: a NaN stays equal to itself
        const double d = lua_tonumber(L, -1);
        std::memcpy(w.bits, &d, sizeof(d));
    } else if (w.type == LUA_TVECTOR) {
        std::memcpy(w.bits, lua_tovector(L, -1), LUA_VECTOR_SIZE * sizeof(float));
    } else {
        w.base_ref = lua_ref(L, -1);  // keeps the baseline alive: no address reuse
    }
    // Pushed before the second ref: if lua_ref runs out of memory the cleanup still finds (and unrefs) base_ref.
    s.watches->push_back(w);
    (*s.watches)[s.watches->size() - 1].holder_ref = lua_ref(L, fn);
}

void walk_one(lua_State* L, StateWalk& s, usize i) {
    const int t = lua_gettop(L);
    const WalkItem item = (*s.items)[i];
    switch (lua_type(L, t)) {
        case LUA_TTABLE: {
            const bool globals = i == 1;  // the script's own global table: each entry names its root
            lua_setreadonly(L, t, 1);
            for (int it = 0; (it = lua_rawiter(L, t, it)) >= 0; lua_pop(L, 2)) {
                u32 root = item.root;
                if (globals && lua_type(L, -2) == LUA_TSTRING && walkable(L, -1) && !s.visited->contains(lua_topointer(L, -1))) {
                    char label[64];
                    std::snprintf(label, sizeof(label), "global '%s'", lua_tostring(L, -2));
                    root = add_root(s, label);
                }
                walk_push(L, s, -2, root, item.line);
                walk_push(L, s, -1, root, item.line);
            }
            if (lua_getmetatable(L, t)) {
                walk_push(L, s, -1, item.root, item.line);
                lua_pop(L, 1);
            }
            return;
        }
        case LUA_TFUNCTION: {
            const bool is_lua = !lua_iscfunction(L, t);
            const bool of_update = i == 0;
            i32 line = item.line;
            lua_Debug ar;
            if (is_lua && lua_getinfo(L, -1, "s", &ar)) line = ar.linedefined;
            // C closures too: string.gmatch keeps its position in an upvalue.
            for (int n = 1;; ++n) {
                const char* name = lua_getupvalue(L, t, n);
                if (!name) break;
                u32 root = item.root;
                if (of_update && walkable(L, -1) && !s.visited->contains(lua_topointer(L, -1))) {
                    char uv[40];
                    char label[64];
                    upvalue_label(uv, sizeof(uv), name, n);
                    std::snprintf(label, sizeof(label), "upvalue %s of update()", uv);
                    root = add_root(s, label);
                }
                watch_upvalue(L, s, t, n, item.root, line, of_update);
                walk_push(L, s, -1, root, line);
                lua_pop(L, 1);
            }
            if (is_lua) {
                lua_getfenv(L, t);
                walk_push(L, s, -1, item.root, line);
                lua_pop(L, 1);
            }
            return;
        }
        case LUA_TUSERDATA:
            if (lua_getmetatable(L, t)) {  // newproxy(true): a metatable the script owns
                walk_push(L, s, -1, item.root, item.line);
                lua_pop(L, 1);
            }
            return;
        case LUA_TBUFFER: {
            size_t len = 0;
            const void* data = lua_tobuffer(L, t, &len);
            s.buffer_bytes += len;
            if (s.buffer_bytes > kWatchedBufferBytes)
                luaL_error(L, "the buffers update() reaches hold more than %u KiB, more than the state check watches; create them per call",
                           static_cast<unsigned>(kWatchedBufferBytes / 1024u));
            StateWatch w;
            w.root = item.root;
            w.line = item.line;
            w.type = LUA_TBUFFER;
            w.bits[0] = s.bytes->size();
            w.bits[1] = len;
            w.holder_ref = lua_ref(L, t);
            s.watches->push_back(w);
            s.bytes->resize(s.bytes->size() + len);
            if (len) std::memcpy(s.bytes->data() + w.bits[0], data, len);
            return;
        }
        default: return;
    }
}

int walk_state(lua_State* L) {
    StateWalk& s = *static_cast<StateWalk*>(lua_touserdata(L, 1));
    lua_createtable(L, 0, 0);  // kWalkQueue: every object found, in order (and kept alive)
    // The shared globals and the library tables in them are read-only already (luaL_sandbox), and
    // `mce` must stay writable for the host's mce.dt: seen before the walk starts.
    s.visited->try_emplace(lua_topointer(L, LUA_GLOBALSINDEX));
    for (int it = 0; (it = lua_rawiter(L, LUA_GLOBALSINDEX, it)) >= 0; lua_pop(L, 2))
        if (lua_istable(L, -1)) s.visited->try_emplace(lua_topointer(L, -1));

    lua_getref(L, s.update_ref);
    lua_Debug ar;
    const i32 update_line = lua_getinfo(L, -1, "s", &ar) ? ar.linedefined : 0;
    walk_push(L, s, -1, s.update_root, update_line);  // item 0
    lua_getfenv(L, -1);
    walk_push(L, s, -1, s.update_root + 1, update_line);  // item 1: the script's global table
    lua_pop(L, 2);
    for (usize i = 0; i < s.items->size(); ++i) {
        lua_rawgeti(L, kWalkQueue, static_cast<int>(i + 1));
        walk_one(L, s, i);
        lua_pop(L, 1);
    }
    return 0;
}

// The watched upvalue's value at the stack top against its baseline.
bool same_value(lua_State* L, const StateWatch& w) {
    const int type = lua_type(L, -1);
    if (type != w.type) return false;
    if (type == LUA_TNUMBER) {
        // Bit patterns, so a NaN upvalue stays equal to itself.
        const double d = lua_tonumber(L, -1);
        u64 now;
        std::memcpy(&now, &d, sizeof(now));
        return now == w.bits[0];
    }
    if (type == LUA_TVECTOR) {
        u64 now[2] = {};
        std::memcpy(now, lua_tovector(L, -1), LUA_VECTOR_SIZE * sizeof(float));
        return now[0] == w.bits[0] && now[1] == w.bits[1];
    }
    lua_getref(L, w.base_ref);
    const bool same = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 1);
    return same;
}

}  // namespace

bool ScriptHost::watch_state(u32 index) {
    HashMap<const void*, u8> visited;
    Array<WalkItem> items;
    StateWalk s;
    s.visited = &visited;
    s.items = &items;
    s.watches = &watches_;
    s.bytes = &watch_bytes_;
    s.roots = &watch_roots_;
    s.update_ref = update_refs_[index];
    s.update_root = static_cast<u32>(watch_roots_.size());
    watch_roots_.push_back(FixedString<64>("update()"));
    watch_roots_.push_back(FixedString<64>("the script's globals"));
    const usize bytes_first = watch_bytes_.size();
    if (lua_cpcall(L_, &walk_state, &s) == 0) return true;
    const char* why = lua_tostring(L_, -1);
    FixedString<256> msg(infos_[index].name.view());
    msg.append(": state check: ");
    msg.append(why ? why : "error");
    lua_pop(L_, 1);
    for (usize k = watch_first_[index]; k < watches_.size(); ++k) {
        lua_unref(L_, watches_[k].holder_ref);
        lua_unref(L_, watches_[k].base_ref);
    }
    watches_.resize(watch_first_[index]);
    watch_bytes_.resize(bytes_first);
    fail(index, msg.c_str());
    return false;
}

bool ScriptHost::state_unchanged(u32 index) {
    const usize end = index + 1 < watch_first_.size() ? watch_first_[index + 1] : watches_.size();
    for (usize k = watch_first_[index]; k < end; ++k) {
        const StateWatch& w = watches_[k];
        lua_getref(L_, w.holder_ref);
        bool same = true;
        if (w.slot == 0) {
            size_t len = 0;
            const void* data = lua_tobuffer(L_, -1, &len);
            same = len == w.bits[1] && (len == 0 || std::memcmp(data, watch_bytes_.data() + w.bits[0], len) == 0);
            lua_pop(L_, 1);
        } else {
            lua_getupvalue(L_, -1, w.slot);
            same = same_value(L_, w);
            lua_pop(L_, 2);
        }
        if (same) continue;
        // "<name>:<line>: update() kept state between ticks: <what> changed; keep it in a component"
        char line[16];
        std::snprintf(line, sizeof(line), ":%d: ", w.line);
        FixedString<256> msg(infos_[index].name.view());
        msg.append(line);
        msg.append("update() kept state between ticks: ");
        if (w.slot == 0) {
            msg.append("a buffer reached from ");
            msg.append(watch_roots_[w.root].view());
        } else {
            lua_getref(L_, w.holder_ref);
            char uv[40];
            const char* uv_name = lua_getupvalue(L_, -1, w.slot);
            upvalue_label(uv, sizeof(uv), uv_name, w.slot);
            lua_pop(L_, uv_name ? 2 : 1);
            msg.append("upvalue ");
            msg.append(uv);
            if (w.of_update) {
                msg.append(" of update()");
            } else {
                char holder[48];
                if (w.of_c) std::snprintf(holder, sizeof(holder), " of a library function");
                else std::snprintf(holder, sizeof(holder), " of the function defined at line %d", w.line);
                msg.append(holder);
            }
        }
        msg.append(" changed");
        if (!w.of_update && w.slot != 0) {
            msg.append(" (reached from ");
            msg.append(watch_roots_[w.root].view());
            msg.append(")");
        }
        msg.append("; keep it in a component");
        fail(index, msg.c_str());
        return false;
    }
    return true;
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
    watch_first_.push_back(static_cast<u32>(watches_.size()));

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
    if (budget_.check_state && !watch_state(index))
        return Error(Errc::ParseError, "script.load", infos_[index].error.view());
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
        MCE_SCRIPT_ZONE(infos_[i].name.c_str());
        current_ = i;
        steps_ = 0;
        random_calls_ = 0;
        lua_getref(L_, update_refs_[i]);
        in_update_ = true;
        const int rc = lua_pcall(L_, 0, 0, 0);
        in_update_ = false;
        infos_[i].last_steps = steps_;
        if (rc != 0) {
            // A write into a table the state check froze: say why it is frozen.
            const char* msg = lua_tostring(L_, -1);
            if (budget_.check_state && msg && StringView(msg).ends_with("readonly table")) {
                FixedString<256> hinted(msg);
                hinted.append(" (tables reachable from update() are frozen after the top level; keep state in a component)");
                fail(i, hinted.c_str());
            } else {
                fail(i, msg);
            }
            lua_pop(L_, 1);
        } else if (budget_.check_state) {
            state_unchanged(i);
        }
    }
    frame_ = nullptr;
}

}  // namespace mce::script
