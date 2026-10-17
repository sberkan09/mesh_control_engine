// Floating origin (M-OPENWORLD A2): rebase_world moves roots, world transforms and WorldPoint fields by whole
// cells, calls the listeners once, and leaves a world without the origin alone.

#include <doctest/doctest.h>

#include "mce/ecs/hierarchy.h"
#include "mce/ecs/origin.h"
#include "mce/refl/macros.h"
#include "test_support.h"

using namespace mce;
using vec3 = mce::math::vec3;

namespace origin_test {

struct Waypoints {
    math::vec3 target;
    math::vec3 path[3];
    math::vec3 velocity;  // a direction: not flagged, never shifted
};
MCE_COMPONENT(Waypoints, (target, WorldPoint), (path, WorldPoint), velocity)

struct Seen {
    u32 calls = 0;
    vec3 delta;
};

void count_shift(World&, const vec3& delta, void* user) {
    Seen& s = *static_cast<Seen*>(user);
    ++s.calls;
    s.delta = delta;
}

}  // namespace origin_test

using namespace origin_test;

namespace {

LocalTransform at(f32 x, f32 y, f32 z) {
    LocalTransform t;
    t.position = {x, y, z};
    return t;
}

}  // namespace

TEST_CASE("origin: a rebase moves roots, world transforms and WorldPoint fields, not children or directions") {
    World w;
    w.register_components<Waypoints>();
    ecs::install_hierarchy(w);
    const Entity root = w.spawn_with(at(1100.0f, 5.0f, -700.0f));
    const Entity child = w.spawn_with(at(1.0f, 2.0f, 3.0f));
    ecs::set_parent(w, child, root);
    Waypoints wp;
    wp.target = {1500.0f, 0.0f, 0.0f};
    wp.path[0] = {1024.0f, 0.0f, 256.0f};
    wp.path[2] = {0.0f, 0.0f, 0.0f};
    wp.velocity = {3.0f, 0.0f, 0.0f};
    w.add(root, wp);
    const Entity bare = w.spawn();  // no transform: untouched
    w.tick(1.0 / 60, 0);            // WorldTransforms exist after propagation
    const vec3 child_world = w.try_get<WorldTransform>(child)->position;
    const u32 changed_before = w.change_tick();

    Seen seen;
    on_origin_shift(w, &origin_test::count_shift, &seen);
    on_origin_shift(w, &origin_test::count_shift, &seen);  // registers once
    install_world_origin(w, 256.0f);
    const i32 cells[3] = {4, 0, -3};  // 1024 m on x, -768 m on z
    rebase_world(w, cells);

    CHECK(seen.calls == 1);
    CHECK(seen.delta.x == 1024.0f);
    CHECK(seen.delta.z == -768.0f);
    CHECK(w.try_get<LocalTransform>(root)->position.x == 76.0f);
    CHECK(w.try_get<LocalTransform>(root)->position.y == 5.0f);
    CHECK(w.try_get<LocalTransform>(root)->position.z == 68.0f);
    CHECK(w.try_get<LocalTransform>(child)->position.x == 1.0f);  // relative to its parent
    const WorldTransform* cw = w.try_get<WorldTransform>(child);
    CHECK(cw->position.x == child_world.x - 1024.0f);
    CHECK(cw->position.z == child_world.z + 768.0f);
    CHECK(cw->matrix.c[3][0] == cw->position.x);
    const Waypoints* p = w.try_get<Waypoints>(root);
    CHECK(p->target.x == 476.0f);
    CHECK(p->path[0].x == 0.0f);
    CHECK(p->path[0].z == 1024.0f);
    CHECK(p->path[2].x == -1024.0f);
    CHECK(p->velocity.x == 3.0f);
    CHECK(w.alive(bare));
    CHECK(w.change_tick() == changed_before);  // nothing stamped

    const WorldOrigin* o = world_origin(w);
    REQUIRE(o);
    CHECK(o->cell[0] == 4);
    CHECK(o->cell[2] == -3);
    CHECK(o->serial == 1);

    // The next tick propagates from the shifted roots: the child keeps its place.
    w.tick(1.0 / 60, 1);
    CHECK(w.try_get<WorldTransform>(child)->position.x == cw->position.x);

    const WorldPoint64 a = to_absolute(w, w.try_get<LocalTransform>(root)->position);
    CHECK(a.x == 1100.0);
    CHECK(a.z == -700.0);
    remove_origin_shift(w, &origin_test::count_shift, &seen);
    const i32 back[3] = {-4, 0, 3};
    rebase_world(w, back);
    CHECK(seen.calls == 1);
    CHECK(w.try_get<LocalTransform>(root)->position.x == 1100.0f);
}

TEST_CASE("origin: absolute and local round-trip far away, a world without the origin is untouched") {
    World w;
    const Entity e = w.spawn_with(at(5.0f, 0.0f, 0.0f));
    const u64 h = w.state_hash();
    CHECK(origin_offset(w).x == 0.0f);
    CHECK(to_absolute(w, {5.0f, 0.0f, 0.0f}).x == 5.0);
    const i32 none[3] = {0, 0, 0};
    rebase_world(w, none);
    CHECK(world_origin(w) == nullptr);
    CHECK(w.state_hash() == h);

    install_world_origin(w, 256.0f);
    const i32 far[3] = {390, 0, -390};  // ~100 km
    rebase_world(w, far);
    const WorldPoint64 abs{99840.25, 12.5, -99840.75};
    const vec3 local = to_local(w, abs);
    CHECK(local.x == 0.25f);
    CHECK(local.z == -0.75f);
    const WorldPoint64 back = to_absolute(w, local);
    CHECK(back.x == abs.x);
    CHECK(back.z == abs.z);
    CHECK(w.try_get<LocalTransform>(e)->position.x == 5.0f - 99840.0f);

    // shift_entities: entities that arrive in absolute coordinates are placed by the origin, no listener.
    const Entity late = w.spawn_with(at(99850.0f, 0.0f, 0.0f));
    const Entity one[] = {late};
    shift_entities(w, Span<const Entity>(one, 1), origin_offset(w));
    CHECK(w.try_get<LocalTransform>(late)->position.x == 10.0f);
}
