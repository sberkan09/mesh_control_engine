#pragma once
// mce_script internals: what the `mce` API's C functions share (script.cpp, api_world.cpp).
//
// Every lua_CFunction may longjmp out (LUA_USE_LONGJMP): no object with a destructor lives across a
// call that can raise (luaL_check*, luaL_error, lua_call). Scratch bytes live in the host.

#include "lua.h"
#include "lualib.h"

#include "mce/refl/type.h"
#include "mce/script/script.h"

namespace mce::script {

// The C functions reach the host's per-run context through this (a friend of ScriptHost).
struct HostAccess {
    using FieldHandle = ScriptHost::FieldHandle;
    using StateWatch = ScriptHost::StateWatch;
    static ScriptHost& host(lua_State* L) { return *static_cast<ScriptHost*>(lua_callbacks(L)->userdata); }
    static Frame* frame(ScriptHost& h) { return h.frame_; }
    static u32 current(ScriptHost& h) { return h.current_; }
    static u32 next_random(ScriptHost& h) { return h.random_calls_++; }
    static u64 seed(ScriptHost& h) { return h.seed_; }
    static asset::AssetSystem* assets(ScriptHost& h) { return h.assets_; }
    static Array<FieldHandle>& handles(ScriptHost& h) { return h.handles_; }
    static Array<u8>& scratch(ScriptHost& h) { return h.scratch_; }
    static Array<Entity>& entities(ScriptHost& h) { return h.entity_scratch_; }
};

namespace detail {

// The running frame; raises outside update().
Frame& frame_of(lua_State* L);
inline World& world_of(lua_State* L) { return frame_of(L).world(); }

// Entity <-> Lua number: index in the low 32 bits, generation above (exact below 2^53). The null
// entity is nil.
void push_entity(lua_State* L, Entity e);
Entity check_entity(lua_State* L, int idx);
Entity opt_entity(lua_State* L, int idx);  // nil -> Entity::null

// A reflected type by name; `want` is TypeFlags::Component or TypeFlags::Event. Raises when unknown.
const refl::TypeRecord& check_type(lua_State* L, int idx, u32 want);
// The type's ComponentId in `w`, registering it from its record when the World has not seen it.
ecs::ComponentId component_id(World& w, const refl::TypeRecord& type);
ecs::ComponentId check_component(lua_State* L, World& w, int idx);

// One field's value at `p` -> Lua values (returns how many were pushed), and back.
int push_value(lua_State* L, const refl::FieldRecord& f, const u8* p);
void write_value(lua_State* L, const refl::FieldRecord& f, u8* p, int arg);
// A whole struct as a table of its fields, and a table's named fields written into `p`.
void push_struct(lua_State* L, const refl::TypeRecord& type, const u8* p);
void write_struct(lua_State* L, const refl::TypeRecord& type, u8* p, int table_idx);

// Default-constructed bytes of `type` in the host's scratch buffer at `offset` (8-aligned);
// returns the pointer (valid until the scratch grows).
u8* scratch_default(ScriptHost& host, const refl::TypeRecord& type, usize offset);

// Adds the entity, events, input, scene and physics functions to the table at the stack top.
void register_world_api(lua_State* L);
// The `mission` and `marker` globals (api_missions.cpp).
void register_mission_api(lua_State* L);
// The `inventory` global (api_inventory.cpp).
void register_inventory_api(lua_State* L);

// The `status` global (api_status.cpp).
void register_status_api(lua_State* L);
// The `clock` global (api_clock.cpp).
void register_clock_api(lua_State* L);
// The `weather` global (api_weather.cpp).
void register_weather_api(lua_State* L);

}  // namespace detail
}  // namespace mce::script
