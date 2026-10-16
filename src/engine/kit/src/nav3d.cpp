#include "mce/kit/nav3d.h"

#include <cmath>

#include "kit3d_internal.h"
#include "mce/asset/asset_system.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/kit3d.h"
#include "mce/log.h"

namespace mce::kit {

// Padding-free: World::state_hash hashes component bytes.
static_assert(sizeof(NavMeshSurface3D) == 96);
static_assert(sizeof(NavArea3D) == 32);
static_assert(sizeof(NavLink3D) == 24);
static_assert(sizeof(NavObstacle3D) == 32);
static_assert(sizeof(NavAgent3D) == 88);

namespace {

struct AgentSlot {
    Entity e;
    i32 agent = -1;
    math::vec3 target;
    math::vec3 written;  // the position the system last put the entity at
    physics::NavAgentParams3D params;
    bool moving = false;
    bool seen = false;
};

struct ObstacleSlot {
    Entity e;
    u32 id = 0;
    math::vec3 center;
    math::vec3 half;
    f32 yaw = 0.0f;
    bool seen = false;
};

// What "kit3d_nav" keeps between ticks: which surface (and which bake of it) is loaded, and the
// crowd agents and obstacles of the entities.
struct Nav3DState {
    Entity surface;
    asset::Guid baked;
    u32 version = 0;
    bool built = false;   // the key above was tried (loaded or failed: no retry until it changes)
    Array<AgentSlot> agents;
    Array<ObstacleSlot> obstacles;
};

bool same(math::vec3 a, math::vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

f32 yaw_of(math::quat r) {
    const math::vec3 x = math::rotate(r, {1.0f, 0.0f, 0.0f});
    return std::atan2(-x.z, x.x);
}

physics::NavAgentParams3D params_of(const NavAgent3D& a) {
    physics::NavAgentParams3D p;
    p.radius = a.radius;
    p.height = a.height;
    p.max_speed = a.speed;
    p.max_acceleration = a.acceleration;
    p.separation = a.separation;
    p.arrive_distance = a.arrive_distance;
    p.avoidance_quality = static_cast<u8>(a.avoidance > 3 ? 3 : a.avoidance);
    p.avoid_agents = a.avoid_agents != 0;
    return p;
}

bool same_params(const physics::NavAgentParams3D& a, const physics::NavAgentParams3D& b) {
    return a.radius == b.radius && a.height == b.height && a.max_speed == b.max_speed &&
           a.max_acceleration == b.max_acceleration && a.separation == b.separation &&
           a.arrive_distance == b.arrive_distance && a.avoidance_quality == b.avoidance_quality &&
           a.avoid_agents == b.avoid_agents;
}

bool in_layers(u32 mask, u32 layer) { return (mask & (1u << (layer & 15u))) != 0; }

// The surface's mesh: the baked asset, else a bake of the scene now.
bool build_mesh(World& w, Entity surface, const NavMeshSurface3D& s, physics::NavMesh3D& mesh) {
    asset::AssetSystem* assets = kit3d_assets(w);
    if (!s.baked.guid.is_null()) {
        if (!assets) {
            MCE_LOG_WARN("nav3d: NavMeshSurface3D on entity {}: no asset system to load its bake", surface.index);
            return false;
        }
        Result<asset::AssetHandle<asset::NavMesh>> h = assets->load_sync<asset::NavMesh>(s.baked.guid);
        if (!h) {
            MCE_LOG_WARN("nav3d: NavMeshSurface3D on entity {}: navmesh {} does not load", surface.index,
                         asset::to_string(s.baked.guid).view());
            return false;
        }
        const bool ok = mesh.load(assets->resolve(h.value()), s.max_agents);
        assets->release(asset::AnyHandle(h.value()));
        return ok;
    }
    Array<byte> blob;
    if (!bake_scene_navmesh(w, surface, assets, blob)) return false;
    return mesh.load(blob.span(), s.max_agents);
}

void apply_area_costs(World& w, physics::NavMesh3D& mesh) {
    u32 set = 0;  // bit per area id already costed (the first volume wins)
    w.query<const NavArea3D>().each([&](const NavArea3D& a) {
        if (a.area <= physics::kNavAreaGround || a.area >= physics::kNavAreaCount || (set & (1u << a.area))) return;
        set |= 1u << a.area;
        if (mesh.area_cost(static_cast<u8>(a.area)) != a.cost) mesh.set_area_cost(static_cast<u8>(a.area), a.cost);
    });
}

void sync_obstacles(World& w, Nav3DState& st, physics::NavMesh3D& mesh) {
    for (ObstacleSlot& s : st.obstacles) s.seen = false;
    w.query<const NavObstacle3D, const LocalTransform>().each([&](Entity e, const NavObstacle3D& o, const LocalTransform& t) {
        ObstacleSlot* slot = nullptr;
        for (ObstacleSlot& s : st.obstacles)
            if (s.e == e) slot = &s;
        if (!o.enabled) return;  // unseen: removed below
        math::vec3 pos, scale;
        math::quat rot;
        world_pose(w, e, t, pos, rot, scale);
        const math::vec3 center = pos + math::rotate(rot, {o.offset.x * scale.x, o.offset.y * scale.y, o.offset.z * scale.z});
        const math::vec3 half{std::fabs(o.half.x * scale.x), std::fabs(o.half.y * scale.y), std::fabs(o.half.z * scale.z)};
        const f32 yaw = yaw_of(rot);
        if (slot && same(slot->center, center) && same(slot->half, half) && slot->yaw == yaw) {
            slot->seen = true;
            return;
        }
        if (slot) mesh.remove_obstacle(slot->id);
        else slot = &st.obstacles.emplace_back();
        slot->e = e;
        slot->id = mesh.add_obstacle(center, half, yaw);
        slot->center = center;
        slot->half = half;
        slot->yaw = yaw;
        slot->seen = true;
        if (!slot->id) MCE_LOG_WARN("nav3d: NavObstacle3D on entity {}: obstacle limit reached", e.index);
    });
    for (usize i = st.obstacles.size(); i-- > 0;) {
        if (st.obstacles[i].seen) continue;
        mesh.remove_obstacle(st.obstacles[i].id);
        st.obstacles.erase(i);
    }
}

AgentSlot* find_agent(Nav3DState& st, Entity e) {
    for (AgentSlot& s : st.agents)
        if (s.e == e) return &s;
    return nullptr;
}

// FixedUpdate_Pre: the mesh, obstacles, the crowd step, the agents' writes.
void kit3d_nav(Frame& f) {
    World& w = f.world();
    Nav3D& nav = f.resource<Nav3D>();
    Nav3DState& st = f.resource<Nav3DState>();

    Entity surface;
    const NavMeshSurface3D* sc = nullptr;
    w.query<const NavMeshSurface3D>().each([&](Entity e, const NavMeshSurface3D& s) {
        if (sc) return;
        surface = e;
        sc = &s;
    });
    if (!sc) {
        if (nav.mesh.loaded()) nav.mesh.clear();
        nav.surface = Entity::null;
        st = Nav3DState{};
        return;
    }
    const bool key_changed = !st.built || st.surface != surface || st.baked != sc->baked.guid ||
                             (sc->baked.guid.is_null() && st.version != sc->version);
    if (key_changed) {
        st.surface = surface;
        st.baked = sc->baked.guid;
        st.version = sc->version;
        st.built = true;
        st.agents.clear();
        st.obstacles.clear();
        nav.surface = surface;
        if (!build_mesh(w, surface, *sc, nav.mesh)) {
            nav.mesh.clear();
            MCE_LOG_WARN("nav3d: no navmesh for the NavMeshSurface3D on entity {}", surface.index);
        }
        NavMeshSurface3D* sm = w.try_get_mut<NavMeshSurface3D>(surface);
        sm->ready = nav.mesh.loaded() ? 1u : 0u;
        sm->polys = nav.mesh.stats().polys;
    }
    if (!nav.mesh.loaded()) return;
    physics::NavMesh3D& mesh = nav.mesh;
    apply_area_costs(w, mesh);
    sync_obstacles(w, st, mesh);

    // Agents: add, re-param, follow outside moves, (re)target; drop the gone.
    for (AgentSlot& s : st.agents) s.seen = false;
    f.query<NavAgent3D, const LocalTransform>().each([&](Entity e, NavAgent3D& a, const LocalTransform& t) {
        math::vec3 pos, scale;
        math::quat rot;
        world_pose(w, e, t, pos, rot, scale);
        // A followed entity's position is the target; small moves keep the current plan.
        bool replan = false;
        if (!a.follow.is_null() && w.alive(a.follow)) {
            if (const LocalTransform* ft = w.try_get<LocalTransform>(a.follow)) {
                math::vec3 fpos, fscale;
                math::quat frot;
                world_pose(w, a.follow, *ft, fpos, frot, fscale);
                a.target = fpos;
            }
        }
        AgentSlot* slot = find_agent(st, e);
        const physics::NavAgentParams3D params = params_of(a);
        if (!slot) {
            slot = &st.agents.emplace_back();
            slot->e = e;
            slot->agent = mesh.add_agent(pos, params);
            slot->params = params;
            slot->written = pos;
            if (slot->agent < 0) MCE_LOG_WARN("nav3d: NavAgent3D on entity {}: the crowd is full", e.index);
        }
        slot->seen = true;
        if (slot->agent < 0) return;
        if (!same_params(slot->params, params)) {
            mesh.set_agent_params(slot->agent, params);
            slot->params = params;
        }
        // A controller (or anything outside) moved the entity: the agent follows.
        if (!same(slot->written, pos)) mesh.move_agent(slot->agent, pos);
        if (a.follow.is_null())
            replan = !same(slot->target, a.target);
        else
            replan = math::length_sq(slot->target - a.target) > a.follow_repath * a.follow_repath;
        if (a.move_to && (!slot->moving || replan)) {
            mesh.set_agent_target(slot->agent, a.target);
            slot->moving = true;
            slot->target = a.target;
        } else if (!a.move_to && slot->moving) {
            mesh.stop_agent(slot->agent);
            slot->moving = false;
        }
    });
    for (usize i = st.agents.size(); i-- > 0;) {
        if (st.agents[i].seen) continue;
        if (st.agents[i].agent >= 0) mesh.remove_agent(st.agents[i].agent);
        st.agents.erase(i);
    }

    mesh.update(f.dt);

    f.query<NavAgent3D, LocalTransform>().each([&](Entity e, NavAgent3D& a, LocalTransform& t) {
        AgentSlot* slot = find_agent(st, e);
        physics::NavAgentStatus3D s;
        if (!slot || slot->agent < 0 || !mesh.agent_status(slot->agent, s)) {
            a.state = NavAgentState::OffMesh;
            return;
        }
        const NavAgentState state = static_cast<NavAgentState>(s.state);
        if (state == NavAgentState::Arrived && a.state != NavAgentState::Arrived) ++a.arrivals;
        a.state = state;
        a.velocity = s.velocity;
        a.distance = s.distance;
        a.next_corner = s.next_corner;
        if (CharacterController3D* c = w.try_get_mut<CharacterController3D>(e)) {
            c->move = {s.velocity.x, 0.0f, s.velocity.z};
        } else if (state != NavAgentState::OffMesh) {
            t.position = s.position;  // a root entity: the agent's position is world space
        }
        const f32 speed2 = s.velocity.x * s.velocity.x + s.velocity.z * s.velocity.z;
        if (a.face_motion && speed2 > 0.01f)
            t.rotation = math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, std::atan2(-s.velocity.x, -s.velocity.z));
        math::vec3 pos, scale;
        math::quat rot;
        world_pose(w, e, t, pos, rot, scale);
        slot->written = pos;
    });
}

}  // namespace

void gather_nav_geometry(World& w, Entity surface, asset::AssetSystem* assets, physics::NavGeometry3D& g,
                         physics::NavBakeSettings3D& settings) {
    g = physics::NavGeometry3D{};
    settings = physics::NavBakeSettings3D{};
    const NavMeshSurface3D* s = w.alive(surface) ? w.try_get<NavMeshSurface3D>(surface) : nullptr;
    if (!s) return;
    settings.cell_size = s->cell_size;
    settings.cell_height = s->cell_height;
    settings.agent_height = s->agent_height;
    settings.agent_radius = s->agent_radius;
    settings.agent_max_climb = s->agent_max_climb;
    settings.agent_max_slope_deg = s->agent_max_slope_deg;
    settings.tile_size = s->tile_size;
    settings.max_obstacles = s->max_obstacles;
    if (const LocalTransform* st = w.try_get<LocalTransform>(surface);
        st && (s->bounds_half.x > 0.0f || s->bounds_half.y > 0.0f || s->bounds_half.z > 0.0f)) {
        math::vec3 pos, scale;
        math::quat rot;
        world_pose(w, surface, *st, pos, rot, scale);
        const math::vec3 h{std::fabs(s->bounds_half.x), std::fabs(s->bounds_half.y), std::fabs(s->bounds_half.z)};
        settings.bounds = {pos - h, pos + h};
    }
    const u32 mask = s->layer_mask;

    w.query<const BoxCollider3D, const LocalTransform>().each([&](Entity e, const BoxCollider3D& c, const LocalTransform& t) {
        if (!in_layers(mask, c.layer)) return;
        math::vec3 pos, scale;
        math::quat rot;
        world_pose(w, e, t, pos, rot, scale);
        const math::quat r = rot * math::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, math::radians(c.pitch_deg));
        const math::vec3 p = pos + math::rotate(rot, {c.offset.x * scale.x, c.offset.y * scale.y, c.offset.z * scale.z});
        const math::vec3 half{std::fabs(c.half.x * scale.x), std::fabs(c.half.y * scale.y), std::fabs(c.half.z * scale.z)};
        g.add_box(math::trs(p, r, {1.0f, 1.0f, 1.0f}), half);
    });
    auto add_mesh = [&](Entity e, const asset::Guid& guid, const LocalTransform& t, const char* what) {
        if (!assets || guid.is_null()) return;
        Result<asset::AssetHandle<asset::Mesh>> h = assets->load_sync<asset::Mesh>(guid);
        if (!h) {
            MCE_LOG_WARN("nav3d: the {} mesh on entity {} does not load; left out of the navmesh", what, e.index);
            return;
        }
        math::vec3 pos, scale;
        math::quat rot;
        world_pose(w, e, t, pos, rot, scale);
        Array<math::vec3> verts;
        Array<u32> indices;
        const bool read = mesh_triangles(assets->resolve(h.value()), math::scaling(scale), verts, indices);
        assets->release(asset::AnyHandle(h.value()));
        if (read) g.add_mesh(verts.span(), indices.span(), math::trs(pos, rot, {1.0f, 1.0f, 1.0f}));
    };
    w.query<const MeshCollider3D, const LocalTransform>().each([&](Entity e, const MeshCollider3D& c, const LocalTransform& t) {
        if (in_layers(mask, c.layer)) add_mesh(e, c.mesh.guid, t, "MeshCollider3D");
    });
    if (s->include_meshes) {
        w.query<const MeshRef, const LocalTransform>().each([&](Entity e, const MeshRef& m, const LocalTransform& t) {
            if (w.has<BoxCollider3D>(e) || w.has<MeshCollider3D>(e) || w.has<RigidBody3D>(e) ||
                w.has<CharacterController3D>(e) || w.has<NavAgent3D>(e) || w.has<NavObstacle3D>(e))
                return;
            add_mesh(e, m.mesh.guid, t, "MeshRef");
        });
    }

    w.query<const NavArea3D, const LocalTransform>().each([&](Entity e, const NavArea3D& a, const LocalTransform& t) {
        math::vec3 pos, scale;
        math::quat rot;
        world_pose(w, e, t, pos, rot, scale);
        physics::NavAreaVolume3D v;
        v.center = pos + math::rotate(rot, {a.offset.x * scale.x, a.offset.y * scale.y, a.offset.z * scale.z});
        v.half = {std::fabs(a.half.x * scale.x), std::fabs(a.half.y * scale.y), std::fabs(a.half.z * scale.z)};
        v.area = static_cast<u8>(a.area < physics::kNavAreaCount ? a.area : physics::kNavAreaGround);
        g.volumes.push_back(v);
    });
    w.query<const NavLink3D, const LocalTransform>().each([&](Entity e, const NavLink3D& l, const LocalTransform& t) {
        math::vec3 pos, scale;
        math::quat rot;
        world_pose(w, e, t, pos, rot, scale);
        physics::NavLinkDesc3D d;
        d.start = pos;
        d.end = pos + math::rotate(rot, {l.end.x * scale.x, l.end.y * scale.y, l.end.z * scale.z});
        d.radius = l.radius;
        d.area = static_cast<u8>(l.area >= 1 && l.area < physics::kNavAreaCount ? l.area : physics::kNavAreaGround);
        d.bidirectional = l.bidirectional != 0;
        d.user_id = e.index;
        g.links.push_back(d);
    });
}

bool bake_scene_navmesh(World& w, Entity surface, asset::AssetSystem* assets, Array<byte>& out, physics::NavBakeStats3D* stats) {
    physics::NavGeometry3D g;
    physics::NavBakeSettings3D settings;
    gather_nav_geometry(w, surface, assets, g, settings);
    if (g.indices.empty()) {
        MCE_LOG_WARN("nav3d: nothing to bake for the NavMeshSurface3D on entity {} (no colliders)", surface.index);
        out.clear();
        return false;
    }
    return physics::bake_navmesh(g, settings, out, stats);
}

void install_nav3d(World& world, asset::AssetSystem* assets) {
    install_bodies3d(world, assets);
    if (world.try_resource<Nav3DState>()) return;
    world.set_resource(Nav3DState{});
    world.set_resource(Nav3D{});
    world.register_components<NavMeshSurface3D, NavArea3D, NavLink3D, NavObstacle3D, NavAgent3D>();
    world.add_system(Phase::FixedUpdate_Pre, "kit3d_nav", &kit3d_nav);
}

void nav3d_debug_geometry(World& world, Array<math::vec3>& tris, Array<u8>& areas, Array<math::vec3>& lines) {
    tris.clear();
    areas.clear();
    lines.clear();
    Nav3D* nav = world.try_resource<Nav3D>();
    if (!nav || !nav->mesh.loaded()) return;
    nav->mesh.debug_triangles(tris, areas, 0.03f, &lines);
    const Nav3DState* st = world.try_resource<Nav3DState>();
    if (!st) return;
    Array<math::vec3> corners;
    for (const AgentSlot& s : st->agents) {
        physics::NavAgentStatus3D a;
        if (s.agent < 0 || !nav->mesh.agent_status(s.agent, a) || a.state != physics::NavAgentState3D::Moving) continue;
        nav->mesh.agent_corners(s.agent, corners);
        math::vec3 prev = a.position;
        for (const math::vec3& c : corners) {
            lines.push_back(prev);
            lines.push_back(c);
            prev = c;
        }
    }
}

}  // namespace mce::kit
