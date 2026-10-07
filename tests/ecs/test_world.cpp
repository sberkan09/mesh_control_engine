#include <doctest/doctest.h>

#include "mce/ecs/world.h"
#include "test_support.h"

using namespace mce;

namespace {
struct Position {
    f32 x, y;
};
struct Velocity {
    f32 x, y;
};
struct Health {
    f32 hp;
};
struct Frozen {};  // tag

int g_adds = 0, g_removes = 0;
}  // namespace

TEST_CASE("entities: generational handles never alias (FR-ENT-010)") {
    World w;
    Entity a = w.spawn();
    CHECK(w.alive(a));
    w.destroy(a);
    CHECK_FALSE(w.alive(a));
    Entity b = w.spawn();  // reuses a's slot
    CHECK(b.index == a.index);
    CHECK(b.generation != a.generation);
    CHECK_FALSE(w.alive(a));
    CHECK(w.try_get<Position>(a) == nullptr);
    CHECK_FALSE(w.alive(Entity::null));
    CHECK(w.entity_count() == 1);
}

TEST_CASE("components: spawn_with, set, add, remove move rows between archetypes") {
    World w;
    Entity e = w.spawn_with(Position{1, 2}, Velocity{3, 4});
    Entity f = w.spawn_with(Position{5, 6}, Velocity{7, 8});
    REQUIRE(w.try_get<Position>(e));
    CHECK(w.try_get<Position>(e)->x == 1);
    CHECK(w.try_get<Velocity>(f)->y == 8);
    CHECK_FALSE(w.has<Health>(e));

    w.add(e, Health{10});
    CHECK(w.try_get<Health>(e)->hp == 10);
    CHECK(w.try_get<Position>(e)->y == 2);  // carried along
    CHECK(w.try_get<Position>(f)->x == 5);  // f was swap-moved in the old table; still correct

    w.set(e, Health{3});
    CHECK(w.try_get<Health>(e)->hp == 3);

    w.remove<Velocity>(e);
    CHECK_FALSE(w.has<Velocity>(e));
    CHECK(w.try_get<Health>(e)->hp == 3);
    CHECK(w.try_get<Position>(e)->x == 1);

    w.add(e, Frozen{});
    CHECK(w.has<Frozen>(e));
    w.destroy(f);
    CHECK(w.try_get<Position>(e)->x == 1);
}

TEST_CASE("queries: with/without filters and deterministic order") {
    World w;
    for (int i = 0; i < 10; ++i) {
        Entity e = w.spawn_with(Position{f32(i), 0}, Velocity{1, 0});
        if (i % 2) w.add(e, Frozen{});
        if (i % 3 == 0) w.add(e, Health{f32(i)});
    }
    CHECK(w.query<Position, const Velocity&>().count() == 10);
    CHECK(w.query<Position>().with<Frozen>().count() == 5);
    CHECK(w.query<Position>().without<Frozen>().count() == 5);
    CHECK(w.query<Health>().without<Frozen>().count() == 2);  // 0, 6

    w.query<Position&, const Velocity&>().without<Frozen>().each([](Position& p, const Velocity& v) { p.x += v.x; });
    f32 sum = 0;
    w.query<const Position&>().each([&](Entity, const Position& p) { sum += p.x; });
    CHECK(sum == 45 + 5);

    // Same structure -> same visit order.
    Array<u32> order1, order2;
    w.query<Position>().each([&](Entity e, Position&) { order1.push_back(e.index); });
    w.query<Position>().each([&](Entity e, Position&) { order2.push_back(e.index); });
    REQUIRE(order1.size() == order2.size());
    for (usize i = 0; i < order1.size(); ++i) CHECK(order1[i] == order2[i]);
}

TEST_CASE("hooks fire exactly once per add/remove") {
    World w;
    g_adds = g_removes = 0;
    w.set_hooks<Health>([](World&, Entity) { ++g_adds; }, [](World&, Entity) { ++g_removes; });
    Entity e = w.spawn_with(Health{1});
    CHECK(g_adds == 1);
    w.set(e, Health{2});  // data write, not an add
    CHECK(g_adds == 1);
    w.add(e, Position{});
    w.remove<Health>(e);
    CHECK(g_removes == 1);
    w.add(e, Health{5});
    CHECK(g_adds == 2);
    w.destroy(e);
    CHECK(g_removes == 2);
}

TEST_CASE("resources are singletons, not entities") {
    World w;
    struct Gravity {
        f32 g;
    };
    CHECK(w.try_resource<Gravity>() == nullptr);
    w.set_resource(Gravity{9.8f});
    CHECK(w.resource<Gravity>().g == doctest::Approx(9.8f));
    w.set_resource(Gravity{1.6f});
    CHECK(w.resource<Gravity>().g == doctest::Approx(1.6f));
    CHECK(w.entity_count() == 0);
}

namespace {
Array<const char*>* g_order = nullptr;
void sys_a(Frame&) { g_order->push_back("a"); }
void sys_b(Frame&) { g_order->push_back("b"); }
void sys_c(Frame&) { g_order->push_back("c"); }
void sys_start(Frame&) { g_order->push_back("start"); }
}  // namespace

TEST_CASE("systems: registration order is execution order; Startup runs once") {
    World w;
    Array<const char*> order;
    g_order = &order;
    w.add_system(Phase::Startup, "start", &sys_start);
    w.add_system(Phase::FixedUpdate_Pre, "a", &sys_a);
    w.add_system(Phase::FixedUpdate_Post, "c", &sys_c);
    w.add_system(Phase::FixedUpdate_Pre, "b", &sys_b, "a");
    w.tick(1.0 / 60, 1);
    w.tick(1.0 / 60, 2);
    const char* expected[] = {"start", "a", "b", "c", "a", "b", "c"};
    REQUIRE(order.size() == 7);
    for (usize i = 0; i < 7; ++i) CHECK(StringView(order[i]) == expected[i]);
    CHECK(w.system_names(Phase::FixedUpdate_Pre).size() == 2);
    {
        test::ExpectAssert expect;
        w.add_system(Phase::FixedUpdate_Pre, "d", &sys_a, "nope");
    }
    g_order = nullptr;
}

TEST_CASE("metrics") {
    World w;
    for (int i = 0; i < 4; ++i) w.spawn_with(Position{});
    w.spawn_with(Position{}, Velocity{});
    WorldMetrics m = w.metrics();
    CHECK(m.entities == 5);
    CHECK(m.tables >= 3);
    CHECK(m.components == 2);
    CHECK(m.avg_table_occupancy_x100 == 250);
}
