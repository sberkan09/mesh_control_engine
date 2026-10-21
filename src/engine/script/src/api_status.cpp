// mce_script (M-STATUS step 2): the `status` global, thin bindings over the kit's StatusEffects (mce/kit/status.h).
// Like `inventory` it keeps no state: changes go in as ApplyStatus / ClearStatus events and take effect on the next
// tick; `has` looks straight at the StatusEffects component. A world without the kit piece gets a named error.
//
//   status.add(e, id, kind, amount, seconds, source?)   queue ApplyStatus. kind is "regen" or "poison" (amount: positive
//                                       hp per second), "armor" or "max_health" (amount: the added value, +-1e6);
//                                       seconds 0.01 to 3600. Adding an id that is active refreshes it. `source` is
//                                       credited for poison damage (default: nobody)
//   status.has(e, id) -> boolean        whether the effect is active on e
//   status.clear(e, id?)                queue ClearStatus (no id: every effect)
//
// Every function may raise (LUA_USE_LONGJMP): nothing with a destructor lives across a luaL_* call.

#include <cmath>
#include <cstring>

#include "api.h"
#include "mce/fixed_string.h"
#include "mce/kit/status.h"

namespace mce::script::detail {

namespace {

World& need_status(lua_State* L, const char* fn) {
    World& w = frame_of(L).world();
    if (!w.event_queue<kit::ApplyStatus>()) luaL_error(L, "%s: this world has no status effects (install_status)", fn);
    return w;
}

FixedString<32> check_effect_id(lua_State* L, int idx, const char* fn) {
    size_t len = 0;
    const char* s = luaL_checklstring(L, idx, &len);
    if (len == 0) luaL_error(L, "%s: the effect id is empty", fn);
    if (len > FixedString<32>::capacity()) luaL_error(L, "%s: the effect id is longer than %d characters", fn, static_cast<int>(FixedString<32>::capacity()));
    FixedString<32> id;
    id.assign(StringView(s, len));
    return id;
}

u32 check_kind(lua_State* L, int idx, const char* fn) {
    const char* s = luaL_checkstring(L, idx);
    if (std::strcmp(s, "regen") == 0) return static_cast<u32>(kit::StatusKind::Regen);
    if (std::strcmp(s, "poison") == 0) return static_cast<u32>(kit::StatusKind::Poison);
    if (std::strcmp(s, "armor") == 0) return static_cast<u32>(kit::StatusKind::Armor);
    if (std::strcmp(s, "max_health") == 0) return static_cast<u32>(kit::StatusKind::MaxHealth);
    luaL_error(L, "%s: unknown kind '%s' (regen, poison, armor, max_health)", fn, s);
    return 0;
}

int l_add(lua_State* L) {
    World& w = need_status(L, "status.add");
    const Entity e = check_entity(L, 1);
    if (!w.alive(e)) luaL_error(L, "status.add: the entity is not alive");
    kit::ApplyStatus a;
    a.target = e;
    a.id = check_effect_id(L, 2, "status.add");
    a.kind = check_kind(L, 3, "status.add");
    const f64 amount = luaL_checknumber(L, 4);
    const f64 seconds = luaL_checknumber(L, 5);
    if (!std::isfinite(amount) || std::fabs(amount) > 1e6) luaL_error(L, "status.add: amount %g is outside +-1000000", amount);
    if (a.kind <= static_cast<u32>(kit::StatusKind::Poison) && !(amount > 0.0)) luaL_error(L, "status.add: regen and poison need a positive amount (got %g)", amount);
    if (!(seconds >= 0.01 && seconds <= kit::kStatusMaxSeconds)) luaL_error(L, "status.add: seconds %g is outside 0.01 to %d", seconds, static_cast<int>(kit::kStatusMaxSeconds));
    a.amount = static_cast<f32>(amount);
    a.seconds = static_cast<f32>(seconds);
    if (!lua_isnoneornil(L, 6)) a.source = check_entity(L, 6);
    frame_of(L).events().push(a);
    return 0;
}

int l_has(lua_State* L) {
    World& w = need_status(L, "status.has");
    const Entity e = check_entity(L, 1);
    const FixedString<32> id = check_effect_id(L, 2, "status.has");
    const kit::StatusEffects* s = w.alive(e) ? w.try_get<kit::StatusEffects>(e) : nullptr;
    lua_pushboolean(L, s && kit::status_find(*s, kit::status_key(id.view())) >= 0 ? 1 : 0);
    return 1;
}

int l_clear(lua_State* L) {
    World& w = need_status(L, "status.clear");
    const Entity e = check_entity(L, 1);
    if (!w.alive(e)) luaL_error(L, "status.clear: the entity is not alive");
    kit::ClearStatus c;
    c.target = e;
    if (!lua_isnoneornil(L, 2)) c.id = check_effect_id(L, 2, "status.clear");
    frame_of(L).events().push(c);
    return 0;
}

const luaL_Reg kStatusLib[] = {{"add", l_add}, {"has", l_has}, {"clear", l_clear}, {nullptr, nullptr}};

}  // namespace

void register_status_api(lua_State* L) {
    lua_newtable(L);
    luaL_register(L, nullptr, kStatusLib);
    lua_setglobal(L, "status");
}

}  // namespace mce::script::detail
