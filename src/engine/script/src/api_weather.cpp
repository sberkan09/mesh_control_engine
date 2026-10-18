// mce_script (M-WEATHER step 4a): the `weather` global, a thin binding over the kit's Weather component
// (mce/kit/weather.h). Like `clock` it holds no state: the kind, the target and the blend live in the Weather
// component, so saves, replays and the editor see them. The functions act on the FIRST Weather in ECS order (a scene
// has one). A world without one gets a named error, never a crash.
//
//   weather.kind() -> string       the kind in effect: clear, overcast, rain, storm or snow (Weather.current)
//   weather.target() -> string     the kind the weather is moving to (Weather.target)
//   weather.set(kind, seconds?)    writes Weather.target; with `seconds` also Weather.transition_seconds (0..3600)
//   weather.intensity() -> number  0..1 blend of the kind in effect
//   weather.wind() -> number       0..1
//   weather.wetness() -> number    0..1
//
// Every function may raise (LUA_USE_LONGJMP): nothing with a destructor lives across a luaL_* call.

#include <cmath>
#include <cstring>

#include "api.h"
#include "mce/kit/weather.h"

namespace mce::script::detail {

namespace {

constexpr const char* kKindNames[] = {"clear", "overcast", "rain", "storm", "snow"};
constexpr int kKindCount = 5;

const char* kind_name(kit::WeatherKind k) {
    const usize i = static_cast<usize>(k);
    return i < static_cast<usize>(kKindCount) ? kKindNames[i] : "clear";
}

// The first Weather, or a Luau error that names the call.
kit::Weather& check_weather(lua_State* L, const char* fn) {
    World& w = world_of(L);
    const refl::TypeRecord* type = refl::find_type(StringId("Weather"));
    if (!type || w.find_component(type->type_hash) == ecs::kInvalidComponent)
        luaL_error(L, "%s: this world has no weather (install_weather)", fn);
    kit::Weather* found = nullptr;
    w.query<kit::Weather>().each([&](Entity, kit::Weather& c) {
        if (!found) found = &c;
    });
    if (!found) luaL_error(L, "%s: the scene has no Weather entity", fn);
    return *found;
}

int l_weather_kind(lua_State* L) {
    lua_pushstring(L, kind_name(check_weather(L, "weather.kind").current));
    return 1;
}

int l_weather_target(lua_State* L) {
    lua_pushstring(L, kind_name(check_weather(L, "weather.target").target));
    return 1;
}

int l_weather_set(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    int kind = -1;
    for (int i = 0; i < kKindCount; ++i) {
        if (std::strcmp(name, kKindNames[i]) == 0) kind = i;
    }
    if (kind < 0) luaL_error(L, "weather.set: unknown weather '%s' (clear, overcast, rain, storm, snow)", name);
    bool has_seconds = false;
    f64 seconds = 0.0;
    if (!lua_isnoneornil(L, 2)) {
        seconds = luaL_checknumber(L, 2);
        if (std::isnan(seconds)) luaL_error(L, "weather.set: the transition seconds are not a number");
        seconds = seconds < 0.0 ? 0.0 : (seconds > 3600.0 ? 3600.0 : seconds);
        has_seconds = true;
    }
    kit::Weather& wx = check_weather(L, "weather.set");
    wx.target = static_cast<kit::WeatherKind>(kind);
    if (has_seconds) wx.transition_seconds = static_cast<f32>(seconds);
    return 0;
}

int l_weather_intensity(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_weather(L, "weather.intensity").intensity));
    return 1;
}

int l_weather_wind(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_weather(L, "weather.wind").wind));
    return 1;
}

int l_weather_wetness(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_weather(L, "weather.wetness").wetness));
    return 1;
}

const luaL_Reg kWeatherLib[] = {
    {"kind", l_weather_kind},
    {"target", l_weather_target},
    {"set", l_weather_set},
    {"intensity", l_weather_intensity},
    {"wind", l_weather_wind},
    {"wetness", l_weather_wetness},
    {nullptr, nullptr},
};

}  // namespace

void register_weather_api(lua_State* L) {
    lua_newtable(L);
    luaL_register(L, nullptr, kWeatherLib);
    lua_setglobal(L, "weather");
}

}  // namespace mce::script::detail
