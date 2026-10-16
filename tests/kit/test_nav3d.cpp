// 3D navigation as components (M-GAMEPLAY3D step 1): NavMeshSurface3D baked from the scene's
// colliders at the first tick, NavAgent3D walking around a wall (on its own and steering a
// CharacterController3D), NavArea3D costs, NavLink3D, NavObstacle3D, and the replay hash.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/nav3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

// A 40 x 40 floor (top y = 0) and a wall across x = 0 from z = -20 to z = 10.
struct Scene {
    World w;
    u64 t = 0;
    Entity surface;
    explicit Scene(bool wall = true) {
        w.register_components<LocalTransform>();
        install_nav3d(w, nullptr);
        BoxCollider3D floor;
        floor.half = {20.0f, 0.5f, 20.0f};
        w.spawn_with(at(0, -0.5f, 0), floor);
        if (wall) {
            BoxCollider3D c;
            c.half = {0.5f, 1.0f, 15.0f};
            w.spawn_with(at(0, 1.0f, -5.0f), c);
        }
        NavMeshSurface3D s;
        s.cell_size = 0.25f;
        s.tile_size = 32;
        surface = w.spawn_with(at(0, 0, 0), s);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    Entity agent(math::vec3 from, math::vec3 to) {
        NavAgent3D a;
        a.target = to;
        a.move_to = 1;
        return w.spawn_with(at(from.x, from.y, from.z), a);
    }
    const NavAgent3D& nav(Entity e) { return *w.try_get<NavAgent3D>(e); }
    math::vec3 pos(Entity e) { return w.try_get<LocalTransform>(e)->position; }
};

}  // namespace

TEST_CASE("nav3d: a surface bakes the scene's colliders and agents walk around the wall") {
    Scene s;
    const Entity a = s.agent({-10, 0, 0}, {10, 0, 0});
    s.run(1);
    const NavMeshSurface3D& surf = *s.w.try_get<NavMeshSurface3D>(s.surface);
    CHECK(surf.ready == 1);
    CHECK(surf.polys > 0);
    CHECK(s.w.resource<Nav3D>().mesh.loaded());
    CHECK(s.nav(a).state == NavAgentState::Moving);

    f32 max_z = -100.0f;
    for (u32 i = 0; i < 60 * 25 && s.nav(a).state != NavAgentState::Arrived; ++i) {
        s.run(1);
        const math::vec3 p = s.pos(a);
        max_z = p.z > max_z ? p.z : max_z;
        if (std::fabs(p.x) < 0.5f) CHECK(p.z > 10.0f);  // never through the wall
    }
    CHECK(s.nav(a).state == NavAgentState::Arrived);
    CHECK(s.nav(a).arrivals == 1);
    CHECK(max_z > 10.0f);
    CHECK(s.pos(a).x == doctest::Approx(10.0f).epsilon(0.05));
    // Facing: local -Z turned toward the walk (+X at the end).
    const math::vec3 fwd = math::rotate(s.w.try_get<LocalTransform>(a)->rotation, {0, 0, -1});
    CHECK(fwd.x > 0.5f);

    // A new target re-plans; clearing move_to stops.
    s.w.try_get_mut<NavAgent3D>(a)->target = {10, 0, -10};
    s.run(10);
    CHECK(s.nav(a).state == NavAgentState::Moving);
    s.w.try_get_mut<NavAgent3D>(a)->move_to = 0;
    s.run(30);
    CHECK(s.nav(a).state == NavAgentState::Idle);
    const math::vec3 stopped = s.pos(a);
    s.run(30);
    CHECK(s.pos(a).x == doctest::Approx(stopped.x).epsilon(0.01));

    // Removing the agent component drops it from the crowd.
    s.w.remove<NavAgent3D>(a);
    s.run(1);
    CHECK(s.w.resource<Nav3D>().mesh.agent_count() == 0);
}

TEST_CASE("nav3d: an agent steers a character controller") {
    Scene s;
    const Entity a = s.agent({-10, 0.9f, 0}, {10, 0, 0});
    CharacterController3D c;
    s.w.add(a, c);
    for (u32 i = 0; i < 60 * 25 && s.nav(a).state != NavAgentState::Arrived; ++i) s.run(1);
    CHECK(s.nav(a).state == NavAgentState::Arrived);
    CHECK(s.pos(a).x > 9.0f);
    CHECK(s.w.try_get<CharacterController3D>(a)->grounded);
}

TEST_CASE("nav3d: an agent follows an entity, re-planning only as it moves away") {
    Scene s(false);
    const Entity leader = s.w.spawn_with(at(8, 0, 0));
    NavAgent3D a;
    a.follow = leader;
    a.move_to = 1;
    a.follow_repath = 1.0f;
    const Entity chaser = s.w.spawn_with(at(-8, 0, 0), a);
    s.run(1);
    CHECK(s.nav(chaser).target.x == doctest::Approx(8.0f));
    s.run(60);
    CHECK(s.pos(chaser).x > -6.0f);

    // A small move updates the target the agent reports but keeps the plan; a big one re-plans.
    s.w.try_get_mut<LocalTransform>(leader)->position = {8, 0, 0.5f};
    s.run(1);
    CHECK(s.nav(chaser).target.z == doctest::Approx(0.5f));
    CHECK(s.nav(chaser).next_corner.z == doctest::Approx(0.0f).epsilon(0.05));
    s.w.try_get_mut<LocalTransform>(leader)->position = {8, 0, 8};
    for (u32 i = 0; i < 60 * 20 && s.nav(chaser).state != NavAgentState::Arrived; ++i) s.run(1);
    CHECK(s.nav(chaser).state == NavAgentState::Arrived);
    CHECK(s.pos(chaser).z == doctest::Approx(8.0f).epsilon(0.1));

    // The leader gone: the agent keeps the last target.
    s.w.destroy(leader);
    s.run(2);
    CHECK(s.nav(chaser).target.z == doctest::Approx(8.0f));
}

TEST_CASE("nav3d: area costs, links and obstacles") {
    // A costly strip: the agent goes around it.
    {
        Scene s(false);
        NavArea3D mud;
        mud.half = {2.0f, 1.0f, 13.0f};
        mud.area = 2;
        mud.cost = 10.0f;
        s.w.spawn_with(at(0, 0, -7.0f), mud);
        const Entity a = s.agent({-10, 0, -5}, {10, 0, -5});
        f32 max_z = -100.0f;
        for (u32 i = 0; i < 60 * 20 && s.nav(a).state != NavAgentState::Arrived; ++i) {
            s.run(1);
            max_z = s.pos(a).z > max_z ? s.pos(a).z : max_z;
        }
        CHECK(s.nav(a).state == NavAgentState::Arrived);
        CHECK(max_z > 5.0f);
    }
    // An obstacle closing the way around the wall: no path, until it goes.
    {
        Scene s;
        NavObstacle3D o;
        o.half = {0.6f, 1.0f, 5.5f};
        const Entity door = s.w.spawn_with(at(0, 1.0f, 15.0f), o);
        const Entity a = s.agent({-10, 0, 0}, {10, 0, 0});
        s.run(60);
        CHECK(s.nav(a).state == NavAgentState::NoPath);
        CHECK(s.w.resource<Nav3D>().mesh.obstacle_count() == 1);
        s.w.try_get_mut<NavObstacle3D>(door)->enabled = 0;
        s.w.try_get_mut<NavAgent3D>(a)->target = {10, 0, 0.5f};  // re-plan
        s.run(1);
        CHECK(s.w.resource<Nav3D>().mesh.obstacle_count() == 0);
        for (u32 i = 0; i < 60 * 25 && s.nav(a).state != NavAgentState::Arrived; ++i) s.run(1);
        CHECK(s.nav(a).state == NavAgentState::Arrived);
    }
    // A link over the wall: the short way.
    {
        Scene s;
        NavLink3D l;
        l.end = {2.6f, 0.0f, 0.0f};
        s.w.spawn_with(at(-1.3f, 0, 0), l);
        const Entity a = s.agent({-10, 0, 0}, {10, 0, 0});
        f32 max_z = -100.0f;
        for (u32 i = 0; i < 60 * 20 && s.nav(a).state != NavAgentState::Arrived; ++i) {
            s.run(1);
            max_z = s.pos(a).z > max_z ? s.pos(a).z : max_z;
        }
        CHECK(s.nav(a).state == NavAgentState::Arrived);
        CHECK(max_z < 3.0f);
        CHECK(s.w.resource<Nav3D>().mesh.stats().links == 1);
    }
}

TEST_CASE("nav3d: a crowd replays to the same state hash; a bake through the helper is byte-identical") {
    auto run = [] {
        Scene s;
        for (u32 i = 0; i < 6; ++i) s.agent({-10, 0, -8.0f + f32(i) * 1.2f}, {10, 0, -8.0f + f32(i) * 1.2f});
        s.run(60 * 10);
        return s.w.state_hash();
    };
    CHECK(run() == run());

    Scene s;
    Array<byte> a, b;
    REQUIRE(bake_scene_navmesh(s.w, s.surface, nullptr, a));
    REQUIRE(bake_scene_navmesh(s.w, s.surface, nullptr, b));
    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
    CHECK(physics::navmesh_blob_valid(a.span()));

    // No surface: nothing loaded, agents report OffMesh.
    World w;
    w.register_components<LocalTransform>();
    install_nav3d(w, nullptr);
    NavAgent3D ag;
    ag.move_to = 1;
    const Entity e = w.spawn_with(at(0, 0, 0), ag);
    w.tick(kDt, 0);
    CHECK_FALSE(w.resource<Nav3D>().mesh.loaded());
    CHECK(w.try_get<NavAgent3D>(e)->state == NavAgentState::Idle);
}
