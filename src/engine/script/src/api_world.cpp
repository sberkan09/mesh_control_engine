// mce_script (M-SCRIPT step 2): the `mce` functions that reach past one component's fields:
// entities (Commands), events, the kit's signals, input, scene changes and the physics queries.
// script.cpp has the field access and the host; api.h the helpers both share.
//
// Every function may raise (LUA_USE_LONGJMP): engine calls whose results own memory run in a
// scope that ends before the next luaL_* call, and lists go through the host's scratch arrays.

#include <cstring>

#include "api.h"
#include "mce/asset/asset_system.h"
#include "mce/asset/guid.h"
#include "mce/audio/components.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/prefab.h"
#include "mce/input/actions.h"
#include "mce/kit/ai.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/sequence.h"
#include "mce/kit/timeline.h"
#include "mce/log.h"
#include "mce/physics/world2d.h"
#include "mce/scene/scene.h"

namespace mce::script::detail {

namespace {

constexpr u32 kMaxSpawnComponents = 32;

math::vec2 check_vec2(lua_State* L, int idx) {
    const float* v = luaL_checkvector(L, idx);
    return {v[0], v[1]};
}

math::vec3 check_vec3(lua_State* L, int idx) {
    const float* v = luaL_checkvector(L, idx);
    return {v[0], v[1], v[2]};
}

void push_vec2(lua_State* L, math::vec2 v) { lua_pushvector(L, v.x, v.y, 0.0f); }
void push_vec3(lua_State* L, math::vec3 v) { lua_pushvector(L, v.x, v.y, v.z); }

// The registered queue of an MCE_EVENT type, or null; raises when the record and the queue disagree.
ecs::detail::EventQueueBase* event_queue(lua_State* L, World& w, const refl::TypeRecord& type) {
    ecs::detail::EventQueueBase* q = w.event_queue_raw(type.type_hash);
    if (q && q->elem_size != type.size) luaL_error(L, "event '%s' has a different size than its record", type.name);
    return q;
}

// ---- entities --------------------------------------------------------------------------------

int l_has(lua_State* L) {
    World& w = world_of(L);
    const Entity e = check_entity(L, 1);
    const refl::TypeRecord& type = check_type(L, 2, refl::TypeFlags::Component);
    const ecs::ComponentId c = w.find_component(type.type_hash);
    lua_pushboolean(L, c != ecs::kInvalidComponent && w.alive(e) && w.has(e, c));
    return 1;
}

int l_alive(lua_State* L) {
    World& w = world_of(L);
    lua_pushboolean(L, w.alive(check_entity(L, 1)));
    return 1;
}

// mce.spawn({Comp = {...}, Tag = true}) -> entity. The components are recorded in ComponentId
// order (never the table's), so two runs record the same bytes.
int l_spawn(lua_State* L) {
    ScriptHost& host = HostAccess::host(L);
    World& w = world_of(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    const refl::TypeRecord* types[kMaxSpawnComponents];
    ecs::ComponentId ids[kMaxSpawnComponents];
    u32 n = 0;
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        lua_pop(L, 1);
        if (n == kMaxSpawnComponents) luaL_error(L, "mce.spawn takes at most %d components", int(kMaxSpawnComponents));
        lua_pushvalue(L, -1);
        const refl::TypeRecord& type = check_type(L, lua_gettop(L), refl::TypeFlags::Component);
        lua_pop(L, 1);
        const ecs::ComponentId c = component_id(w, type);
        u32 at = n++;
        while (at > 0 && ids[at - 1] > c) {
            ids[at] = ids[at - 1];
            types[at] = types[at - 1];
            --at;
        }
        ids[at] = c;
        types[at] = &type;
    }
    // Lay every value out first: scratch_default may grow (and move) the buffer.
    usize offsets[kMaxSpawnComponents];
    usize total = 0;
    for (u32 i = 0; i < n; ++i) {
        offsets[i] = total;
        total += align_up(usize(types[i]->size), 16);
    }
    Array<u8>& scratch = HostAccess::scratch(host);
    if (scratch.size() < total + 16) scratch.resize(total + 16);
    ecs::RawComponent raw[kMaxSpawnComponents];
    for (u32 i = 0; i < n; ++i) {
        u8* p = scratch_default(host, *types[i], offsets[i]);
        lua_getfield(L, 1, types[i]->name);
        if (lua_istable(L, -1)) write_struct(L, *types[i], p, lua_gettop(L));
        else if (!lua_isboolean(L, -1)) luaL_error(L, "mce.spawn: '%s' takes a table of fields or true", types[i]->name);
        lua_pop(L, 1);
        raw[i] = ecs::RawComponent{ids[i], p};
    }
    const Entity e = w.reserve_entity();
    w.commands().spawn_raw(e, Span<const ecs::RawComponent>(raw, n));
    push_entity(L, e);
    return 1;
}

// mce.add(e, "Comp", {...}): the component with the named fields set (the rest from the current
// value when the entity has it, else the defaults), at the next sync point.
int l_add(lua_State* L) {
    ScriptHost& host = HostAccess::host(L);
    World& w = world_of(L);
    const Entity e = check_entity(L, 1);
    const refl::TypeRecord& type = check_type(L, 2, refl::TypeFlags::Component);
    const ecs::ComponentId c = component_id(w, type);
    u8* p = scratch_default(host, type, 0);
    if (const void* row = w.alive(e) && w.has(e, c) ? w.get_raw(e, c) : nullptr) std::memcpy(p, row, type.size);
    if (lua_istable(L, 3)) write_struct(L, type, p, 3);
    else if (!lua_isnoneornil(L, 3)) luaL_error(L, "mce.add: the third argument is a table of fields");
    w.commands().set_raw(e, c, p);
    return 0;
}

int l_remove(lua_State* L) {
    World& w = world_of(L);
    const Entity e = check_entity(L, 1);
    const refl::TypeRecord& type = check_type(L, 2, refl::TypeFlags::Component);
    const ecs::ComponentId c = w.find_component(type.type_hash);
    if (c != ecs::kInvalidComponent) w.commands().remove_raw(e, c);
    return 0;
}

int l_destroy(lua_State* L) {
    World& w = world_of(L);
    w.commands().destroy(check_entity(L, 1));
    return 0;
}

// mce.instantiate("guid:<prefab>", position?) -> the root entity, or nil when it failed (logged).
int l_instantiate(lua_State* L) {
    ScriptHost& host = HostAccess::host(L);
    World& w = world_of(L);
    const char* text = luaL_checkstring(L, 1);
    const bool at_position = !lua_isnoneornil(L, 2);
    const math::vec3 position = at_position ? check_vec3(L, 2) : math::vec3{};
    asset::AssetSystem* assets = HostAccess::assets(host);
    if (!assets) luaL_error(L, "mce.instantiate: this host has no assets");
    const StringView sv(text);
    asset::Guid guid;
    {
        Result<asset::Guid> parsed = asset::parse_guid(sv.substr(0, 5) == StringView("guid:") ? sv.substr(5) : sv);
        if (parsed) guid = parsed.value();
    }
    if (guid.is_null()) luaL_error(L, "mce.instantiate: '%s' is not a GUID", text);
    Array<Entity>& out = HostAccess::entities(host);
    out.clear();
    bool ok = false;
    {
        const asset::AssetKey key = assets->find(guid);
        if (key == asset::kInvalidAssetKey) {
            MCE_LOG_ERROR("script: instantiate {}: not in the mounted assets", text);
        } else if (Result<asset::AssetHandle<asset::Prefab>> h = assets->load_sync<asset::Prefab>(key); !h) {
            FixedString<512> msg;
            format_error(h.error(), msg);
            MCE_LOG_ERROR("script: instantiate {}: {}", text, msg.c_str());
        } else {
            const ecs::PrefabOverride o = ecs::PrefabOverride::of("LocalTransform.position", position);
            ecs::SpawnParams p;
            if (at_position) p.overrides = Span<const ecs::PrefabOverride>(&o, 1);
            const Result<u32> r = ecs::instantiate(w, assets->resolve(h.value()), p, &out);
            assets->release(asset::AnyHandle(h.value()));
            ok = r && !out.empty();
            if (!r) {
                FixedString<512> msg;
                format_error(r.error(), msg);
                MCE_LOG_ERROR("script: instantiate {}: {}", text, msg.c_str());
            }
        }
    }
    if (ok) push_entity(L, out[0]);
    else lua_pushnil(L);
    return 1;
}

// ---- events and signals ------------------------------------------------------------------------

// mce.events("TriggerEnter") -> an array of tables (this tick's readable events; empty when the
// World has no such queue).
int l_events(lua_State* L) {
    World& w = world_of(L);
    const refl::TypeRecord& type = check_type(L, 1, refl::TypeFlags::Event);
    ecs::detail::EventQueueBase* q = event_queue(L, w, type);
    const usize n = q ? q->front_size() : 0;
    lua_createtable(L, static_cast<int>(n), 0);
    if (!q) return 1;
    w.note_event_access(*q, false);
    const auto* data = static_cast<const u8*>(q->front_data());
    for (usize i = 0; i < n; ++i) {
        push_struct(L, type, data + i * q->elem_size);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mce.emit("Type", {...}): pushes one event, readable from the next tick.
int l_emit(lua_State* L) {
    ScriptHost& host = HostAccess::host(L);
    World& w = world_of(L);
    const refl::TypeRecord& type = check_type(L, 1, refl::TypeFlags::Event);
    ecs::detail::EventQueueBase* q = event_queue(L, w, type);
    if (!q) luaL_error(L, "event '%s' is not registered in this world", type.name);
    u8* p = scratch_default(host, type, 0);
    if (lua_istable(L, 2)) write_struct(L, type, p, 2);
    else if (!lua_isnoneornil(L, 2)) luaL_error(L, "mce.emit: the second argument is a table of fields");
    w.note_event_access(*q, true);
    q->push_raw(p);
    return 0;
}

// mce.signal(name, e?): a named signal that sequences' and behavior trees' wait nodes see.
int l_signal(lua_State* L) {
    Frame& f = frame_of(L);
    World& w = f.world();
    const StringId id(luaL_checkstring(L, 1));
    const Entity e = opt_entity(L, 2);
    if (w.event_queue<kit::SeqSignal>()) f.events().push(kit::SeqSignal{id, e});
    if (w.event_queue<kit::BtSignal>()) f.events().push(kit::BtSignal{id, e});
    return 0;
}

template <class S>
void collect_signals(Frame& f, StringId id, Array<Entity>& out) {
    if (!f.world().event_queue<S>()) return;
    for (const S& s : f.events().read<S>())
        if (s.id == id) out.push_back(s.entity);
}

// mce.signals(name) -> the emitters (entities, or false for signals with none) of every readable
// signal of that name: sequences', timelines' and behavior trees', in that order.
int l_signals(lua_State* L) {
    ScriptHost& host = HostAccess::host(L);
    Frame& f = frame_of(L);
    const StringId id(luaL_checkstring(L, 1));
    Array<Entity>& out = HostAccess::entities(host);
    out.clear();
    collect_signals<kit::SeqSignal>(f, id, out);
    collect_signals<kit::TimelineSignal>(f, id, out);
    collect_signals<kit::BtSignal>(f, id, out);
    lua_createtable(L, static_cast<int>(out.size()), 0);
    for (usize i = 0; i < out.size(); ++i) {
        if (out[i].is_null()) lua_pushboolean(L, 0);
        else push_entity(L, out[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mce.play_sound("guid:<sfx>", gain_db?)
int l_play_sound(lua_State* L) {
    Frame& f = frame_of(L);
    const char* text = luaL_checkstring(L, 1);
    const f32 gain = static_cast<f32>(luaL_optnumber(L, 2, 0.0));
    const StringView sv(text);
    audio::PlaySfx sfx;
    {
        Result<asset::Guid> parsed = asset::parse_guid(sv.substr(0, 5) == StringView("guid:") ? sv.substr(5) : sv);
        if (parsed) sfx.clip = parsed.value();
    }
    if (sfx.clip.is_null()) luaL_error(L, "mce.play_sound: '%s' is not a GUID", text);
    if (!f.world().event_queue<audio::PlaySfx>()) return 0;  // no audio in this world
    sfx.gain_db = gain;
    f.events().push(sfx);
    return 0;
}

// ---- input -------------------------------------------------------------------------------------

// The action's state this tick; a zero state when the World has no input or no such action.
input::ActionState action_of(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const input::InputState* in = world_of(L).try_resource<input::InputState>();
    const StringId id(name);
    if (!in || !in->has_action(id)) return {};
    return in->action(id);
}

int l_pressed(lua_State* L) {
    lua_pushboolean(L, action_of(L).pressed);
    return 1;
}
int l_held(lua_State* L) {
    lua_pushboolean(L, action_of(L).held);
    return 1;
}
int l_released(lua_State* L) {
    lua_pushboolean(L, action_of(L).released);
    return 1;
}
int l_axis(lua_State* L) {
    lua_pushnumber(L, action_of(L).x);
    return 1;
}
int l_axis2(lua_State* L) {
    const input::ActionState a = action_of(L);
    lua_pushvector(L, a.x, a.y, 0.0f);
    return 1;
}

// ---- scene -------------------------------------------------------------------------------------

int l_load_scene(lua_State* L) {
    World& w = world_of(L);
    usize len = 0;
    const char* target = luaL_checklstring(L, 1, &len);
    scene::request_change(w, StringView(target, len));
    return 0;
}

int l_quit(lua_State* L) {
    scene::request_quit(world_of(L));
    return 0;
}

// ---- physics -----------------------------------------------------------------------------------

physics::World2D& physics2d(lua_State* L) {
    physics::World2D* pw = world_of(L).try_resource<physics::World2D>();
    if (!pw) luaL_error(L, "this world has no 2D physics");
    return *pw;
}

// The optional entity a query skips (usually the caller's own body).
struct Exclude {
    Entity e;
    physics::QueryFilter filter2d() const {
        physics::QueryFilter f;
        if (!e.is_null()) f.exclude = Span<const Entity>(&e, 1);
        return f;
    }
};

// A hit as a table: entity, point, normal and `last` (the ray's distance, or a shape cast's swept fraction).
void push_hit2d(lua_State* L, const physics::RayHit2D& hit, const char* last) {
    lua_createtable(L, 0, 4);
    push_entity(L, hit.entity);
    lua_setfield(L, -2, "entity");
    push_vec2(L, hit.point);
    lua_setfield(L, -2, "point");
    push_vec2(L, hit.normal);
    lua_setfield(L, -2, "normal");
    lua_pushnumber(L, hit.t);
    lua_setfield(L, -2, last);
}

// mce.raycast2d(origin, dir, max, exclude?) -> {entity, point, normal, distance} or nil
int l_raycast2d(lua_State* L) {
    physics::World2D& pw = physics2d(L);
    const math::vec2 origin = check_vec2(L, 1);
    const math::vec2 dir = check_vec2(L, 2);
    const f32 max = static_cast<f32>(luaL_checknumber(L, 3));
    const Exclude ex{opt_entity(L, 4)};
    const std::optional<physics::RayHit2D> hit = pw.raycast(origin, dir, max, ex.filter2d());
    if (!hit) {
        lua_pushnil(L);
        return 1;
    }
    push_hit2d(L, *hit, "distance");
    return 1;
}

// mce.overlap2d(min, max, exclude?) -> the entities whose shapes' boxes overlap, in body order
int l_overlap2d(lua_State* L) {
    ScriptHost& host = HostAccess::host(L);
    physics::World2D& pw = physics2d(L);
    const math::aabb2 box{check_vec2(L, 1), check_vec2(L, 2)};
    const Exclude ex{opt_entity(L, 3)};
    Array<Entity>& out = HostAccess::entities(host);
    out.clear();
    pw.overlap_aabb(box, out, ex.filter2d());
    lua_createtable(L, static_cast<int>(out.size()), 0);
    for (usize i = 0; i < out.size(); ++i) {
        push_entity(L, out[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mce.move_and_collide(e, delta) -> {position, grounded, ground, ceiling, wall_left, wall_right}
int l_move_and_collide(lua_State* L) {
    physics::World2D& pw = physics2d(L);
    const Entity e = check_entity(L, 1);
    const math::vec2 delta = check_vec2(L, 2);
    if (!pw.has_body(e)) luaL_error(L, "mce.move_and_collide: the entity has no 2D body");
    const physics::MoveResult2D r = pw.move_and_collide(e, delta);
    lua_createtable(L, 0, 6);
    push_vec2(L, r.position);
    lua_setfield(L, -2, "position");
    lua_pushboolean(L, (r.flags & physics::ControllerFlags::Ground) != 0);
    lua_setfield(L, -2, "grounded");
    push_entity(L, r.ground_body);
    lua_setfield(L, -2, "ground");
    lua_pushboolean(L, r.hit_ceiling);
    lua_setfield(L, -2, "ceiling");
    lua_pushboolean(L, (r.flags & physics::ControllerFlags::WallLeft) != 0);
    lua_setfield(L, -2, "wall_left");
    lua_pushboolean(L, (r.flags & physics::ControllerFlags::WallRight) != 0);
    lua_setfield(L, -2, "wall_right");
    return 1;
}

// mce.teleport2d(e, position): puts a 2D body at `position` without sweeping (a respawn, a level's start).
// The physics step writes LocalTransform back from the body, so a plain LocalTransform write does not move a body.
int l_teleport2d(lua_State* L) {
    physics::World2D& pw = physics2d(L);
    World& w = world_of(L);
    const Entity e = check_entity(L, 1);
    const math::vec2 at = check_vec2(L, 2);
    if (!pw.has_body(e)) luaL_error(L, "mce.teleport2d: the entity has no 2D body");
    f32 angle = 0.0f;
    if (LocalTransform* t = w.try_get_mut<LocalTransform>(e)) {
        angle = angle_z(t->rotation);
        t->position.x = at.x;
        t->position.y = at.y;
    }
    pw.teleport(e, at, angle);
    return 0;
}

int l_velocity2d(lua_State* L) {
    physics::World2D& pw = physics2d(L);
    push_vec2(L, pw.linear_velocity(check_entity(L, 1)));
    return 1;
}

int l_set_velocity2d(lua_State* L) {
    physics::World2D& pw = physics2d(L);
    const Entity e = check_entity(L, 1);
    pw.set_linear_velocity(e, check_vec2(L, 2));
    return 0;
}

// mce.impulse2d(e, impulse): at the body's origin (no spin)
int l_impulse2d(lua_State* L) {
    physics::World2D& pw = physics2d(L);
    const Entity e = check_entity(L, 1);
    const math::vec2 impulse = check_vec2(L, 2);
    physics::BodyState s;
    if (!pw.body_state(e, s)) return 0;
    // mce.impulse2d(e, impulse, point?): at `point` (world space) to spin the body, default its origin
    pw.apply_impulse(e, impulse, lua_isnoneornil(L, 3) ? s.position : check_vec2(L, 3));
    return 0;
}

// A body the call needs: a clear error, not a silent no-op.
Entity body_arg(lua_State* L, physics::World2D& pw, const char* fn) {
    const Entity e = check_entity(L, 1);
    if (!pw.has_body(e)) luaL_error(L, "mce.%s: the entity has no 2D body", fn);
    return e;
}

// mce.force2d(e, force): a force on the body's origin, kept until the next physics step
int l_force2d(lua_State* L) {
    physics::World2D& pw = physics2d(L);
    const Entity e = body_arg(L, pw, "force2d");
    pw.apply_force(e, check_vec2(L, 2));
    return 0;
}

// mce.angular_velocity2d(e) -> radians per second
int l_angular_velocity2d(lua_State* L) {
    physics::World2D& pw = physics2d(L);
    const Entity e = body_arg(L, pw, "angular_velocity2d");
    physics::BodyState s;
    pw.body_state(e, s);
    lua_pushnumber(L, s.angular_velocity);
    return 1;
}

int l_set_angular_velocity2d(lua_State* L) {
    physics::World2D& pw = physics2d(L);
    const Entity e = body_arg(L, pw, "set_angular_velocity2d");
    pw.set_angular_velocity(e, static_cast<f32>(luaL_checknumber(L, 2)));
    return 0;
}

// mce.awake2d(e) -> boolean; mce.set_awake2d(e, awake): false puts the body's whole island to sleep
int l_awake2d(lua_State* L) {
    physics::World2D& pw = physics2d(L);
    lua_pushboolean(L, pw.is_awake(body_arg(L, pw, "awake2d")));
    return 1;
}

int l_set_awake2d(lua_State* L) {
    physics::World2D& pw = physics2d(L);
    const Entity e = body_arg(L, pw, "set_awake2d");
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    pw.set_awake(e, lua_toboolean(L, 2) != 0);
    return 0;
}

// mce.set_enabled2d(e, enabled): a disabled body neither moves nor collides
int l_set_enabled2d(lua_State* L) {
    physics::World2D& pw = physics2d(L);
    const Entity e = body_arg(L, pw, "set_enabled2d");
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    pw.set_enabled(e, lua_toboolean(L, 2) != 0);
    return 0;
}

// mce.overlap_point2d(point, exclude?) -> true when a shape contains the point
int l_overlap_point2d(lua_State* L) {
    physics::World2D& pw = physics2d(L);
    const math::vec2 p = check_vec2(L, 1);
    const Exclude ex{opt_entity(L, 2)};
    lua_pushboolean(L, pw.overlap_point(p, ex.filter2d()));
    return 1;
}

// mce.raycast_all2d(origin, dir, max, exclude?) -> every hit along the ray, nearest first
int l_raycast_all2d(lua_State* L) {
    physics::World2D& pw = physics2d(L);
    const math::vec2 origin = check_vec2(L, 1);
    const math::vec2 dir = check_vec2(L, 2);
    const f32 max = static_cast<f32>(luaL_checknumber(L, 3));
    const Exclude ex{opt_entity(L, 4)};
    Array<physics::RayHit2D> hits;
    pw.raycast_all(origin, dir, max, hits, ex.filter2d());
    lua_createtable(L, static_cast<int>(hits.size()), 0);
    for (usize i = 0; i < hits.size(); ++i) {
        push_hit2d(L, hits[i], "distance");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mce.circle_cast2d(from, to, radius, exclude?) / mce.box_cast2d(from, to, half_extents, exclude?)
//   -> {entity, point, normal, fraction} for the first thing the shape hits on its way, or nil
int shape_cast2d(lua_State* L, const physics::ColliderShape& shape) {
    physics::World2D& pw = physics2d(L);
    const math::vec2 from = check_vec2(L, 1);
    const math::vec2 to = check_vec2(L, 2);
    const Exclude ex{opt_entity(L, 4)};
    const std::optional<physics::RayHit2D> hit = pw.shape_cast(shape, from, to, ex.filter2d());
    if (!hit) {
        lua_pushnil(L);
        return 1;
    }
    push_hit2d(L, *hit, "fraction");
    return 1;
}

int l_circle_cast2d(lua_State* L) {
    const f32 r = static_cast<f32>(luaL_checknumber(L, 3));
    if (!(r > 0.0f)) luaL_error(L, "mce.circle_cast2d: the radius must be above 0");
    return shape_cast2d(L, physics::ColliderShape::circle(r));
}

int l_box_cast2d(lua_State* L) {
    const math::vec2 half = check_vec2(L, 3);
    if (!(half.x > 0.0f && half.y > 0.0f)) luaL_error(L, "mce.box_cast2d: the half extents must be above 0");
    return shape_cast2d(L, physics::ColliderShape::box(half));
}

// mce.raycast3d(origin, dir, max, exclude?) -> {entity, point, normal, distance} or nil
int l_raycast3d(lua_State* L) {
    kit::Physics3D* p3 = world_of(L).try_resource<kit::Physics3D>();
    if (!p3) luaL_error(L, "this world has no 3D physics");
    const math::vec3 origin = check_vec3(L, 1);
    const math::vec3 dir = check_vec3(L, 2);
    const f32 max = static_cast<f32>(luaL_checknumber(L, 3));
    const Entity skip = opt_entity(L, 4);
    physics::QueryFilter3D filter;
    if (!skip.is_null()) filter.exclude = Span<const Entity>(&skip, 1);
    const f32 len = math::length(dir);
    if (!(len > 0.0f)) {
        lua_pushnil(L);
        return 1;
    }
    const std::optional<physics::RayHit3D> hit = p3->world.raycast(origin, dir * (1.0f / len), max, filter);
    if (!hit) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 4);
    push_entity(L, hit->entity);
    lua_setfield(L, -2, "entity");
    push_vec3(L, hit->point);
    lua_setfield(L, -2, "point");
    push_vec3(L, hit->normal);
    lua_setfield(L, -2, "normal");
    lua_pushnumber(L, hit->t);
    lua_setfield(L, -2, "distance");
    return 1;
}

const luaL_Reg kWorldLib[] = {
    {"has", l_has},
    {"alive", l_alive},
    {"spawn", l_spawn},
    {"add", l_add},
    {"remove", l_remove},
    {"destroy", l_destroy},
    {"instantiate", l_instantiate},
    {"events", l_events},
    {"emit", l_emit},
    {"signal", l_signal},
    {"signals", l_signals},
    {"play_sound", l_play_sound},
    {"pressed", l_pressed},
    {"held", l_held},
    {"released", l_released},
    {"axis", l_axis},
    {"axis2", l_axis2},
    {"load_scene", l_load_scene},
    {"quit", l_quit},
    {"raycast2d", l_raycast2d},
    {"overlap2d", l_overlap2d},
    {"move_and_collide", l_move_and_collide},
    {"teleport2d", l_teleport2d},
    {"velocity2d", l_velocity2d},
    {"set_velocity2d", l_set_velocity2d},
    {"impulse2d", l_impulse2d},
    {"force2d", l_force2d},
    {"angular_velocity2d", l_angular_velocity2d},
    {"set_angular_velocity2d", l_set_angular_velocity2d},
    {"awake2d", l_awake2d},
    {"set_awake2d", l_set_awake2d},
    {"set_enabled2d", l_set_enabled2d},
    {"overlap_point2d", l_overlap_point2d},
    {"raycast_all2d", l_raycast_all2d},
    {"circle_cast2d", l_circle_cast2d},
    {"box_cast2d", l_box_cast2d},
    {"raycast3d", l_raycast3d},
    {nullptr, nullptr},
};

}  // namespace

void register_world_api(lua_State* L) { luaL_register(L, nullptr, kWorldLib); }

}  // namespace mce::script::detail
