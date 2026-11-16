// mce_script (M-DAYNIGHT step 4a): the `clock` global, a thin binding over the kit's WorldClock component
// (mce/kit/world_clock.h). Like the `mce` functions it holds no state: the hour, the day, the pause and the speed
// live in the WorldClock component, so saves, replays and the editor see them. The functions act on the FIRST
// WorldClock in ECS order (a scene has one). A world without one gets a named error, never a crash.
//
//   clock.hour() -> number     the hour of the day, 0..24
//   clock.day() -> integer     whole days passed since the start
//   clock.set(hour)            writes WorldClock.hour (0..24); the clock re-seats itself on its next tick
//   clock.pause(paused)        WorldClock.paused
//   clock.speed(scale)         WorldClock.time_scale, clamped to 0..100 like the component
//   clock.paused() -> boolean
//
// Every function may raise (LUA_USE_LONGJMP): nothing with a destructor lives across a luaL_* call.

#include <cmath>

#include "api.h"
#include "mce/kit/world_clock.h"

namespace mce::script::detail {

namespace {

// The first WorldClock, or a Luau error that names the call.
kit::WorldClock& check_clock(lua_State* L, const char* fn) {
    World& w = world_of(L);
    const refl::TypeRecord* type = refl::find_type(StringId("WorldClock"));
    if (!type || w.find_component(type->type_hash) == ecs::kInvalidComponent)
        luaL_error(L, "%s: this world has no world clock (install_world_clock)", fn);
    kit::WorldClock* found = nullptr;
    w.query<kit::WorldClock>().each([&](Entity, kit::WorldClock& c) {
        if (!found) found = &c;
    });
    if (!found) luaL_error(L, "%s: the scene has no WorldClock entity", fn);
    return *found;
}

int l_clock_hour(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_clock(L, "clock.hour").hour));
    return 1;
}

int l_clock_day(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_clock(L, "clock.day").day));
    return 1;
}

int l_clock_set(lua_State* L) {
    const f64 h = luaL_checknumber(L, 1);
    if (!(h >= 0.0 && h <= 24.0)) luaL_error(L, "clock.set: the hour %g is not a number from 0 to 24", h);
    check_clock(L, "clock.set").hour = static_cast<f32>(h);
    return 0;
}

int l_clock_pause(lua_State* L) {
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    check_clock(L, "clock.pause").paused = lua_toboolean(L, 1) ? 1 : 0;
    return 0;
}

int l_clock_speed(lua_State* L) {
    f64 s = luaL_checknumber(L, 1);
    if (std::isnan(s)) luaL_error(L, "clock.speed: the scale is not a number");
    s = s < 0.0 ? 0.0 : (s > 100.0 ? 100.0 : s);
    check_clock(L, "clock.speed").time_scale = static_cast<f32>(s);
    return 0;
}

int l_clock_paused(lua_State* L) {
    lua_pushboolean(L, check_clock(L, "clock.paused").paused ? 1 : 0);
    return 1;
}

const luaL_Reg kClockLib[] = {
    {"hour", l_clock_hour},
    {"day", l_clock_day},
    {"set", l_clock_set},
    {"pause", l_clock_pause},
    {"speed", l_clock_speed},
    {"paused", l_clock_paused},
    {nullptr, nullptr},
};

}  // namespace

void register_clock_api(lua_State* L) {
    lua_newtable(L);
    luaL_register(L, nullptr, kClockLib);
    lua_setglobal(L, "clock");
}

}  // namespace mce::script::detail
