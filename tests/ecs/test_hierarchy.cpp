// ChildOf hierarchy (v0.5, FR-ENT-050): same-frame propagation, dirty subtrees only, depth order,
// re-parenting, cycles, orphans-destroy.

#include <doctest/doctest.h>

#include "mce/ecs/hierarchy.h"
#include "test_support.h"

using namespace mce;
using namespace mce::math;
using vec3 = mce::math::vec3;

namespace {

LocalTransform at(f32 x, f32 y, f32 z = 0) {
    LocalTransform t;
    t.position = {x, y, z};
    return t;
}


vec3 world_pos(World& w, Entity e) { return w.try_get<WorldTransform>(e)->position; }

}  // namespace

namespace {

// For the install-order test: the parent and child, what a FixedUpdate_Post reader saw, and what a
// FixedUpdate_Post writer moves.
struct LateInstall {
    Entity parent, child, carried, carrier;
    f32 seen_x = 0.0f;
};

void move_parent_pre(Frame& f) {
    LateInstall& s = f.resource<LateInstall>();
    LocalTransform t = *f.world().try_get<LocalTransform>(s.parent);
    t.position.x += 1.0f;
    f.world().set(s.parent, t);
}

void read_child_post(Frame& f) {
    LateInstall& s = f.resource<LateInstall>();
    if (const WorldTransform* wt = f.world().try_get<WorldTransform>(s.child)) s.seen_x = wt->position.x;
}

void move_carrier_post(Frame& f) {
    LateInstall& s = f.resource<LateInstall>();
    LocalTransform t = *f.world().try_get<LocalTransform>(s.carrier);
    t.position.y += 1.0f;
    f.world().set(s.carrier, t);
}

}  // namespace

TEST_CASE("hierarchy: installed after FixedUpdate_Post systems, they still see this tick's moves") {
    // The order a shipped game has: the kit's systems first, the hierarchy at scene load.
    World w;
    w.register_components<LocalTransform>();
    w.add_system(Phase::FixedUpdate_Pre, "move_parent", &move_parent_pre);
    w.add_system(Phase::FixedUpdate_Post, "read_child", &read_child_post);
    w.add_system(Phase::FixedUpdate_Post, "move_carrier", &move_carrier_post);
    ecs::install_hierarchy(w);
    const Span<const char* const> post = w.system_names(Phase::FixedUpdate_Post);
    REQUIRE(post.size() == 4);
    CHECK(StringView(post[0]) == "transform_propagate_early");
    CHECK(StringView(post[3]) == "transform_propagate");

    LateInstall s;
    s.parent = w.spawn_with(at(0, 0));
    s.child = w.spawn_with(at(2, 0), ChildOf{s.parent});
    s.carrier = w.spawn_with(at(0, 0));
    s.carried = w.spawn_with(at(0, 3), ChildOf{s.carrier});
    ecs::propagate_transforms(w);  // what a scene load does
    w.set_resource(s);
    for (u64 t = 0; t < 3; ++t) {
        w.tick(1.0 / 60, t);
        // The reader saw the parent's FixedUpdate_Pre move of this tick (it ran before the late run).
        CHECK(w.resource<LateInstall>().seen_x == doctest::Approx(3.0f + static_cast<f32>(t)));
        // A FixedUpdate_Post writer ahead of the late run: its child follows the same tick.
        CHECK(math::near(world_pos(w, s.carried), {0, 4.0f + static_cast<f32>(t), 0}));
    }
    // One tick's stats count both runs: the early run composes the moved parent's subtree, the
    // late one the carrier's.
    CHECK(ecs::hierarchy_stats(w).propagated == 4);
}

TEST_CASE("hierarchy: rotating a parent moves its children in the same tick") {
    World w;
    ecs::install_hierarchy(w);
    Entity root = w.spawn_with(at(10, 0));
    Entity child = w.spawn_with(at(2, 0), ChildOf{root});
    Entity grandchild = w.spawn_with(at(1, 0), ChildOf{child});
    w.tick(1.0 / 60, 0);
    REQUIRE(w.has<WorldTransform>(grandchild));
    CHECK(math::near(world_pos(w, child), {12, 0, 0}));
    CHECK(math::near(world_pos(w, grandchild), {13, 0, 0}));

    LocalTransform r = *w.try_get<LocalTransform>(root);
    r.rotation = rotation_z(kPi * 0.5f);
    w.set(root, r);
    w.tick(1.0 / 60, 1);
    CHECK(math::near(world_pos(w, child), {10, 2, 0}));
    CHECK(math::near(world_pos(w, grandchild), {10, 3, 0}));
    CHECK(ecs::hierarchy_stats(w).propagated == 3);
    CHECK(ecs::depth_of(w, grandchild) == 2);
    CHECK(ecs::parent_of(w, grandchild) == child);
    REQUIRE(ecs::children_of(w, root).size() == 1);
    // The matrix agrees with the decomposed pose.
    const WorldTransform* gw = w.try_get<WorldTransform>(grandchild);
    CHECK(math::near(transform_point(gw->matrix, {0, 0, 0}), gw->position));
}

TEST_CASE("hierarchy: dirty propagation touches exactly the changed subtrees") {
    World w;
    ecs::install_hierarchy(w);
    // 50 independent trees of 1 root + 4 children + 4x2 grandchildren = 13 nodes each.
    Array<Entity> roots, leaves;
    for (int t = 0; t < 50; ++t) {
        Entity r = w.spawn_with(at(f32(t), 0));
        roots.push_back(r);
        for (int c = 0; c < 4; ++c) {
            Entity ch = w.spawn_with(at(0, f32(c)), ChildOf{r});
            for (int g = 0; g < 2; ++g) leaves.push_back(w.spawn_with(at(0, 0, f32(g)), ChildOf{ch}));
        }
    }
    w.tick(1.0 / 60, 0);
    CHECK(ecs::hierarchy_stats(w).propagated == 50 * 13);
    w.tick(1.0 / 60, 1);
    CHECK(ecs::hierarchy_stats(w).propagated == 0);  // nothing moved

    w.set(roots[7], at(100, 0));  // one root: its 13-node subtree
    w.set(leaves[3], at(0, 0, 5));  // one leaf elsewhere
    w.set(leaves[3 + 8 * 7], at(0, 0, 9));  // a leaf inside the dirty subtree: no extra work
    w.tick(1.0 / 60, 2);
    CHECK(ecs::hierarchy_stats(w).seeds == 3);
    CHECK(ecs::hierarchy_stats(w).propagated == 13 + 1);
    CHECK(math::near(world_pos(w, leaves[3 + 8 * 7]), {100, 1, 9}));  // child 1 of tree 7, grandchild 1
}

TEST_CASE("hierarchy: re-parenting through set() and set_parent() relinks the index") {
    World w;
    ecs::install_hierarchy(w);
    Entity a = w.spawn_with(at(1, 0));
    Entity b = w.spawn_with(at(0, 5));
    Entity c = w.spawn_with(at(1, 1), ChildOf{a});
    w.tick(1.0 / 60, 0);
    CHECK(math::near(world_pos(w, c), {2, 1, 0}));

    w.set(c, ChildOf{b});  // existing component: reconciled by the propagation system
    w.tick(1.0 / 60, 1);
    CHECK(ecs::parent_of(w, c) == b);
    CHECK(ecs::children_of(w, a).empty());
    CHECK(math::near(world_pos(w, c), {1, 6, 0}));

    CHECK(ecs::set_parent(w, c, a));
    CHECK(ecs::parent_of(w, c) == a);
    CHECK(ecs::set_parent(w, c, Entity::null));
    CHECK_FALSE(w.has<ChildOf>(c));
    w.tick(1.0 / 60, 2);
    CHECK(math::near(world_pos(w, c), {1, 1, 0}));
}

TEST_CASE("hierarchy: cycles are rejected and the entity stays a root") {
    World w;
    ecs::install_hierarchy(w);
    Entity a = w.spawn_with(at(1, 0));
    Entity b = w.spawn_with(at(1, 0), ChildOf{a});
    Entity c = w.spawn_with(at(1, 0), ChildOf{b});
    CHECK_FALSE(ecs::set_parent(w, a, c));
    CHECK_FALSE(ecs::set_parent(w, a, a));
    CHECK(ecs::hierarchy_stats(w).cycles_rejected == 2);
    w.set(a, ChildOf{c});  // via a component write: rejected at reconcile
    w.tick(1.0 / 60, 0);
    CHECK(ecs::parent_of(w, a).is_null());
    CHECK(ecs::hierarchy_stats(w).cycles_rejected >= 3);
    CHECK(math::near(world_pos(w, c), {3, 0, 0}));
}

TEST_CASE("hierarchy: destroying a parent destroys its subtree (immediate and via Commands)") {
    World w;
    ecs::install_hierarchy(w);
    Entity a = w.spawn_with(at(0, 0));
    Entity b = w.spawn_with(at(0, 0), ChildOf{a});
    Entity c = w.spawn_with(at(0, 0), ChildOf{b});
    Entity other = w.spawn_with(at(0, 0));
    w.destroy(a);
    CHECK_FALSE(w.alive(b));
    CHECK_FALSE(w.alive(c));
    CHECK(w.alive(other));

    Entity p = w.spawn_with(at(0, 0));
    Entity q = w.spawn_with(at(0, 0), ChildOf{p});
    w.commands().destroy(p);
    w.sync(Phase::FixedUpdate_Pre);
    CHECK_FALSE(w.alive(q));
    CHECK(w.entity_count() == 1);
}

TEST_CASE("hierarchy: children spawned by Commands get WorldTransforms in the same tick") {
    World w;
    ecs::install_hierarchy(w);
    w.set_resource(Entity{});
    w.add_system(Phase::FixedUpdate_Pre, "spawn", [](Frame& f) {
        if (f.tick != 0) return;
        Commands& cmd = f.commands();
        const Entity parent = cmd.spawn(at(3, 0));
        f.resource<Entity>() = cmd.spawn(at(0, 4), ChildOf{parent});
    });
    w.tick(1.0 / 60, 0);
    const Entity child = w.resource<Entity>();
    REQUIRE(w.has<WorldTransform>(child));
    CHECK(math::near(world_pos(w, child), {3, 4, 0}));
}

TEST_CASE("hierarchy: propagate_transforms works outside phases and is deterministic") {
    auto build = [](World& w) {
        ecs::install_hierarchy(w);
        Entity prev = Entity::null;
        for (int i = 0; i < 20; ++i) {
            LocalTransform t = at(1, 0);
            t.rotation = rotation_z(0.1f * f32(i));
            t.scale = {1.0f, 1.0f + 0.01f * f32(i), 1.0f};
            prev = prev.is_null() ? w.spawn_with(t) : w.spawn_with(t, ChildOf{prev});
        }
        ecs::propagate_transforms(w);
        return prev;
    };
    World a, b;
    const Entity la = build(a);
    build(b);
    REQUIRE(a.has<WorldTransform>(la));
    CHECK(a.state_hash() == b.state_hash());
    ecs::propagate_transforms(a);
    CHECK(ecs::hierarchy_stats(a).propagated == 0);
}

TEST_CASE("hierarchy: moving 1 of 5,000 entities re-composes exactly that entity (FR-ENT-040/050)") {
    World w;
    ecs::install_hierarchy(w);
    Array<Entity> all;
    for (int r = 0; r < 1000; ++r) {
        Entity root = w.spawn_with(at(f32(r), 0));
        all.push_back(root);
        for (int c = 0; c < 4; ++c) all.push_back(w.spawn_with(at(0, f32(c)), ChildOf{root}));
    }
    REQUIRE(all.size() == 5000);
    w.tick(1.0 / 60, 0);
    CHECK(ecs::hierarchy_stats(w).propagated == 5000);
    w.set(all[4242], at(0, 9));  // a leaf
    w.tick(1.0 / 60, 1);
    CHECK(ecs::hierarchy_stats(w).seeds == 1);
    CHECK(ecs::hierarchy_stats(w).propagated == 1);
    // Only that entity's WorldTransform changed this tick.
    struct Count {
        u32 n = 0;
    };
    w.set_resource(Count{});
    w.add_system(Phase::Render_Pre, "count_changed_world", [](Frame& f) {
        f.resource<Count>().n = static_cast<u32>(f.query<const WorldTransform&>().changed<WorldTransform>().count());
    });
    w.render(0.0f);  // first run of the counter: everything is new to it
    w.set(all[17], at(0, 3));
    w.tick(1.0 / 60, 2);
    w.render(0.0f);
    CHECK(w.resource<Count>().n == 1);
}
