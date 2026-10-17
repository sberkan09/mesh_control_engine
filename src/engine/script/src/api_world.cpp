// mce_script (M-SCRIPT step 2): the `mce` functions that reach past one component's fields:
// entities (Commands), events, the kit's signals, input, scene changes and the physics queries.
// script.cpp has the field access and the host; api.h the helpers both share.
//
// Every function may raise (LUA_USE_LONGJMP): engine calls whose results own memory run in a
// scope that ends before the next luaL_* call, and lists go through the host's scratch arrays.

#include <cmath>
#include <cstring>

#include "api.h"
#include "mce/asset/asset_system.h"
#include "mce/asset/guid.h"
#include "mce/audio/components.h"
#include "mce/convar.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/hash.h"
#include "mce/ecs/prefab.h"
#include "mce/input/actions.h"
#include "mce/kit/ai.h"
#include "mce/kit/cinematic.h"
#include "mce/kit/effects.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/nav3d.h"
#include "mce/kit/pathfinding.h"
#include "mce/kit/perception.h"
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

// mce.make_noise(position, loudness?, source?, team?): a kit::Noise that perceivers hear on the next
// tick (loudness in metres a hearing-1 perceiver hears it from, default 10; perceivers of `team` ignore it).
int l_make_noise(lua_State* L) {
    Frame& f = frame_of(L);
    kit::Noise n;
    n.position = check_vec3(L, 1);
    n.loudness = static_cast<f32>(luaL_optnumber(L, 2, 10.0));
    if (!(n.loudness >= 0.0f)) luaL_error(L, "mce.make_noise: loudness must be 0 or more");
    n.source = opt_entity(L, 3);
    const double team = luaL_optnumber(L, 4, 0.0);
    if (!(team >= 0.0 && team < 4294967296.0)) luaL_error(L, "mce.make_noise: team must be 0 or a positive whole number");
    n.team = static_cast<u32>(team);
    if (!f.world().event_queue<kit::Noise>()) luaL_error(L, "mce.make_noise: this world has no perception (install_ai)");
    f.events().push(n);
    return 0;
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

// ---- game plumbing (M-SCRIPT2 3) ---------------------------------------------------------------

// A save slot is a file name: letters, digits, '_' and '-', 1..64, so a script can never name a path.
const char* slot_arg(lua_State* L, int idx) {
    const char* s = luaL_checkstring(L, idx);
    const StringView sv(s);
    bool ok = !sv.empty() && sv.size() <= 64;
    for (char c : sv) ok = ok && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-');
    if (!ok) luaL_error(L, "save slot '%s' must be 1 to 64 letters, digits, '_' or '-'", s);
    return s;
}

// mce.save(slot) -> true, or false and a reason. Writes the world now (a read of the world, so it is safe
// mid-tick); the scene recorded is the one the host last loaded or changed to.
int l_save(lua_State* L) {
    World& w = world_of(L);
    const StringView slot(slot_arg(L, 1));
    save::SaveInfo info;
    if (const scene::SceneChange* c = w.try_resource<scene::SceneChange>()) info.scene = c->current_guid;
    info.tick = frame_of(L).tick;
    info.playtime = static_cast<f64>(info.tick) * static_cast<f64>(frame_of(L).dt);
    Result<void> r = save::save_game(w, slot, info);
    lua_pushboolean(L, r.has_value());
    if (r) return 1;
    FixedString<512> why;
    format_error(r.error(), why);
    lua_pushstring(L, why.c_str());
    return 2;
}

// mce.load(slot) -> true when the slot exists and the load is queued (the host applies it after this tick).
int l_load(lua_State* L) {
    const StringView slot(slot_arg(L, 1));
    const bool there = save::exists(slot);
    if (there) scene::request_load(world_of(L), slot);
    lua_pushboolean(L, there);
    return 1;
}

int l_has_save(lua_State* L) {
    lua_pushboolean(L, save::exists(StringView(slot_arg(L, 1))));
    return 1;
}

// mce.set_time_scale(s): how fast the game clock runs against the wall, 0.1 to 4 (hit-stop, slow motion).
// It scales the player's accumulator, never dt or the sim, so it has no effect in headless runs.
int l_set_time_scale(lua_State* L) {
    const f64 s = luaL_checknumber(L, 1);
    if (!(s >= 0.1 && s <= 4.0)) luaL_error(L, "mce.set_time_scale: %g is outside 0.1 to 4", s);
    if (ConVar* cv = ConVar::find(StringId("time_scale"))) cv->set_float(static_cast<f32>(s), true);
    return 0;
}

int l_time_scale(lua_State* L) {
    ConVar* cv = ConVar::find(StringId("time_scale"));
    lua_pushnumber(L, cv ? static_cast<double>(cv->get_float()) : 1.0);
    return 1;
}

// mce.shake(trauma): adds trauma (0..1, clamped to 1 in total) to every CameraShake2D / CameraShake3D.
int l_shake(lua_State* L) {
    World& w = world_of(L);
    const f32 t = static_cast<f32>(luaL_checknumber(L, 1));
    if (!(t >= 0.0f && t <= 1.0f)) luaL_error(L, "mce.shake: trauma %g is outside 0 to 1", static_cast<double>(t));
    w.query<kit::CameraShake2D>().each([&](kit::CameraShake2D& s) { s.trauma = s.trauma + t > 1.0f ? 1.0f : s.trauma + t; });
    w.query<kit::CameraShake3D>().each([&](kit::CameraShake3D& s) { s.trauma = s.trauma + t > 1.0f ? 1.0f : s.trauma + t; });
    return 0;
}

// mce.burst(e, count?): fires the entity's ParticleBurst once (count overrides the component's).
int l_burst(lua_State* L) {
    World& w = world_of(L);
    const Entity e = check_entity(L, 1);
    kit::ParticleBurst* b = w.try_get_mut<kit::ParticleBurst>(e);
    if (!b) luaL_error(L, "mce.burst: the entity has no ParticleBurst");
    if (!lua_isnoneornil(L, 2)) {
        const f64 n = luaL_checknumber(L, 2);
        if (!(n >= 1 && n <= 4096)) luaL_error(L, "mce.burst: count %g is outside 1 to 4096", n);
        b->count = static_cast<u32>(n);
    }
    b->fire = true;
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

// mce.find_path(from, to, options?) -> array of waypoints (vectors, z = 0) after `from`, ending at `to`; nil when `to`
// is blocked or cannot be reached. The grid is made from the world's static colliders (the same ones
// kit::mark_static_colliders reads), so it follows a level that was rebuilt; the last one is kept in the world and
// reused while the colliders, `cell` and `radius` stay the same. options: cell (grid size in metres, default 0.5),
// radius (clearance kept from walls, default 0.25), diagonal (default true), smooth (default true).
struct ScriptNavCache {
    u64 key = 0;
    bool valid = false;
    kit::NavGrid grid;
    kit::Pathfinder finder;
};

int l_find_path(lua_State* L) {
    World& w = world_of(L);
    const math::vec2 from = check_vec2(L, 1);
    const math::vec2 to = check_vec2(L, 2);
    f32 cell = 0.5f, radius = 0.25f;
    kit::PathOptions opt;
    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        lua_getfield(L, 3, "cell");
        if (!lua_isnil(L, -1)) cell = static_cast<f32>(luaL_checknumber(L, -1));
        lua_pop(L, 1);
        lua_getfield(L, 3, "radius");
        if (!lua_isnil(L, -1)) radius = static_cast<f32>(luaL_checknumber(L, -1));
        lua_pop(L, 1);
        lua_getfield(L, 3, "diagonal");
        if (!lua_isnil(L, -1)) opt.diagonal = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, 3, "smooth");
        if (!lua_isnil(L, -1)) opt.smooth = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    }
    if (!(cell >= 0.05f && cell <= 100.0f)) luaL_error(L, "mce.find_path: cell must be from 0.05 to 100 metres");
    if (!(radius >= 0.0f && radius <= 100.0f)) luaL_error(L, "mce.find_path: radius must be from 0 to 100 metres");

    const math::aabb2 bounds = kit::static_collider_bounds(w);
    Array<math::vec2> path;
    lua_createtable(L, 0, 0);
    if (bounds.min.x > bounds.max.x) {  // nothing to walk around
        push_vec2(L, to);
        lua_rawseti(L, -2, 1);
        return 1;
    }
    Hasher64 h;
    h.u64v(kit::static_colliders_hash(w));
    h.f32v(cell);
    h.f32v(radius);
    ScriptNavCache* cache = w.try_resource<ScriptNavCache>();
    if (!cache) {
        w.set_resource(ScriptNavCache{});
        cache = w.try_resource<ScriptNavCache>();
    }
    if (!cache->valid || cache->key != h.value()) {
        const f32 margin = cell * 4.0f + radius;
        const math::vec2 lo = bounds.min - math::vec2(margin), hi = bounds.max + math::vec2(margin);
        const f64 cols = std::ceil(static_cast<f64>(hi.x - lo.x) / static_cast<f64>(cell));
        const f64 rows = std::ceil(static_cast<f64>(hi.y - lo.y) / static_cast<f64>(cell));
        if (cols * rows > 4.0e6) luaL_error(L, "mce.find_path: the level needs a %.0f x %.0f grid at cell %g; raise 'cell'", cols, rows, static_cast<double>(cell));
        cache->grid = kit::NavGrid(static_cast<u32>(cols), static_cast<u32>(rows), cell, lo, 1);
        kit::mark_static_colliders(cache->grid, w, radius);
        cache->key = h.value();
        cache->valid = true;
    }
    // The grid covers the walls plus a free margin. Anything outside it is open ground: walk to the
    // nearest edge cell, and tack the real point on at that end.
    const math::vec2 glo = cache->grid.origin();
    const math::vec2 ghi = glo + math::vec2(static_cast<f32>(cache->grid.width()), static_cast<f32>(cache->grid.height())) * cache->grid.cell_size();
    const f32 inset = cache->grid.cell_size() * 0.5f;
    const auto clamp_in = [&](math::vec2 p) {
        return math::vec2(std::min(std::max(p.x, glo.x + inset), ghi.x - inset), std::min(std::max(p.y, glo.y + inset), ghi.y - inset));
    };
    const math::vec2 cfrom = clamp_in(from), cto = clamp_in(to);
    const bool from_out = cfrom.x != from.x || cfrom.y != from.y, to_out = cto.x != to.x || cto.y != to.y;
    if (!cache->finder.find_path(cache->grid, cfrom, cto, opt, path)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return 1;
    }
    if (from_out) path.insert(0, from);
    if (to_out) path.push_back(to);
    for (usize i = 0; i < path.size(); ++i) {
        push_vec2(L, path[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
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

// ---- 3D bodies (M-SCRIPT2 4) -------------------------------------------------------------------
// Reads and body writes over the kit's physics::World3D, as the 2D calls are over World2D. Character walking is not
// here on purpose: the kit's CharacterController3D (move, jump) is data a script sets, and the kit's own system moves it.

physics::World3D& physics3d(lua_State* L) {
    kit::Physics3D* p3 = world_of(L).try_resource<kit::Physics3D>();
    if (!p3) luaL_error(L, "this world has no 3D physics");
    return p3->world;
}

physics::World3D& body3d(lua_State* L, Entity e, const char* fn) {
    physics::World3D& pw = physics3d(L);
    if (!pw.has_body(e)) luaL_error(L, "mce.%s: the entity has no 3D body", fn);
    return pw;
}

constexpr usize kMaxOverlaps3d = 256;

int push_overlaps3d(lua_State* L, Array<Entity>& out, u32 n) {
    const usize count = n < out.size() ? n : out.size();
    lua_createtable(L, static_cast<int>(count), 0);
    for (usize i = 0; i < count; ++i) {
        push_entity(L, out[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mce.overlap_sphere3d(center, radius, skip?) -> the entities whose shapes touch the sphere (at most 256)
int l_overlap_sphere3d(lua_State* L) {
    physics::World3D& pw = physics3d(L);
    const math::vec3 center = check_vec3(L, 1);
    const f32 radius = static_cast<f32>(luaL_checknumber(L, 2));
    if (!(radius > 0.0f)) luaL_error(L, "mce.overlap_sphere3d: the radius must be above 0");
    const Entity skip = opt_entity(L, 3);
    physics::QueryFilter3D filter;
    if (!skip.is_null()) filter.exclude = Span<const Entity>(&skip, 1);
    Array<Entity>& out = HostAccess::entities(HostAccess::host(L));
    out.resize(kMaxOverlaps3d);
    return push_overlaps3d(L, out, pw.overlap_sphere(center, radius, Span<Entity>(out.data(), out.size()), filter));
}

// mce.overlap_box3d(center, half, skip?) -> the same for an axis-aligned box
int l_overlap_box3d(lua_State* L) {
    physics::World3D& pw = physics3d(L);
    const math::vec3 center = check_vec3(L, 1);
    const math::vec3 half = check_vec3(L, 2);
    if (!(half.x > 0.0f && half.y > 0.0f && half.z > 0.0f)) luaL_error(L, "mce.overlap_box3d: the half extents must be above 0");
    const Entity skip = opt_entity(L, 3);
    physics::QueryFilter3D filter;
    if (!skip.is_null()) filter.exclude = Span<const Entity>(&skip, 1);
    Array<Entity>& out = HostAccess::entities(HostAccess::host(L));
    out.resize(kMaxOverlaps3d);
    return push_overlaps3d(L, out, pw.overlap_box(center, half, math::quat::identity(), Span<Entity>(out.data(), out.size()), filter));
}

// mce.sphere_cast3d(from, to, radius, skip?) -> {entity, point, normal, position, fraction} or nil
int l_sphere_cast3d(lua_State* L) {
    physics::World3D& pw = physics3d(L);
    const math::vec3 from = check_vec3(L, 1);
    const math::vec3 to = check_vec3(L, 2);
    const f32 radius = static_cast<f32>(luaL_checknumber(L, 3));
    if (!(radius > 0.0f)) luaL_error(L, "mce.sphere_cast3d: the radius must be above 0");
    const Entity skip = opt_entity(L, 4);
    physics::QueryFilter3D filter;
    if (!skip.is_null()) filter.exclude = Span<const Entity>(&skip, 1);
    const std::optional<physics::ShapeHit3D> hit = pw.shape_cast(radius, from, to, filter);
    if (!hit) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 5);
    push_entity(L, hit->entity);
    lua_setfield(L, -2, "entity");
    push_vec3(L, hit->point);
    lua_setfield(L, -2, "point");
    push_vec3(L, hit->normal);
    lua_setfield(L, -2, "normal");
    push_vec3(L, hit->position);
    lua_setfield(L, -2, "position");
    lua_pushnumber(L, hit->t);
    lua_setfield(L, -2, "fraction");
    return 1;
}

int l_velocity3d(lua_State* L) {
    const Entity e = check_entity(L, 1);
    physics::BodyState3D s;
    if (!body3d(L, e, "velocity3d").body_state(e, s)) luaL_error(L, "mce.velocity3d: the entity has no 3D body");
    push_vec3(L, s.linear_velocity);
    return 1;
}

int l_set_velocity3d(lua_State* L) {
    const Entity e = check_entity(L, 1);
    body3d(L, e, "set_velocity3d").set_linear_velocity(e, check_vec3(L, 2));
    return 0;
}

int l_angular_velocity3d(lua_State* L) {
    const Entity e = check_entity(L, 1);
    physics::BodyState3D s;
    if (!body3d(L, e, "angular_velocity3d").body_state(e, s)) luaL_error(L, "mce.angular_velocity3d: the entity has no 3D body");
    push_vec3(L, s.angular_velocity);
    return 1;
}

int l_set_angular_velocity3d(lua_State* L) {
    const Entity e = check_entity(L, 1);
    body3d(L, e, "set_angular_velocity3d").set_angular_velocity(e, check_vec3(L, 2));
    return 0;
}

// mce.impulse3d(e, impulse): at the centre of mass (dynamic bodies)
int l_impulse3d(lua_State* L) {
    const Entity e = check_entity(L, 1);
    body3d(L, e, "impulse3d").apply_impulse(e, check_vec3(L, 2));
    return 0;
}

// Rotations in Luau are Euler angles in degrees as a vector (x = pitch, y = yaw, z = roll), applied yaw, then pitch,
// then roll (q = Ry * Rx * Rz). Forward is -Z and up +Y, as the camera and look_at_rh have it; no quaternion type exists
// in the sandbox and a script never needs one.
constexpr f32 kDegToRad = 0.017453292519943295f;
constexpr f32 kRadToDeg = 57.29577951308232f;

math::quat quat_from_euler_deg(math::vec3 d) {
    const math::quat yaw = math::quat_from_axis_angle({0, 1, 0}, d.y * kDegToRad);
    const math::quat pitch = math::quat_from_axis_angle({1, 0, 0}, d.x * kDegToRad);
    const math::quat roll = math::quat_from_axis_angle({0, 0, 1}, d.z * kDegToRad);
    return math::normalize(yaw * pitch * roll);
}

math::vec3 euler_deg_from_quat(math::quat q) {
    q = math::normalize(q);
    const math::vec3 f = math::rotate(q, {0, 0, -1});
    const f32 pitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, f.y)));
    const f32 yaw = std::atan2(-f.x, -f.z);  // 0 at +-90 degrees of pitch, where yaw and roll are one angle
    const math::quat yp = math::quat_from_axis_angle({0, 1, 0}, yaw) * math::quat_from_axis_angle({1, 0, 0}, pitch);
    const math::quat r = math::conjugate(yp) * q;
    f32 roll = 2.0f * std::atan2(r.z, r.w);
    if (roll > 3.14159265f) roll -= 6.28318531f;
    if (roll < -3.14159265f) roll += 6.28318531f;
    return {pitch * kRadToDeg, yaw * kRadToDeg, roll * kRadToDeg};
}

// mce.look_rotation3d(from, to) -> the rotation (degrees) that turns -Z from `from` toward `to`, roll 0. Pure: no entity.
int l_look_rotation3d(lua_State* L) {
    const math::vec3 d = check_vec3(L, 2) - check_vec3(L, 1);
    const f32 len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (!(len > 1e-6f)) luaL_error(L, "mce.look_rotation3d: from and to are the same point");
    const math::vec3 n{d.x / len, d.y / len, d.z / len};
    push_vec3(L, {std::asin(std::fmax(-1.0f, std::fmin(1.0f, n.y))) * kRadToDeg, std::atan2(-n.x, -n.z) * kRadToDeg, 0.0f});
    return 1;
}

// mce.rotation3d(e) -> the body's rotation in degrees.
int l_rotation3d(lua_State* L) {
    const Entity e = check_entity(L, 1);
    physics::BodyState3D s;
    if (!body3d(L, e, "rotation3d").body_state(e, s)) luaL_error(L, "mce.rotation3d: the entity has no 3D body");
    push_vec3(L, euler_deg_from_quat(s.rotation));
    return 1;
}

// Moves a 3D body to `at` with `rot`, writing LocalTransform too: the physics step writes it back from the body,
// so writing LocalTransform alone moves nothing.
void place3d(lua_State* L, Entity e, math::vec3 at, math::quat rot, physics::World3D& pw) {
    if (LocalTransform* t = world_of(L).try_get_mut<LocalTransform>(e)) {
        t->position = at;
        t->rotation = rot;
    }
    pw.teleport(e, at, rot);
}

// mce.teleport3d(e, position, rotation?): puts a 3D body or character at `position` with no sweep; `rotation` (degrees)
// defaults to the body's own.
int l_teleport3d(lua_State* L) {
    const Entity e = check_entity(L, 1);
    const math::vec3 at = check_vec3(L, 2);
    physics::World3D& pw = body3d(L, e, "teleport3d");
    physics::BodyState3D s;
    math::quat rot = math::quat::identity();
    if (pw.body_state(e, s)) rot = s.rotation;
    if (!lua_isnoneornil(L, 3)) rot = quat_from_euler_deg(check_vec3(L, 3));
    place3d(L, e, at, rot, pw);
    return 0;
}

// mce.set_rotation3d(e, rotation): turns a 3D body in place (degrees, no sweep).
int l_set_rotation3d(lua_State* L) {
    const Entity e = check_entity(L, 1);
    const math::quat rot = quat_from_euler_deg(check_vec3(L, 2));
    physics::World3D& pw = body3d(L, e, "set_rotation3d");
    physics::BodyState3D s;
    if (!pw.body_state(e, s)) luaL_error(L, "mce.set_rotation3d: the entity has no 3D body");
    place3d(L, e, s.position, rot, pw);
    return 0;
}

// mce.find_path3d(from, to) -> the corner points after leaving `from`, ending at `to` (as mce.find_path), and true when
// the path is partial (the target cannot be reached: it ends at the closest reachable point); nil when there is no path.
int l_find_path3d(lua_State* L) {
    World& w = world_of(L);
    const math::vec3 from = check_vec3(L, 1);
    const math::vec3 to = check_vec3(L, 2);
    kit::Nav3D* nav = w.try_resource<kit::Nav3D>();
    if (!nav || !nav->mesh.loaded()) luaL_error(L, "this world has no navmesh (a NavMeshSurface3D bakes one)");
    Array<math::vec3> path;
    const physics::NavPath3D r = nav->mesh.find_path(from, to, path);
    if (r == physics::NavPath3D::None || path.empty()) {
        lua_pushnil(L);
        return 1;
    }
    // The navmesh answers with the start (snapped to the mesh) first; mce.find_path does not list it, and neither does this.
    const usize first = path.size() > 1 ? 1 : 0;
    lua_createtable(L, static_cast<int>(path.size() - first), 0);
    for (usize i = first; i < path.size(); ++i) {
        push_vec3(L, path[i]);
        lua_rawseti(L, -2, static_cast<int>(i - first + 1));
    }
    lua_pushboolean(L, r == physics::NavPath3D::Partial);
    return 2;
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
    {"make_noise", l_make_noise},
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
    {"find_path", l_find_path},
    {"save", l_save},
    {"load", l_load},
    {"has_save", l_has_save},
    {"set_time_scale", l_set_time_scale},
    {"time_scale", l_time_scale},
    {"shake", l_shake},
    {"burst", l_burst},
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
    {"overlap_sphere3d", l_overlap_sphere3d},
    {"overlap_box3d", l_overlap_box3d},
    {"sphere_cast3d", l_sphere_cast3d},
    {"velocity3d", l_velocity3d},
    {"set_velocity3d", l_set_velocity3d},
    {"angular_velocity3d", l_angular_velocity3d},
    {"set_angular_velocity3d", l_set_angular_velocity3d},
    {"impulse3d", l_impulse3d},
    {"teleport3d", l_teleport3d},
    {"rotation3d", l_rotation3d},
    {"set_rotation3d", l_set_rotation3d},
    {"look_rotation3d", l_look_rotation3d},
    {"find_path3d", l_find_path3d},
    {nullptr, nullptr},
};

}  // namespace

void register_world_api(lua_State* L) { luaL_register(L, nullptr, kWorldLib); }

}  // namespace mce::script::detail
