// Floating origin through the kit (M-OPENWORLD A2): a world rebased in the middle of a run carries on as the same
// world: bodies keep falling and stacking, a character keeps walking, a door held to the world keeps its hinge, and
// a navmesh agent keeps its path; in absolute coordinates every pose matches a run without the rebase.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/origin.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/joints3d.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/nav3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

struct Scene {
    World w;
    u64 t = 0;
    Entity boxes[3], character, door, agent;
    Scene() {
        w.register_components<LocalTransform>();
        install_nav3d(w, nullptr);
        install_joints3d(w, nullptr);
        BoxCollider3D floor;
        floor.half = {20.0f, 0.5f, 20.0f};
        w.spawn_with(at(0, -0.5f, 0), floor);
        BoxCollider3D wall;
        wall.half = {0.5f, 1.0f, 6.0f};
        w.spawn_with(at(0, 1.0f, -8.0f), wall);
        NavMeshSurface3D s;
        s.cell_size = 0.25f;
        s.tile_size = 32;
        w.spawn_with(at(0, 0, 0), s);
        RigidBody3D b;
        b.half = {0.4f, 0.4f, 0.4f};
        for (int i = 0; i < 3; ++i) boxes[i] = w.spawn_with(at(8.0f, 1.0f + 1.0f * f32(i), 8.0f), b);
        CharacterController3D c;
        c.use_input = false;
        c.move = {2.0f, 0.0f, 0.0f};
        character = w.spawn_with(at(-15.0f, 1.0f, 12.0f), c);
        RigidBody3D panel;
        panel.half = {0.5f, 1.0f, 0.05f};
        panel.density = 200.0f;
        door = w.spawn_with(at(14.5f, 1.05f, -17.0f), panel);
        Joint3D j;
        j.kind = JointKind3D::Hinge;
        j.anchor = {-0.5f, 0, 0};
        j.axis = {0, 1, 0};
        w.add(door, j);
        NavAgent3D a;
        a.target = {10.0f, 0.0f, -8.0f};
        a.move_to = 1;
        agent = w.spawn_with(at(-10.0f, 0.0f, -8.0f), a);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    WorldPoint64 abs(Entity e) { return to_absolute(w, w.try_get<LocalTransform>(e)->position); }
};

f64 dist(const WorldPoint64& a, const WorldPoint64& b) {
    const f64 dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

TEST_CASE("origin3d: a world rebased mid-run matches the same run without the rebase") {
    Scene ref, moved;
    install_world_origin(moved.w, 256.0f);
    ref.run(30);
    moved.run(30);
    // The push that swings the door, then the rebase between two ticks.
    for (Scene* s : {&ref, &moved}) s->w.try_get_mut<RigidBody3D>(s->door)->impulse = {0, 0, -40.0f};
    ref.run(1);
    moved.run(1);
    const u32 bodies = moved.w.resource<Physics3D>().world.stats().bodies;
    const i32 cells[3] = {-4, 0, 3};  // -1024 m on x, +768 m on z
    rebase_world(moved.w, cells);
    CHECK(moved.w.try_get<LocalTransform>(moved.boxes[0])->position.x == doctest::Approx(8.0f + 1024.0f).epsilon(1e-3));
    const f32 walk_before = moved.w.try_get<CharacterController3D>(moved.character)->velocity.x;

    moved.run(1);
    // Not read as an outside move: the character keeps its walk, no body was added or rebuilt.
    CHECK(moved.w.try_get<CharacterController3D>(moved.character)->velocity.x == doctest::Approx(walk_before).epsilon(0.05));
    CHECK(moved.w.resource<Physics3D>().world.stats().bodies == bodies);
    ref.run(1);

    for (u32 i = 0; i < 600; ++i) {
        ref.run(1);
        moved.run(1);
    }
    for (int i = 0; i < 3; ++i) CHECK(dist(ref.abs(ref.boxes[i]), moved.abs(moved.boxes[i])) < 0.05);
    CHECK(dist(ref.abs(ref.character), moved.abs(moved.character)) < 0.05);
    CHECK(dist(ref.abs(ref.door), moved.abs(moved.door)) < 0.05);
    CHECK(dist(ref.abs(ref.agent), moved.abs(moved.agent)) < 0.1);
    // The door is still on its hinge (the world-held anchor moved with everything).
    const LocalTransform& t = *moved.w.try_get<LocalTransform>(moved.door);
    const WorldPoint64 edge = to_absolute(moved.w, t.position + math::rotate(t.rotation, math::vec3{-0.5f, 0, 0}));
    CHECK(dist(edge, WorldPoint64{14.0, 1.05, -17.0}) < 0.03);
    // The agent reached its target, authored before the rebase.
    CHECK(ref.w.try_get<NavAgent3D>(ref.agent)->state == NavAgentState::Arrived);
    CHECK(moved.w.try_get<NavAgent3D>(moved.agent)->state == NavAgentState::Arrived);
    CHECK(moved.abs(moved.agent).x == doctest::Approx(10.0).epsilon(0.01));
}

TEST_CASE("origin3d: two rebased runs hash alike") {
    u64 h[2] = {};
    for (u64& out : h) {
        Scene s;
        install_world_origin(s.w, 256.0f);
        s.run(20);
        const i32 cells[3] = {2, 0, -1};
        rebase_world(s.w, cells);
        s.run(120);
        out = s.w.state_hash() ^ s.w.resource<Physics3D>().world.hash_state(1);
    }
    CHECK(h[0] == h[1]);
}
