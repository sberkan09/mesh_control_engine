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
// Snow cover (M-SNOWCOVER step 6d) is the same kind of binding over the first kit::SnowCover (mce/kit/snow_cover.h).
// Named errors as above.
//
//   weather.snow_cover() -> number        SnowCover.cover (0..1), engine-written by kit_snow_cover; read-only
//   weather.snow_strength() -> number     SnowCover.strength (0..4)
//   weather.snow_build() -> number        SnowCover.build_seconds (0..86400)
//   weather.snow_melt() -> number         SnowCover.melt_seconds (0..86400)
//   weather.snow_coverage() -> number     SnowCover.coverage (0..1)
//   weather.snow_scale() -> number        SnowCover.noise_scale_m (0.1..100)
//   weather.snow_wind() -> number         SnowCover.wind_bias (0..1)
//   weather.set_snow{strength=, build=, melt=, coverage=, scale=, wind=}
//                                         a table, every key optional, each value clamped to the field's Range;
//                                         an unknown key or a non-number raises an error that names the call
//
// Rain effects (M-RAINFX step 7d) are the same kind of binding over the first kit::RainRipples (mce/kit/rain_ripples.h),
// kit::RainSplash (mce/kit/rain_splash.h) and kit::ThunderDelay (mce/kit/thunder_delay.h). Named errors as above.
// The engine-written fields (RainSplash.counter, ThunderDelay.pending, due, gain_db) and ThunderDelay.clip are not
// reachable from Luau.
//
//   weather.ripples_strength() -> number  RainRipples.strength (0..1)
//   weather.ripples_density() -> number   RainRipples.density (0..1)
//   weather.ripples_speed() -> number     RainRipples.speed (0.1..10)
//   weather.ripples_ring_width() -> number RainRipples.ring_width (0..1)
//   weather.set_ripples{strength=, density=, speed=, ring_width=}
//   weather.splash_rate() -> number       RainSplash.rate (0..64)
//   weather.splash_height() -> number     RainSplash.height_m (0.01..5)
//   weather.splash_life() -> number       RainSplash.life_seconds (0.02..5)
//   weather.splash_radius() -> number     RainSplash.radius_m (0.5..500)
//   weather.splash_seed() -> number       RainSplash.seed (0..4294967295)
//   weather.set_splash{rate=, height=, life=, radius=, seed=}
//   weather.thunder_speed() -> number     ThunderDelay.speed_mps (1..1000000)
//   weather.thunder_volume() -> number    ThunderDelay.volume (0..4)
//   weather.set_thunder{speed=, volume=}
//                                         each setter takes a table, every key optional, each value clamped to the
//                                         field's Range; an unknown key or a non-number raises an error that names
//                                         the call
//
// Wind direction (M-WINDDIR step 8e) is the same kind of binding over the first kit::WindDirection
// (mce/kit/wind_direction.h). Named errors as above. Only the authored degrees and drift are settable: angle_q, frac
// and seeded are engine-written (kit_wind_direction) and an unknown key names them.
//
//   weather.wind_degrees() -> number      WindDirection.degrees (0..360), the authored heading
//   weather.wind_drift() -> number        WindDirection.drift_degrees_per_minute (-3600..3600)
//   weather.wind_heading() -> number      the resolved heading in degrees, 0..360 (from the engine-written angle_q)
//   weather.set_wind_direction{degrees=, drift=}
//                                         a table, every key optional, each value clamped to the field's Range;
//                                         an unknown key or a non-number raises an error that names the call
//
// Clouds (M-CLOUDS step 9d) are the same kind of binding over the first kit::CloudLayer (mce/kit/cloud_layer.h). Named
// errors as above. All six fields are authoring, so all are settable.
//
//   weather.cloud_strength() -> number           CloudLayer.strength (0..1)
//   weather.cloud_fair_coverage() -> number      CloudLayer.fair_coverage (0..1)
//   weather.cloud_overcast_coverage() -> number  CloudLayer.overcast_coverage (0..1)
//   weather.cloud_scale() -> number              CloudLayer.scale_m (50..20000)
//   weather.cloud_speed_scale() -> number        CloudLayer.speed_scale (0..10)
//   weather.cloud_height() -> number             CloudLayer.height_m (100..20000)
//   weather.set_clouds{strength=, fair_coverage=, overcast_coverage=, scale=, speed_scale=, height=}
//                                         a table, every key optional, each value clamped to the field's Range;
//                                         an unknown key or a non-number raises an error that names the call
//
// Rain and wind ambience (M-WEATHERAUDIO step 10c) is the same kind of binding over the first kit::WeatherAudio
// (mce/kit/weather_audio.h). Named errors as above. The two sources and the fade levels belong to the scene and the
// engine, so they are not settable; the levels are read-only.
//
//   weather.rain_level() -> number        the current rain bed level (WeatherAudio.rain_q / 2^24, 0..1)
//   weather.wind_level() -> number        the current wind bed level (WeatherAudio.wind_q / 2^24, 0..1)
//   weather.audio_rain_db() -> number     WeatherAudio.rain_db (-60..12)
//   weather.audio_wind_db() -> number     WeatherAudio.wind_db (-60..12)
//   weather.audio_fade() -> number        WeatherAudio.fade_seconds (0..60)
//   weather.audio_enabled() -> number     WeatherAudio.enabled (0 or 1)
//   weather.set_audio{rain_db=, wind_db=, fade=, enabled=}
//                                         a table, every key optional, each value clamped to the field's Range
//                                         (enabled: any non-zero number is 1); an unknown key or a non-number
//                                         raises an error that names the call
//
// Lightning bolts (M-LIGHTNINGBOLT step 11d) are the same kind of binding over the first kit::LightningBolt
// (mce/kit/lightning_bolt.h). Named errors as above; the last strike belongs to the engine and is read-only.
//
//   weather.bolt_min_range() / bolt_max_range() -> number   LightningBolt.min_range_m / max_range_m (10..5000)
//   weather.bolt_ground_y() -> number                       LightningBolt.ground_y
//   weather.bolt_height() -> number                         LightningBolt.height_m (50..3000)
//   weather.bolt_width() -> number                          LightningBolt.width_m (0.05..20)
//   weather.bolt_segments() -> number                       LightningBolt.segments_log2 (2..7)
//   weather.bolt_jitter() -> number                         LightningBolt.jitter (0..0.6)
//   weather.bolt_fork() / bolt_enabled() -> number          0 or 1
//   weather.bolt_light() / bolt_light_radius() -> number    light_intensity (0..100000) / light_radius_m (1..5000)
//   weather.bolt_distance() -> number                       strike_dist_m, the last strike's distance from the camera
//   weather.bolt_pos() -> x, z                              strike_x, strike_z, the last strike's ground position
//   weather.bolt_active() -> number                         active, the flash ticks left
//   weather.set_bolt{min_range=, max_range=, ground_y=, height=, width=, segments=, jitter=, fork=, light=,
//                    light_radius=, enabled=}
//                                         a table, every key optional, each value clamped to the field's Range
//                                         (segments rounded down; fork and enabled: any non-zero number is 1); an
//                                         unknown key or a non-number raises an error that names the call
//
// Wind gusts (M-WINDGUST step 12d) are the same kind of binding over the first kit::WindGust
// (mce/kit/wind_gust.h). Named errors as above; the setter never adds the component, and the gust state is
// engine-written and read-only.
//
//   weather.gust_strength() -> number                       WindGust.strength (0..3)
//   weather.gust_min_gap() / gust_max_gap() -> number       min_gap_s / max_gap_s (0..120)
//   weather.gust_min_rise() / gust_max_rise() -> number     min_rise_s / max_rise_s (0.05..30)
//   weather.gust_min_fall() / gust_max_fall() -> number     min_fall_s / max_fall_s (0.05..60)
//   weather.gust_min_peak() / gust_max_peak() -> number     min_peak / max_peak (0..1)
//   weather.gust_seed() / gust_affects() -> number          seed / affects (u32)
//   weather.gust_enabled() -> number                        0 or 1
//   weather.gust_level() -> number                          gust_q / 2^24 (0..1), read-only
//   weather.effective_wind() -> number                      effective_wind(Weather, WindGust, all consumer bits)
//   weather.set_gust{strength=, min_gap=, max_gap=, min_rise=, max_rise=, min_fall=, max_fall=, min_peak=,
//                    max_peak=, seed=, affects=, enabled=}
//                                         a table, every key optional, each value clamped to the field's Range
//                                         (seed and affects rounded down; enabled: any non-zero number is 1); an
//                                         unknown key or a non-number raises an error that names the call
//
// Ground mist (M-GROUNDMIST step 13c) is the same kind of binding over the first kit::GroundMist
// (mce/kit/ground_mist.h). Named errors as above; the setter never adds the component. mist_level is derived, read-only.
//
//   weather.mist_strength() -> number                       GroundMist.strength (0..2)
//   weather.mist_peak_hour() -> number                      peak_hour (0..24)
//   weather.mist_rise() / mist_burn() -> number             rise_hours / burn_hours (0.1..12)
//   weather.mist_base_humidity() / mist_wet_gain() -> number  base_humidity / wet_gain (0..1)
//   weather.mist_weather_damp() -> number                   weather_damp (0..1)
//   weather.mist_density() -> number                        density (0..1)
//   weather.mist_thickness() -> number                      thickness_m (0.5..200)
//   weather.mist_base_height() -> number                    base_height_m (-1000..1000)
//   weather.mist_exp2_density() -> number                   exp2_density (0..0.2)
//   weather.mist_color() -> r, g, b                         color
//   weather.mist_enabled() -> number                        0 or 1
//   weather.mist_level() -> number                          kit::mist_level on Environment3D.time_of_day and the first
//                                                           Weather (none: no damping), 0 without a clock; read-only
//   weather.set_mist{strength=, peak_hour=, rise_hours=, burn_hours=, base_humidity=, wet_gain=, weather_damp=,
//                    density=, thickness=, base_height=, exp2_density=, color={r,g,b}, enabled=}
//                                         a table, every key optional, each value clamped to the field's Range (color
//                                         lanes 0..1; enabled: any non-zero number is 1); an unknown key, a non-number
//                                         or a color that is not three numbers raises an error that names the call
//
// Every function may raise (LUA_USE_LONGJMP): nothing with a destructor lives across a luaL_* call.

#include <cmath>
#include <cstring>

#include "api.h"
#include "mce/kit/cloud_layer.h"
#include "mce/kit/rain_ripples.h"
#include "mce/kit/rain_splash.h"
#include "mce/kit/snow_cover.h"
#include "mce/kit/thunder_delay.h"
#include "mce/kit/wind_direction.h"
#include "mce/kit/weather.h"
#include "mce/kit/wind_sway.h"
#include "mce/kit/wet_surfaces.h"
#include "mce/kit/weather_audio.h"
#include "mce/kit/lightning_bolt.h"
#include "mce/kit/wind_gust.h"
#include "mce/kit/ground_mist.h"
#include "mce/kit/kit3d.h"

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

// The first SnowCover, or a Luau error that names the call.
kit::SnowCover& check_snow(lua_State* L, const char* fn) {
    World& w = world_of(L);
    const refl::TypeRecord* type = refl::find_type(StringId("SnowCover"));
    if (!type || w.find_component(type->type_hash) == ecs::kInvalidComponent)
        luaL_error(L, "%s: this world has no weather (install_weather)", fn);
    kit::SnowCover* found = nullptr;
    w.query<kit::SnowCover>().each([&](Entity, kit::SnowCover& c) {
        if (!found) found = &c;
    });
    if (!found) luaL_error(L, "%s: the scene has no SnowCover entity", fn);
    return *found;
}

// The first component of type T (registered under `name`), or a Luau error that names the call.
template <typename T>
T& check_first(lua_State* L, const char* fn, const char* name) {
    World& w = world_of(L);
    const refl::TypeRecord* type = refl::find_type(StringId(name));
    if (!type || w.find_component(type->type_hash) == ecs::kInvalidComponent)
        luaL_error(L, "%s: this world has no weather (install_weather)", fn);
    T* found = nullptr;
    w.query<T>().each([&](Entity, T& c) {
        if (!found) found = &c;
    });
    if (!found) luaL_error(L, "%s: the scene has no %s entity", fn, name);
    return *found;
}

kit::RainRipples& check_ripples(lua_State* L, const char* fn) { return check_first<kit::RainRipples>(L, fn, "RainRipples"); }
kit::RainSplash& check_splash(lua_State* L, const char* fn) { return check_first<kit::RainSplash>(L, fn, "RainSplash"); }
kit::ThunderDelay& check_thunder(lua_State* L, const char* fn) { return check_first<kit::ThunderDelay>(L, fn, "ThunderDelay"); }
kit::WindDirection& check_wind_direction(lua_State* L, const char* fn) {
    return check_first<kit::WindDirection>(L, fn, "WindDirection");
}

kit::CloudLayer& check_clouds(lua_State* L, const char* fn) { return check_first<kit::CloudLayer>(L, fn, "CloudLayer"); }

kit::WeatherAudio& check_audio(lua_State* L, const char* fn) { return check_first<kit::WeatherAudio>(L, fn, "WeatherAudio"); }

kit::LightningBolt& check_bolt(lua_State* L, const char* fn) { return check_first<kit::LightningBolt>(L, fn, "LightningBolt"); }

kit::WindGust& check_gust(lua_State* L, const char* fn) { return check_first<kit::WindGust>(L, fn, "WindGust"); }

kit::GroundMist& check_mist(lua_State* L, const char* fn) { return check_first<kit::GroundMist>(L, fn, "GroundMist"); }

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

int l_weather_snow_cover(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_snow(L, "weather.snow_cover").cover));
    return 1;
}

int l_weather_snow_strength(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_snow(L, "weather.snow_strength").strength));
    return 1;
}

int l_weather_snow_build(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_snow(L, "weather.snow_build").build_seconds));
    return 1;
}

int l_weather_snow_melt(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_snow(L, "weather.snow_melt").melt_seconds));
    return 1;
}

int l_weather_snow_coverage(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_snow(L, "weather.snow_coverage").coverage));
    return 1;
}

int l_weather_snow_scale(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_snow(L, "weather.snow_scale").noise_scale_m));
    return 1;
}

int l_weather_snow_wind(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_snow(L, "weather.snow_wind").wind_bias));
    return 1;
}

// weather.set_snow{strength=, build=, melt=, coverage=, scale=, wind=}: like set_wet, validated before anything is
// written. (`cover` and `snow_q` are engine-written and not settable.)
int l_weather_set_snow(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    // Slots: 0 strength, 1 build, 2 melt, 3 coverage, 4 scale, 5 wind.
    static const char* const kKeys[6] = {"strength", "build", "melt", "coverage", "scale", "wind"};
    f64 value[6] = {0, 0, 0, 0, 0, 0};
    bool given[6] = {false, false, false, false, false, false};
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING)
            luaL_error(L, "weather.set_snow: the keys are names (strength, build, melt, coverage, scale, wind)");
        const char* key = lua_tostring(L, -2);
        int slot = -1;
        for (int i = 0; i < 6; ++i) {
            if (std::strcmp(key, kKeys[i]) == 0) slot = i;
        }
        if (slot < 0)
            luaL_error(L, "weather.set_snow: unknown key '%s' (strength, build, melt, coverage, scale, wind)", key);
        if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "weather.set_snow: '%s' is not a number", key);
        const f64 v = lua_tonumber(L, -1);
        if (std::isnan(v)) luaL_error(L, "weather.set_snow: '%s' is not a number", key);
        value[slot] = v;
        given[slot] = true;
        lua_pop(L, 1);
    }
    kit::SnowCover& sc = check_snow(L, "weather.set_snow");
    if (given[0]) sc.strength = static_cast<f32>(clamp_range(value[0], 0.0, 4.0));
    if (given[1]) sc.build_seconds = static_cast<f32>(clamp_range(value[1], 0.0, 86400.0));
    if (given[2]) sc.melt_seconds = static_cast<f32>(clamp_range(value[2], 0.0, 86400.0));
    if (given[3]) sc.coverage = static_cast<f32>(clamp_range(value[3], 0.0, 1.0));
    if (given[4]) sc.noise_scale_m = static_cast<f32>(clamp_range(value[4], 0.1, 100.0));
    if (given[5]) sc.wind_bias = static_cast<f32>(clamp_range(value[5], 0.0, 1.0));
    return 0;
}

// Reads the settings table at index 1 into value[]/given[] (slot i is keys[i]); raises on a non-name key, an unknown
// key or a non-number (NaN included) before anything is written. `list` is the key list for the messages.
void read_settings(lua_State* L, const char* fn, const char* const* keys, int n, const char* list, f64* value,
                   bool* given, int vec3_slot = -1, f64* vec3 = nullptr) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "%s: the keys are names (%s)", fn, list);
        const char* key = lua_tostring(L, -2);
        int slot = -1;
        for (int i = 0; i < n; ++i) {
            if (std::strcmp(key, keys[i]) == 0) slot = i;
        }
        if (slot < 0) luaL_error(L, "%s: unknown key '%s' (%s)", fn, key, list);
        if (slot == vec3_slot && vec3) {  // a {r, g, b} table of three numbers
            if (lua_type(L, -1) != LUA_TTABLE) luaL_error(L, "%s: '%s' is not a table of three numbers", fn, key);
            for (int i = 0; i < 3; ++i) {
                lua_rawgeti(L, -1, i + 1);
                const f64 c = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : std::nan("");
                lua_pop(L, 1);
                if (std::isnan(c)) luaL_error(L, "%s: '%s' is not a table of three numbers", fn, key);
                vec3[i] = c;
            }
            given[slot] = true;
            lua_pop(L, 1);
            continue;
        }
        if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "%s: '%s' is not a number", fn, key);
        const f64 v = lua_tonumber(L, -1);
        if (std::isnan(v)) luaL_error(L, "%s: '%s' is not a number", fn, key);
        value[slot] = v;
        given[slot] = true;
        lua_pop(L, 1);
    }
}

#define MCE_WEATHER_GETTER(luaname, check, field)                                                  \
    int l_weather_##luaname(lua_State* L) {                                                       \
        lua_pushnumber(L, static_cast<double>(check(L, "weather." #luaname).field));              \
        return 1;                                                                                  \
    }
MCE_WEATHER_GETTER(ripples_strength, check_ripples, strength)
MCE_WEATHER_GETTER(ripples_density, check_ripples, density)
MCE_WEATHER_GETTER(ripples_speed, check_ripples, speed)
MCE_WEATHER_GETTER(ripples_ring_width, check_ripples, ring_width)
MCE_WEATHER_GETTER(splash_rate, check_splash, rate)
MCE_WEATHER_GETTER(splash_height, check_splash, height_m)
MCE_WEATHER_GETTER(splash_life, check_splash, life_seconds)
MCE_WEATHER_GETTER(splash_radius, check_splash, radius_m)
MCE_WEATHER_GETTER(splash_seed, check_splash, seed)
MCE_WEATHER_GETTER(thunder_speed, check_thunder, speed_mps)
MCE_WEATHER_GETTER(thunder_volume, check_thunder, volume)
#undef MCE_WEATHER_GETTER

// weather.set_ripples{strength=, density=, speed=, ring_width=}: validated before anything is written.
int l_weather_set_ripples(lua_State* L) {
    static const char* const kKeys[4] = {"strength", "density", "speed", "ring_width"};
    f64 value[4] = {0, 0, 0, 0};
    bool given[4] = {false, false, false, false};
    read_settings(L, "weather.set_ripples", kKeys, 4, "strength, density, speed, ring_width", value, given);
    kit::RainRipples& r = check_ripples(L, "weather.set_ripples");
    if (given[0]) r.strength = static_cast<f32>(clamp_range(value[0], 0.0, 1.0));
    if (given[1]) r.density = static_cast<f32>(clamp_range(value[1], 0.0, 1.0));
    if (given[2]) r.speed = static_cast<f32>(clamp_range(value[2], 0.1, 10.0));
    if (given[3]) r.ring_width = static_cast<f32>(clamp_range(value[3], 0.0, 1.0));
    return 0;
}

// weather.set_splash{rate=, height=, life=, radius=, seed=}. (`counter` is engine-written and not settable.)
int l_weather_set_splash(lua_State* L) {
    static const char* const kKeys[5] = {"rate", "height", "life", "radius", "seed"};
    f64 value[5] = {0, 0, 0, 0, 0};
    bool given[5] = {false, false, false, false, false};
    read_settings(L, "weather.set_splash", kKeys, 5, "rate, height, life, radius, seed", value, given);
    kit::RainSplash& s = check_splash(L, "weather.set_splash");
    if (given[0]) s.rate = static_cast<f32>(clamp_range(value[0], 0.0, 64.0));
    if (given[1]) s.height_m = static_cast<f32>(clamp_range(value[1], 0.01, 5.0));
    if (given[2]) s.life_seconds = static_cast<f32>(clamp_range(value[2], 0.02, 5.0));
    if (given[3]) s.radius_m = static_cast<f32>(clamp_range(value[3], 0.5, 500.0));
    if (given[4]) s.seed = static_cast<u32>(std::floor(clamp_range(value[4], 0.0, 4294967295.0)));
    return 0;
}

// weather.set_thunder{speed=, volume=}. (The clip, pending, due and gain_db are not settable.)
int l_weather_set_thunder(lua_State* L) {
    static const char* const kKeys[2] = {"speed", "volume"};
    f64 value[2] = {0, 0};
    bool given[2] = {false, false};
    read_settings(L, "weather.set_thunder", kKeys, 2, "speed, volume", value, given);
    kit::ThunderDelay& t = check_thunder(L, "weather.set_thunder");
    if (given[0]) t.speed_mps = static_cast<f32>(clamp_range(value[0], 1.0, 1000000.0));
    if (given[1]) t.volume = static_cast<f32>(clamp_range(value[1], 0.0, 4.0));
    return 0;
}

#define MCE_WEATHER_GETTER_WD(luaname, field)                                                       \
    int l_weather_##luaname(lua_State* L) {                                                         \
        lua_pushnumber(L, static_cast<double>(check_wind_direction(L, "weather." #luaname).field)); \
        return 1;                                                                                   \
    }
MCE_WEATHER_GETTER_WD(wind_degrees, degrees)
MCE_WEATHER_GETTER_WD(wind_drift, drift_degrees_per_minute)
#undef MCE_WEATHER_GETTER_WD

// weather.wind_heading(): the resolved heading in degrees from the quantized angle_q (1/65536 degree), 0..360.
int l_weather_wind_heading(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_wind_direction(L, "weather.wind_heading").angle_q) / 65536.0);
    return 1;
}

// weather.set_wind_direction{degrees=, drift=}: validated before anything is written. (angle_q, frac and seeded are
// engine-written and not settable.)
int l_weather_set_wind_direction(lua_State* L) {
    static const char* const kKeys[2] = {"degrees", "drift"};
    f64 value[2] = {0, 0};
    bool given[2] = {false, false};
    read_settings(L, "weather.set_wind_direction", kKeys, 2, "degrees, drift", value, given);
    kit::WindDirection& d = check_wind_direction(L, "weather.set_wind_direction");
    if (given[0]) d.degrees = static_cast<f32>(clamp_range(value[0], 0.0, 360.0));
    if (given[1]) d.drift_degrees_per_minute = static_cast<f32>(clamp_range(value[1], -3600.0, 3600.0));
    return 0;
}

#define MCE_WEATHER_GETTER_CL(luaname, field)                                                       \
    int l_weather_##luaname(lua_State* L) {                                                         \
        lua_pushnumber(L, static_cast<double>(check_clouds(L, "weather." #luaname).field));         \
        return 1;                                                                                   \
    }
MCE_WEATHER_GETTER_CL(cloud_strength, strength)
MCE_WEATHER_GETTER_CL(cloud_fair_coverage, fair_coverage)
MCE_WEATHER_GETTER_CL(cloud_overcast_coverage, overcast_coverage)
MCE_WEATHER_GETTER_CL(cloud_scale, scale_m)
MCE_WEATHER_GETTER_CL(cloud_speed_scale, speed_scale)
MCE_WEATHER_GETTER_CL(cloud_height, height_m)
#undef MCE_WEATHER_GETTER_CL

// weather.set_clouds{strength=, fair_coverage=, overcast_coverage=, scale=, speed_scale=, height=}: validated before
// anything is written.
int l_weather_set_clouds(lua_State* L) {
    static const char* const kKeys[6] = {"strength", "fair_coverage", "overcast_coverage", "scale", "speed_scale", "height"};
    f64 value[6] = {0, 0, 0, 0, 0, 0};
    bool given[6] = {false, false, false, false, false, false};
    read_settings(L, "weather.set_clouds", kKeys, 6, "strength, fair_coverage, overcast_coverage, scale, speed_scale, height",
                  value, given);
    kit::CloudLayer& c = check_clouds(L, "weather.set_clouds");
    if (given[0]) c.strength = static_cast<f32>(clamp_range(value[0], 0.0, 1.0));
    if (given[1]) c.fair_coverage = static_cast<f32>(clamp_range(value[1], 0.0, 1.0));
    if (given[2]) c.overcast_coverage = static_cast<f32>(clamp_range(value[2], 0.0, 1.0));
    if (given[3]) c.scale_m = static_cast<f32>(clamp_range(value[3], 50.0, 20000.0));
    if (given[4]) c.speed_scale = static_cast<f32>(clamp_range(value[4], 0.0, 10.0));
    if (given[5]) c.height_m = static_cast<f32>(clamp_range(value[5], 100.0, 20000.0));
    return 0;
}

#define MCE_WEATHER_GETTER_WA(luaname, field)                                                       \
    int l_weather_##luaname(lua_State* L) {                                                         \
        lua_pushnumber(L, static_cast<double>(check_audio(L, "weather." #luaname).field));         \
        return 1;                                                                                   \
    }
MCE_WEATHER_GETTER_WA(audio_rain_db, rain_db)
MCE_WEATHER_GETTER_WA(audio_wind_db, wind_db)
MCE_WEATHER_GETTER_WA(audio_fade, fade_seconds)
MCE_WEATHER_GETTER_WA(audio_enabled, enabled)
#undef MCE_WEATHER_GETTER_WA

// The levels are saved as integers out of 2^24 (the Weather.wet_q convention).
int l_weather_rain_level(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_audio(L, "weather.rain_level").rain_q) / 16777216.0);
    return 1;
}

int l_weather_wind_level(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_audio(L, "weather.wind_level").wind_q) / 16777216.0);
    return 1;
}

// weather.set_audio{rain_db=, wind_db=, fade=, enabled=}: validated before anything is written. (The sources and the
// levels are not settable.)
int l_weather_set_audio(lua_State* L) {
    static const char* const kKeys[4] = {"rain_db", "wind_db", "fade", "enabled"};
    f64 value[4] = {0, 0, 0, 0};
    bool given[4] = {false, false, false, false};
    read_settings(L, "weather.set_audio", kKeys, 4, "rain_db, wind_db, fade, enabled", value, given);
    kit::WeatherAudio& a = check_audio(L, "weather.set_audio");
    if (given[0]) a.rain_db = static_cast<f32>(clamp_range(value[0], -60.0, 12.0));
    if (given[1]) a.wind_db = static_cast<f32>(clamp_range(value[1], -60.0, 12.0));
    if (given[2]) a.fade_seconds = static_cast<f32>(clamp_range(value[2], 0.0, 60.0));
    if (given[3]) a.enabled = value[3] != 0.0 ? 1 : 0;
    return 0;
}

#define MCE_WEATHER_GETTER_LB(luaname, field)                                                      \
    int l_weather_##luaname(lua_State* L) {                                                         \
        lua_pushnumber(L, static_cast<double>(check_bolt(L, "weather." #luaname).field));           \
        return 1;                                                                                   \
    }
MCE_WEATHER_GETTER_LB(bolt_min_range, min_range_m)
MCE_WEATHER_GETTER_LB(bolt_max_range, max_range_m)
MCE_WEATHER_GETTER_LB(bolt_ground_y, ground_y)
MCE_WEATHER_GETTER_LB(bolt_height, height_m)
MCE_WEATHER_GETTER_LB(bolt_width, width_m)
MCE_WEATHER_GETTER_LB(bolt_segments, segments_log2)
MCE_WEATHER_GETTER_LB(bolt_jitter, jitter)
MCE_WEATHER_GETTER_LB(bolt_fork, fork)
MCE_WEATHER_GETTER_LB(bolt_enabled, enabled)
MCE_WEATHER_GETTER_LB(bolt_light, light_intensity)
MCE_WEATHER_GETTER_LB(bolt_light_radius, light_radius_m)
MCE_WEATHER_GETTER_LB(bolt_distance, strike_dist_m)
MCE_WEATHER_GETTER_LB(bolt_active, active)
#undef MCE_WEATHER_GETTER_LB

int l_weather_bolt_pos(lua_State* L) {
    const kit::LightningBolt& b = check_bolt(L, "weather.bolt_pos");
    lua_pushnumber(L, static_cast<double>(b.strike_x));
    lua_pushnumber(L, static_cast<double>(b.strike_z));
    return 2;
}

// weather.set_bolt{...}: validated before anything is written. (The last strike is engine-written, not settable.)
int l_weather_set_bolt(lua_State* L) {
    static const char* const kKeys[11] = {"min_range", "max_range", "ground_y", "height", "width", "segments",
                                          "jitter", "fork", "light", "light_radius", "enabled"};
    f64 value[11] = {};
    bool given[11] = {};
    read_settings(L, "weather.set_bolt", kKeys, 11,
                  "min_range, max_range, ground_y, height, width, segments, jitter, fork, light, light_radius, enabled",
                  value, given);
    kit::LightningBolt& b = check_bolt(L, "weather.set_bolt");
    if (given[0]) b.min_range_m = static_cast<f32>(clamp_range(value[0], 10.0, 5000.0));
    if (given[1]) b.max_range_m = static_cast<f32>(clamp_range(value[1], 10.0, 5000.0));
    if (given[2]) b.ground_y = static_cast<f32>(clamp_range(value[2], -1.0e6, 1.0e6));
    if (given[3]) b.height_m = static_cast<f32>(clamp_range(value[3], 50.0, 3000.0));
    if (given[4]) b.width_m = static_cast<f32>(clamp_range(value[4], 0.05, 20.0));
    if (given[5]) b.segments_log2 = static_cast<u32>(std::floor(clamp_range(value[5], 2.0, 7.0)));
    if (given[6]) b.jitter = static_cast<f32>(clamp_range(value[6], 0.0, 0.6));
    if (given[7]) b.fork = value[7] != 0.0 ? 1 : 0;
    if (given[8]) b.light_intensity = static_cast<f32>(clamp_range(value[8], 0.0, 100000.0));
    if (given[9]) b.light_radius_m = static_cast<f32>(clamp_range(value[9], 1.0, 5000.0));
    if (given[10]) b.enabled = value[10] != 0.0 ? 1 : 0;
    return 0;
}

#define MCE_WEATHER_GETTER_WG(luaname, field)                                                      \
    int l_weather_##luaname(lua_State* L) {                                                         \
        lua_pushnumber(L, static_cast<double>(check_gust(L, "weather." #luaname).field));            \
        return 1;                                                                                   \
    }
MCE_WEATHER_GETTER_WG(gust_strength, strength)
MCE_WEATHER_GETTER_WG(gust_min_gap, min_gap_s)
MCE_WEATHER_GETTER_WG(gust_max_gap, max_gap_s)
MCE_WEATHER_GETTER_WG(gust_min_rise, min_rise_s)
MCE_WEATHER_GETTER_WG(gust_max_rise, max_rise_s)
MCE_WEATHER_GETTER_WG(gust_min_fall, min_fall_s)
MCE_WEATHER_GETTER_WG(gust_max_fall, max_fall_s)
MCE_WEATHER_GETTER_WG(gust_min_peak, min_peak)
MCE_WEATHER_GETTER_WG(gust_max_peak, max_peak)
MCE_WEATHER_GETTER_WG(gust_seed, seed)
MCE_WEATHER_GETTER_WG(gust_affects, affects)
MCE_WEATHER_GETTER_WG(gust_enabled, enabled)
#undef MCE_WEATHER_GETTER_WG

int l_weather_gust_level(lua_State* L) {
    const kit::WindGust& g = check_gust(L, "weather.gust_level");
    lua_pushnumber(L, static_cast<double>(g.gust_q) / 16777216.0);
    return 1;
}

int l_weather_effective_wind(lua_State* L) {
    const kit::Weather& wx = check_weather(L, "weather.effective_wind");
    const kit::WindGust& g = check_gust(L, "weather.effective_wind");
    lua_pushnumber(L, static_cast<double>(kit::effective_wind(wx, &g, kit::kGustAll)));
    return 1;
}

// weather.set_gust{...}: validated before anything is written. (The gust state is engine-written, not settable.)
int l_weather_set_gust(lua_State* L) {
    static const char* const kKeys[12] = {"strength", "min_gap", "max_gap", "min_rise", "max_rise", "min_fall",
                                          "max_fall", "min_peak", "max_peak", "seed", "affects", "enabled"};
    f64 value[12] = {};
    bool given[12] = {};
    read_settings(L, "weather.set_gust", kKeys, 12,
                  "strength, min_gap, max_gap, min_rise, max_rise, min_fall, max_fall, min_peak, max_peak, seed, "
                  "affects, enabled",
                  value, given);
    kit::WindGust& g = check_gust(L, "weather.set_gust");
    if (given[0]) g.strength = static_cast<f32>(clamp_range(value[0], 0.0, 3.0));
    if (given[1]) g.min_gap_s = static_cast<f32>(clamp_range(value[1], 0.0, 120.0));
    if (given[2]) g.max_gap_s = static_cast<f32>(clamp_range(value[2], 0.0, 120.0));
    if (given[3]) g.min_rise_s = static_cast<f32>(clamp_range(value[3], 0.05, 30.0));
    if (given[4]) g.max_rise_s = static_cast<f32>(clamp_range(value[4], 0.05, 30.0));
    if (given[5]) g.min_fall_s = static_cast<f32>(clamp_range(value[5], 0.05, 60.0));
    if (given[6]) g.max_fall_s = static_cast<f32>(clamp_range(value[6], 0.05, 60.0));
    if (given[7]) g.min_peak = static_cast<f32>(clamp_range(value[7], 0.0, 1.0));
    if (given[8]) g.max_peak = static_cast<f32>(clamp_range(value[8], 0.0, 1.0));
    if (given[9]) g.seed = static_cast<u32>(std::floor(clamp_range(value[9], 0.0, 4294967295.0)));
    if (given[10]) g.affects = static_cast<u32>(std::floor(clamp_range(value[10], 0.0, 4294967295.0)));
    if (given[11]) g.enabled = value[11] != 0.0 ? 1 : 0;
    return 0;
}

#define MCE_WEATHER_GETTER_GM(luaname, field)                                                      \
    int l_weather_##luaname(lua_State* L) {                                                         \
        lua_pushnumber(L, static_cast<double>(check_mist(L, "weather." #luaname).field));            \
        return 1;                                                                                   \
    }
MCE_WEATHER_GETTER_GM(mist_strength, strength)
MCE_WEATHER_GETTER_GM(mist_peak_hour, peak_hour)
MCE_WEATHER_GETTER_GM(mist_rise, rise_hours)
MCE_WEATHER_GETTER_GM(mist_burn, burn_hours)
MCE_WEATHER_GETTER_GM(mist_base_humidity, base_humidity)
MCE_WEATHER_GETTER_GM(mist_wet_gain, wet_gain)
MCE_WEATHER_GETTER_GM(mist_weather_damp, weather_damp)
MCE_WEATHER_GETTER_GM(mist_density, density)
MCE_WEATHER_GETTER_GM(mist_thickness, thickness_m)
MCE_WEATHER_GETTER_GM(mist_base_height, base_height_m)
MCE_WEATHER_GETTER_GM(mist_exp2_density, exp2_density)
MCE_WEATHER_GETTER_GM(mist_enabled, enabled)
#undef MCE_WEATHER_GETTER_GM

int l_weather_mist_color(lua_State* L) {
    const kit::GroundMist& m = check_mist(L, "weather.mist_color");
    lua_pushnumber(L, static_cast<double>(m.color.x));
    lua_pushnumber(L, static_cast<double>(m.color.y));
    lua_pushnumber(L, static_cast<double>(m.color.z));
    return 3;
}

// The level the extract would apply now: the Environment3D resource (else the first component, else the default,
// which has no clock) gives the hour; the first Weather, if any, the wetness and the damping.
int l_weather_mist_level(lua_State* L) {
    const kit::GroundMist& m = check_mist(L, "weather.mist_level");
    World& w = world_of(L);
    f32 hour = kit::Environment3D{}.time_of_day;
    if (const kit::Environment3D* r = w.try_resource<kit::Environment3D>()) {
        hour = r->time_of_day;
    } else {
        bool found = false;
        w.query<const kit::Environment3D&>().each([&](const kit::Environment3D& c) {
            if (found) return;
            found = true;
            hour = c.time_of_day;
        });
    }
    const kit::Weather* wx = nullptr;
    const refl::TypeRecord* type = refl::find_type(StringId("Weather"));
    if (type && w.find_component(type->type_hash) != ecs::kInvalidComponent) {
        w.query<kit::Weather>().each([&](Entity, kit::Weather& c) {
            if (!wx) wx = &c;
        });
    }
    lua_pushnumber(L, static_cast<double>(kit::mist_level(m, hour, wx)));
    return 1;
}

// weather.set_mist{...}: validated before anything is written.
int l_weather_set_mist(lua_State* L) {
    static const char* const kKeys[13] = {"strength", "peak_hour", "rise_hours", "burn_hours", "base_humidity",
                                          "wet_gain", "weather_damp", "density", "thickness", "base_height",
                                          "exp2_density", "color", "enabled"};
    f64 value[13] = {};
    bool given[13] = {};
    f64 color[3] = {};
    read_settings(L, "weather.set_mist", kKeys, 13,
                  "strength, peak_hour, rise_hours, burn_hours, base_humidity, wet_gain, weather_damp, density, "
                  "thickness, base_height, exp2_density, color, enabled",
                  value, given, 11, color);
    kit::GroundMist& m = check_mist(L, "weather.set_mist");
    if (given[0]) m.strength = static_cast<f32>(clamp_range(value[0], 0.0, 2.0));
    if (given[1]) m.peak_hour = static_cast<f32>(clamp_range(value[1], 0.0, 24.0));
    if (given[2]) m.rise_hours = static_cast<f32>(clamp_range(value[2], 0.1, 12.0));
    if (given[3]) m.burn_hours = static_cast<f32>(clamp_range(value[3], 0.1, 12.0));
    if (given[4]) m.base_humidity = static_cast<f32>(clamp_range(value[4], 0.0, 1.0));
    if (given[5]) m.wet_gain = static_cast<f32>(clamp_range(value[5], 0.0, 1.0));
    if (given[6]) m.weather_damp = static_cast<f32>(clamp_range(value[6], 0.0, 1.0));
    if (given[7]) m.density = static_cast<f32>(clamp_range(value[7], 0.0, 1.0));
    if (given[8]) m.thickness_m = static_cast<f32>(clamp_range(value[8], 0.5, 200.0));
    if (given[9]) m.base_height_m = static_cast<f32>(clamp_range(value[9], -1000.0, 1000.0));
    if (given[10]) m.exp2_density = static_cast<f32>(clamp_range(value[10], 0.0, 0.2));
    if (given[11]) {
        m.color.x = static_cast<f32>(clamp_range(color[0], 0.0, 1.0));
        m.color.y = static_cast<f32>(clamp_range(color[1], 0.0, 1.0));
        m.color.z = static_cast<f32>(clamp_range(color[2], 0.0, 1.0));
    }
    if (given[12]) m.enabled = value[12] != 0.0 ? 1 : 0;
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
    {"snow_cover", l_weather_snow_cover},
    {"snow_strength", l_weather_snow_strength},
    {"snow_build", l_weather_snow_build},
    {"snow_melt", l_weather_snow_melt},
    {"snow_coverage", l_weather_snow_coverage},
    {"snow_scale", l_weather_snow_scale},
    {"snow_wind", l_weather_snow_wind},
    {"set_snow", l_weather_set_snow},
    {"ripples_strength", l_weather_ripples_strength},
    {"ripples_density", l_weather_ripples_density},
    {"ripples_speed", l_weather_ripples_speed},
    {"ripples_ring_width", l_weather_ripples_ring_width},
    {"set_ripples", l_weather_set_ripples},
    {"splash_rate", l_weather_splash_rate},
    {"splash_height", l_weather_splash_height},
    {"splash_life", l_weather_splash_life},
    {"splash_radius", l_weather_splash_radius},
    {"splash_seed", l_weather_splash_seed},
    {"set_splash", l_weather_set_splash},
    {"thunder_speed", l_weather_thunder_speed},
    {"thunder_volume", l_weather_thunder_volume},
    {"set_thunder", l_weather_set_thunder},
    {"wind_degrees", l_weather_wind_degrees},
    {"wind_drift", l_weather_wind_drift},
    {"wind_heading", l_weather_wind_heading},
    {"set_wind_direction", l_weather_set_wind_direction},
    {"cloud_strength", l_weather_cloud_strength},
    {"cloud_fair_coverage", l_weather_cloud_fair_coverage},
    {"cloud_overcast_coverage", l_weather_cloud_overcast_coverage},
    {"cloud_scale", l_weather_cloud_scale},
    {"cloud_speed_scale", l_weather_cloud_speed_scale},
    {"cloud_height", l_weather_cloud_height},
    {"set_clouds", l_weather_set_clouds},
    {"rain_level", l_weather_rain_level},
    {"wind_level", l_weather_wind_level},
    {"audio_rain_db", l_weather_audio_rain_db},
    {"audio_wind_db", l_weather_audio_wind_db},
    {"audio_fade", l_weather_audio_fade},
    {"audio_enabled", l_weather_audio_enabled},
    {"set_audio", l_weather_set_audio},
    {"bolt_min_range", l_weather_bolt_min_range},
    {"bolt_max_range", l_weather_bolt_max_range},
    {"bolt_ground_y", l_weather_bolt_ground_y},
    {"bolt_height", l_weather_bolt_height},
    {"bolt_width", l_weather_bolt_width},
    {"bolt_segments", l_weather_bolt_segments},
    {"bolt_jitter", l_weather_bolt_jitter},
    {"bolt_fork", l_weather_bolt_fork},
    {"bolt_enabled", l_weather_bolt_enabled},
    {"bolt_light", l_weather_bolt_light},
    {"bolt_light_radius", l_weather_bolt_light_radius},
    {"bolt_distance", l_weather_bolt_distance},
    {"bolt_pos", l_weather_bolt_pos},
    {"bolt_active", l_weather_bolt_active},
    {"set_bolt", l_weather_set_bolt},
    {"gust_strength", l_weather_gust_strength},
    {"gust_min_gap", l_weather_gust_min_gap},
    {"gust_max_gap", l_weather_gust_max_gap},
    {"gust_min_rise", l_weather_gust_min_rise},
    {"gust_max_rise", l_weather_gust_max_rise},
    {"gust_min_fall", l_weather_gust_min_fall},
    {"gust_max_fall", l_weather_gust_max_fall},
    {"gust_min_peak", l_weather_gust_min_peak},
    {"gust_max_peak", l_weather_gust_max_peak},
    {"gust_seed", l_weather_gust_seed},
    {"gust_affects", l_weather_gust_affects},
    {"gust_enabled", l_weather_gust_enabled},
    {"gust_level", l_weather_gust_level},
    {"effective_wind", l_weather_effective_wind},
    {"set_gust", l_weather_set_gust},
    {"mist_strength", l_weather_mist_strength},
    {"mist_peak_hour", l_weather_mist_peak_hour},
    {"mist_rise", l_weather_mist_rise},
    {"mist_burn", l_weather_mist_burn},
    {"mist_base_humidity", l_weather_mist_base_humidity},
    {"mist_wet_gain", l_weather_mist_wet_gain},
    {"mist_weather_damp", l_weather_mist_weather_damp},
    {"mist_density", l_weather_mist_density},
    {"mist_thickness", l_weather_mist_thickness},
    {"mist_base_height", l_weather_mist_base_height},
    {"mist_exp2_density", l_weather_mist_exp2_density},
    {"mist_color", l_weather_mist_color},
    {"mist_enabled", l_weather_mist_enabled},
    {"mist_level", l_weather_mist_level},
    {"set_mist", l_weather_set_mist},
    {nullptr, nullptr},
};

}  // namespace

void register_weather_api(lua_State* L) {
    lua_newtable(L);
    luaL_register(L, nullptr, kWeatherLib);
    lua_setglobal(L, "weather");
}

}  // namespace mce::script::detail
