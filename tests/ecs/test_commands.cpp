#include <doctest/doctest.h>

#include "mce/ecs/world.h"
#include "test_support.h"

using namespace mce;

namespace {
struct Position {
    f32 x, y;
};
struct Counter {
    u32 n;
};
struct Marked {};
}  // namespace

TEST_CASE("Commands: spawned entities appear at the sync point") {
    World w;
    Entity e = w.commands().spawn(Position{1, 2});
    CHECK(w.alive(e));                         // handle valid immediately
    CHECK(w.try_get<Position>(e) == nullptr);  // components not yet
    CHECK(w.metrics().pending_commands == 1);
    w.sync(Phase::FixedUpdate_Pre);
    REQUIRE(w.try_get<Position>(e));
    CHECK(w.try_get<Position>(e)->y == 2);
}

TEST_CASE("Commands: mutations apply in recorded order") {
    World w;
    Entity e = w.spawn_with(Counter{0});
    Commands& c = w.commands();
    c.set(e, Counter{1});
    c.set(e, Counter{2});
    c.add(e, Marked{});
    c.remove<Marked>(e);
    c.set(e, Counter{3});
    w.sync(Phase::FixedUpdate_Pre);
    CHECK(w.try_get<Counter>(e)->n == 3);
    CHECK_FALSE(w.has<Marked>(e));
}

TEST_CASE("Commands: destroys run after mutations; stale targets are ignored") {
    World w;
    Entity a = w.spawn_with(Counter{0});
    Entity b = w.spawn_with(Counter{0});
    w.commands().destroy(a);
    w.commands().set(a, Counter{5});  // recorded after destroy, but destroy pass runs last
    w.commands().set(b, Counter{7});
    w.sync(Phase::FixedUpdate_Pre);
    CHECK_FALSE(w.alive(a));
    CHECK(w.try_get<Counter>(b)->n == 7);
    w.commands().set(a, Counter{9});  // stale: no effect, no crash
    w.sync(Phase::FixedUpdate_Pre);
    CHECK(w.entity_count() == 1);
}

namespace {
void storm(Frame& f) {
    if (f.tick != 1) return;
    f.commands().spawn_n(10000, Counter{42});
}
void spawn_and_check(Frame& f) {
    // A system never sees its own structural change.
    Entity e = f.commands().spawn(Position{});
    CHECK(f.world().try_get<Position>(e) == nullptr);
}
#if MCE_ASSERTS_ENABLED
void mutate_in_phase(Frame& f) {
    test::ExpectAssert expect;
    f.world().spawn();
}
#endif
}  // namespace

TEST_CASE("spawn storm: 10k spawns in one tick apply in bulk at the sync point (FR-ENT-020)") {
    World w;
    w.add_system(Phase::FixedUpdate_Pre, "storm", &storm);
    w.tick(1.0 / 60, 1);
    CHECK(w.entity_count() == 10000);
    CHECK(w.query<Counter>().count() == 10000);
    u64 sum = 0;
    w.query<const Counter&>().each([&](const Counter& c) { sum += c.n; });
    CHECK(sum == 420000);
}

TEST_CASE("systems cannot see their own structural changes; immediate structural calls assert in a phase") {
    World w;
    w.add_system(Phase::FixedUpdate_Pre, "spawn_and_check", &spawn_and_check);
#if MCE_ASSERTS_ENABLED
    w.add_system(Phase::FixedUpdate_Post, "mutate_in_phase", &mutate_in_phase);
#endif
    w.tick(1.0 / 60, 1);
    CHECK(w.query<Position>().count() == 1);
}

TEST_CASE("queries are stable while commands are recorded mid-iteration") {
    World w;
    for (int i = 0; i < 100; ++i) w.spawn_with(Counter{u32(i)});
    usize visited = 0;
    w.query<Counter>().each([&](Entity e, Counter&) {
        ++visited;
        w.commands().spawn(Counter{0});  // would invalidate a naive iterator
        w.commands().destroy(e);
    });
    CHECK(visited == 100);
    w.sync(Phase::FixedUpdate_Pre);
    CHECK(w.entity_count() == 100);
}
