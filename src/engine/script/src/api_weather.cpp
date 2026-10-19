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
// Wet surfaces (M-WETSURFACE step 5d) are a thin binding over the first kit::WetSurfaces (mce/kit/wet_surfaces.h),
// which only tunes the look of the wet world. A world without one gets a named error.
//
//   weather.wet_strength() -> number      WetSurfaces.strength (0..4), scales Weather.wetness
//   weather.wet_darken() -> number        WetSurfaces.darken (0..1)
//   weather.wet_puddles() -> number       WetSurfaces.puddles (0..1)
//   weather.wet_puddle_scale() -> number  WetSurfaces.puddle_scale_m (0.1..100)
//   weather.set_wet{strength=, darken=, puddles=, puddle_scale=}
//                                         a table, every key optional, each value clamped to the field's Range;
//                                         an unknown key or a non-number raises an error that names the call
//
// Wind sway (M-WINDSWAY step 2) is the same kind of binding over the first kit::WindSway (mce/kit/wind_sway.h): the
// gains Kit3DExtract scales the foliage wind by (factor 1 + (gain - 1) * Weather.wind). Named errors as above.
//
//   weather.sway_amplitude() -> number    WindSway.amplitude_gain (0..50)
//   weather.sway_frequency() -> number    WindSway.frequency_gain (0..10)
//   weather.sway_gust() -> number         WindSway.gust_gain (-1..1)
//   weather.sway_flutter() -> number      WindSway.flutter_gain (0..50)
//   weather.set_sway{amplitude=, frequency=, gust=, flutter=}
//                                         a table, every key optional, each value clamped to the field's Range;
//                                         an unknown key or a non-number raises an error that names the call
//
// Every function may raise (LUA_USE_LONGJMP): nothing with a destructor lives across a luaL_* call.

#include <cmath>
#include <cstring>

#include "api.h"
#include "mce/kit/weather.h"
#include "mce/kit/wind_sway.h"
#include "mce/kit/wet_surfaces.h"

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

// The first WetSurfaces, or a Luau error that names the call.
kit::WetSurfaces& check_wet(lua_State* L, const char* fn) {
    World& w = world_of(L);
    const refl::TypeRecord* type = refl::find_type(StringId("WetSurfaces"));
    if (!type || w.find_component(type->type_hash) == ecs::kInvalidComponent)
        luaL_error(L, "%s: this world has no weather (install_weather)", fn);
    kit::WetSurfaces* found = nullptr;
    w.query<kit::WetSurfaces>().each([&](Entity, kit::WetSurfaces& c) {
        if (!found) found = &c;
    });
    if (!found) luaL_error(L, "%s: the scene has no WetSurfaces entity", fn);
    return *found;
}

// The first WindSway, or a Luau error that names the call.
kit::WindSway& check_sway(lua_State* L, const char* fn) {
    World& w = world_of(L);
    const refl::TypeRecord* type = refl::find_type(StringId("WindSway"));
    if (!type || w.find_component(type->type_hash) == ecs::kInvalidComponent)
        luaL_error(L, "%s: this world has no weather (install_weather)", fn);
    kit::WindSway* found = nullptr;
    w.query<kit::WindSway>().each([&](Entity, kit::WindSway& c) {
        if (!found) found = &c;
    });
    if (!found) luaL_error(L, "%s: the scene has no WindSway entity", fn);
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

int l_weather_wet_strength(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_wet(L, "weather.wet_strength").strength));
    return 1;
}

int l_weather_wet_darken(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_wet(L, "weather.wet_darken").darken));
    return 1;
}

int l_weather_wet_puddles(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_wet(L, "weather.wet_puddles").puddles));
    return 1;
}

int l_weather_wet_puddle_scale(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_wet(L, "weather.wet_puddle_scale").puddle_scale_m));
    return 1;
}

f64 clamp_range(f64 v, f64 lo, f64 hi) { return v < lo ? lo : (v > hi ? hi : v); }

// weather.set_wet{strength=, darken=, puddles=, puddle_scale=}: every key optional, validated before anything is
// written (an error leaves the component as it was), values clamped to the component's Range.
int l_weather_set_wet(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    // Slots: 0 strength, 1 darken, 2 puddles, 3 puddle_scale.
    static const char* const kKeys[4] = {"strength", "darken", "puddles", "puddle_scale"};
    f64 value[4] = {0, 0, 0, 0};
    bool given[4] = {false, false, false, false};
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING)
            luaL_error(L, "weather.set_wet: the keys are names (strength, darken, puddles, puddle_scale)");
        const char* key = lua_tostring(L, -2);
        int slot = -1;
        for (int i = 0; i < 4; ++i) {
            if (std::strcmp(key, kKeys[i]) == 0) slot = i;
        }
        if (slot < 0)
            luaL_error(L, "weather.set_wet: unknown key '%s' (strength, darken, puddles, puddle_scale)", key);
        if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "weather.set_wet: '%s' is not a number", key);
        const f64 v = lua_tonumber(L, -1);
        if (std::isnan(v)) luaL_error(L, "weather.set_wet: '%s' is not a number", key);
        value[slot] = v;
        given[slot] = true;
        lua_pop(L, 1);
    }
    kit::WetSurfaces& ws = check_wet(L, "weather.set_wet");
    if (given[0]) ws.strength = static_cast<f32>(clamp_range(value[0], 0.0, 4.0));
    if (given[1]) ws.darken = static_cast<f32>(clamp_range(value[1], 0.0, 1.0));
    if (given[2]) ws.puddles = static_cast<f32>(clamp_range(value[2], 0.0, 1.0));
    if (given[3]) ws.puddle_scale_m = static_cast<f32>(clamp_range(value[3], 0.1, 100.0));
    return 0;
}

int l_weather_sway_amplitude(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_sway(L, "weather.sway_amplitude").amplitude_gain));
    return 1;
}

int l_weather_sway_frequency(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_sway(L, "weather.sway_frequency").frequency_gain));
    return 1;
}

int l_weather_sway_gust(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_sway(L, "weather.sway_gust").gust_gain));
    return 1;
}

int l_weather_sway_flutter(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_sway(L, "weather.sway_flutter").flutter_gain));
    return 1;
}

// weather.set_sway{amplitude=, frequency=, gust=, flutter=}: like set_wet, validated before anything is written.
int l_weather_set_sway(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    // Slots: 0 amplitude, 1 frequency, 2 gust, 3 flutter.
    static const char* const kKeys[4] = {"amplitude", "frequency", "gust", "flutter"};
    f64 value[4] = {0, 0, 0, 0};
    bool given[4] = {false, false, false, false};
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING)
            luaL_error(L, "weather.set_sway: the keys are names (amplitude, frequency, gust, flutter)");
        const char* key = lua_tostring(L, -2);
        int slot = -1;
        for (int i = 0; i < 4; ++i) {
            if (std::strcmp(key, kKeys[i]) == 0) slot = i;
        }
        if (slot < 0)
            luaL_error(L, "weather.set_sway: unknown key '%s' (amplitude, frequency, gust, flutter)", key);
        if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "weather.set_sway: '%s' is not a number", key);
        const f64 v = lua_tonumber(L, -1);
        if (std::isnan(v)) luaL_error(L, "weather.set_sway: '%s' is not a number", key);
        value[slot] = v;
        given[slot] = true;
        lua_pop(L, 1);
    }
    kit::WindSway& ws = check_sway(L, "weather.set_sway");
    if (given[0]) ws.amplitude_gain = static_cast<f32>(clamp_range(value[0], 0.0, 50.0));
    if (given[1]) ws.frequency_gain = static_cast<f32>(clamp_range(value[1], 0.0, 10.0));
    if (given[2]) ws.gust_gain = static_cast<f32>(clamp_range(value[2], -1.0, 1.0));
    if (given[3]) ws.flutter_gain = static_cast<f32>(clamp_range(value[3], 0.0, 50.0));
    return 0;
}

const luaL_Reg kWeatherLib[] = {
    {"kind", l_weather_kind},
    {"target", l_weather_target},
    {"set", l_weather_set},
    {"intensity", l_weather_intensity},
    {"wind", l_weather_wind},
    {"wetness", l_weather_wetness},
    {"wet_strength", l_weather_wet_strength},
    {"wet_darken", l_weather_wet_darken},
    {"wet_puddles", l_weather_wet_puddles},
    {"wet_puddle_scale", l_weather_wet_puddle_scale},
    {"set_wet", l_weather_set_wet},
    {"sway_amplitude", l_weather_sway_amplitude},
    {"sway_frequency", l_weather_sway_frequency},
    {"sway_gust", l_weather_sway_gust},
    {"sway_flutter", l_weather_sway_flutter},
    {"set_sway", l_weather_set_sway},
    {nullptr, nullptr},
};

}  // namespace

void register_weather_api(lua_State* L) {
    lua_newtable(L);
    luaL_register(L, nullptr, kWeatherLib);
    lua_setglobal(L, "weather");
}

}  // namespace mce::script::detail
