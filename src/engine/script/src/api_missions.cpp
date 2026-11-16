// mce_script (M-MISSION step 3a): the `mission` and `marker` globals, thin bindings over the kit's mission
// and map-marker components (mce/kit/missions.h, map_markers.h). Like the `mce` functions they hold no state:
// everything lives in the Mission / MapMarker components and the MissionSignal event, so saves, replays and
// the editor see it. A world without the kit pieces gets a named error, never a crash.
//
//   mission.start(e) -> boolean        the idle mission starts on the next tick (Mission.start = 1)
//   mission.fail(e) -> boolean         an active mission fails now (MissionFailed, reason 3: a script)
//   mission.status(e) -> state, objective, elapsed, done
//   mission.signal(name, amount?)      a MissionSignal for the active `signal` objectives
//   marker.add(e, {icon, color, layer, label, always_visible, follow})
//   marker.remove(e)
//
// Every function may raise (LUA_USE_LONGJMP): nothing with a destructor lives across a luaL_* call.

#include <cmath>
#include <cstring>

#include "api.h"
#include "mce/fixed_string.h"
#include "mce/kit/map_markers.h"
#include "mce/kit/missions.h"

namespace mce::script::detail {

namespace {

constexpr f64 kMaxAmount = 100000.0;

kit::Mission& check_mission(lua_State* L, World& w, Entity e, const char* fn) {
    if (!w.alive(e)) luaL_error(L, "%s: the entity is not alive", fn);
    kit::Mission* m = w.try_get_mut<kit::Mission>(e);
    if (!m) luaL_error(L, "%s: the entity has no Mission component", fn);
    return *m;
}

const char* state_name(kit::MissionState s) {
    switch (s) {
        case kit::MissionState::Idle: return "idle";
        case kit::MissionState::Active: return "active";
        case kit::MissionState::Complete: return "complete";
        case kit::MissionState::Failed: return "failed";
    }
    return "idle";
}

// mission.start(e) -> true when the mission was idle and will start on the next tick, false when it already
// ran (active, complete or failed missions are left alone).
int l_mission_start(lua_State* L) {
    World& w = world_of(L);
    kit::Mission& m = check_mission(L, w, check_entity(L, 1), "mission.start");
    if (m.state != kit::MissionState::Idle) {
        lua_pushboolean(L, 0);
        return 1;
    }
    m.start = 1;
    lua_pushboolean(L, 1);
    return 1;
}

// mission.fail(e) -> true when an active mission failed now, false for any other state.
int l_mission_fail(lua_State* L) {
    Frame& f = frame_of(L);
    World& w = f.world();
    const Entity e = check_entity(L, 1);
    kit::Mission& m = check_mission(L, w, e, "mission.fail");
    if (m.state != kit::MissionState::Active) {
        lua_pushboolean(L, 0);
        return 1;
    }
    if (!w.event_queue<kit::MissionFailed>()) luaL_error(L, "mission.fail: this world has no missions (install_missions)");
    m.state = kit::MissionState::Failed;
    f.events().push(kit::MissionFailed{e, 3u});
    lua_pushboolean(L, 1);
    return 1;
}

// mission.status(e) -> state ("idle", "active", "complete", "failed"), the active objective (nil unless active),
// seconds since it started, objectives done.
int l_mission_status(lua_State* L) {
    World& w = world_of(L);
    const Entity e = check_entity(L, 1);
    if (!w.alive(e)) luaL_error(L, "mission.status: the entity is not alive");
    const kit::Mission* m = w.try_get<kit::Mission>(e);
    if (!m) luaL_error(L, "mission.status: the entity has no Mission component");
    lua_pushstring(L, state_name(m->state));
    if (m->state == kit::MissionState::Active && !m->current.is_null() && w.alive(m->current)) push_entity(L, m->current);
    else lua_pushnil(L);
    lua_pushnumber(L, static_cast<double>(m->elapsed));
    lua_pushnumber(L, static_cast<double>(m->done));
    return 4;
}

// mission.signal(name, amount?): the MissionSignal that `signal` objectives count (default amount 1).
int l_mission_signal(lua_State* L) {
    Frame& f = frame_of(L);
    size_t len = 0;
    const char* name = luaL_checklstring(L, 1, &len);
    const f64 amount = luaL_optnumber(L, 2, 1.0);
    if (len == 0) luaL_error(L, "mission.signal: the name is empty");
    if (len > decltype(kit::MissionSignal::name)::capacity())
        luaL_error(L, "mission.signal: the name is longer than %d characters", static_cast<int>(decltype(kit::MissionSignal::name)::capacity()));
    if (!(amount >= 1.0 && amount <= kMaxAmount && amount == std::floor(amount)))
        luaL_error(L, "mission.signal: the amount %g is not a whole number from 1 to 100000", amount);
    if (!f.world().event_queue<kit::MissionSignal>()) luaL_error(L, "mission.signal: this world has no missions (install_missions)");
    kit::MissionSignal s;
    s.name.assign(StringView(name, len));
    s.amount = static_cast<u32>(amount);
    f.events().push(s);
    return 0;
}

// ---- markers -----------------------------------------------------------------------------------

// A whole number in [lo, hi] at field `key` of the table at `t`; false when the key is absent.
bool whole_field(lua_State* L, int t, const char* key, f64 lo, f64 hi, f64& out) {
    lua_getfield(L, t, key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "marker.add: '%s' must be a number", key);
    out = lua_tonumber(L, -1);
    if (!(out >= lo && out <= hi && out == std::floor(out)))
        luaL_error(L, "marker.add: '%s' %g is not a whole number from %g to %g", key, out, lo, hi);
    lua_pop(L, 1);
    return true;
}

// The color at field `key`: {r, g, b} or {r, g, b, a} (numbers, alpha 1 when left out), or a vector (r, g, b).
bool color_field(lua_State* L, int t, const char* key, math::Color& out) {
    lua_getfield(L, t, key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    f32 c[4] = {1, 1, 1, 1};
    if (lua_isvector(L, -1)) {
        const float* v = lua_tovector(L, -1);
        c[0] = v[0];
        c[1] = v[1];
        c[2] = v[2];
    } else if (lua_istable(L, -1)) {
        const int n = static_cast<int>(lua_objlen(L, -1));
        if (n != 3 && n != 4) luaL_error(L, "marker.add: '%s' takes {r, g, b} or {r, g, b, a}", key);
        for (int i = 0; i < n; ++i) {
            lua_rawgeti(L, -1, i + 1);
            if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "marker.add: '%s' takes numbers", key);
            c[i] = static_cast<f32>(lua_tonumber(L, -1));
            lua_pop(L, 1);
        }
    } else {
        luaL_error(L, "marker.add: '%s' takes {r, g, b, a} or a vector", key);
    }
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(c[i])) luaL_error(L, "marker.add: '%s' has a value that is not a finite number", key);
    out = math::Color{c[0], c[1], c[2], c[3]};
    lua_pop(L, 1);
    return true;
}

bool known_marker_key(const char* k) {
    static const char* const keys[] = {"icon", "color", "colour", "layer", "label", "always_visible", "follow"};
    for (const char* key : keys)
        if (std::strcmp(k, key) == 0) return true;
    return false;
}

// marker.add(e, {icon, color, layer, label, always_visible, follow}): puts a MapMarker on the entity at the
// next sync point. Fields left out keep the entity's current marker, or the defaults. `colour` is `color`.
int l_marker_add(lua_State* L) {
    World& w = world_of(L);
    const Entity e = check_entity(L, 1);
    if (!w.alive(e)) luaL_error(L, "marker.add: the entity is not alive");
    const refl::TypeRecord* marker_type = refl::find_type(StringId("MapMarker"));
    if (!marker_type || w.find_component(marker_type->type_hash) == ecs::kInvalidComponent)
        luaL_error(L, "marker.add: this world has no map markers (install_map_markers)");
    const bool has_table = !lua_isnoneornil(L, 2);
    if (has_table && !lua_istable(L, 2)) luaL_error(L, "marker.add: the second argument is a table of fields");
    kit::MapMarker marker;
    if (const kit::MapMarker* cur = w.try_get<kit::MapMarker>(e)) marker = *cur;
    if (has_table) {
        lua_pushnil(L);
        while (lua_next(L, 2) != 0) {
            lua_pop(L, 1);
            if (lua_type(L, -1) != LUA_TSTRING) luaL_error(L, "marker.add: the table's keys are field names");
            if (!known_marker_key(lua_tostring(L, -1)))
                luaL_error(L, "marker.add: unknown field '%s' (icon, color, layer, label, always_visible, follow)", lua_tostring(L, -1));
        }
        f64 n = 0;
        if (whole_field(L, 2, "icon", 0, 4294967295.0, n)) marker.icon = static_cast<u32>(n);
        if (whole_field(L, 2, "layer", 0, 31, n)) marker.layer = static_cast<u32>(n);
        color_field(L, 2, "color", marker.color);
        color_field(L, 2, "colour", marker.color);
        lua_getfield(L, 2, "label");
        if (!lua_isnil(L, -1)) {
            if (lua_type(L, -1) != LUA_TSTRING) luaL_error(L, "marker.add: 'label' must be a string");
            size_t len = 0;
            const char* s = lua_tolstring(L, -1, &len);
            if (len > decltype(marker.label)::capacity())
                luaL_error(L, "marker.add: 'label' is longer than %d characters", static_cast<int>(decltype(marker.label)::capacity()));
            marker.label.assign(StringView(s, len));
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "always_visible");
        if (!lua_isnil(L, -1)) {
            if (lua_type(L, -1) != LUA_TBOOLEAN) luaL_error(L, "marker.add: 'always_visible' must be a boolean");
            marker.always_visible = lua_toboolean(L, -1) ? 1 : 0;
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "follow");
        if (!lua_isnil(L, -1)) {
            const Entity target = check_entity(L, lua_gettop(L));
            if (!w.alive(target) || (!w.try_get<kit::Objective>(target) && !w.try_get<kit::Mission>(target)))
                luaL_error(L, "marker.add: 'follow' must be an entity with an Objective or a Mission");
            marker.follow_objective = target;
        }
        lua_pop(L, 1);
    }
    w.commands().set(e, marker);
    return 0;
}

// marker.remove(e): takes the MapMarker off at the next sync point (nothing happens without one).
int l_marker_remove(lua_State* L) {
    World& w = world_of(L);
    const Entity e = check_entity(L, 1);
    if (!w.alive(e)) luaL_error(L, "marker.remove: the entity is not alive");
    if (w.try_get<kit::MapMarker>(e)) w.commands().remove<kit::MapMarker>(e);
    return 0;
}

const luaL_Reg kMissionLib[] = {
    {"start", l_mission_start},
    {"fail", l_mission_fail},
    {"status", l_mission_status},
    {"signal", l_mission_signal},
    {nullptr, nullptr},
};

const luaL_Reg kMarkerLib[] = {
    {"add", l_marker_add},
    {"remove", l_marker_remove},
    {nullptr, nullptr},
};

}  // namespace

void register_mission_api(lua_State* L) {
    lua_newtable(L);
    luaL_register(L, nullptr, kMissionLib);
    lua_setglobal(L, "mission");
    lua_newtable(L);
    luaL_register(L, nullptr, kMarkerLib);
    lua_setglobal(L, "marker");
}

}  // namespace mce::script::detail
