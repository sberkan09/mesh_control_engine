// ECS integration: FixedUpdate_Physics import/step/export, hooks, events, determinism hash
// (physics.md §3.1, §3.3, §6 tests 8, 9, 11).

#include <doctest/doctest.h>

#include "mce/ecs/transform.h"
#include "mce/physics/module.h"

using namespace mce;
using namespace mce::physics;
using math::vec2;

namespace {

constexpr f64 kDt = 1.0 / 60.0;

LocalTransform at(vec2 p, f32 angle = 0.0f) {
    LocalTransform t;
    t.position = {p.x, p.y, 0.0f};
    t.rotation = rotation_z(angle);
    return t;
}

RigidBody2D body(BodyType type) {
    RigidBody2D rb;
    rb.type = type;
    return rb;
}

Collider2D box(vec2 half, bool sensor = false) {
    Collider2D c;
    c.shape = ColliderShape::box(half);
    c.is_sensor = sensor;
    return c;
}

struct Counters {
    u32 enters = 0;
    u32 exits = 0;
    u32 contact_begins = 0;
    Entity last_other;
};

void count_events(Frame& f) {
    Counters& c = f.resource<Counters>();
    for (const TriggerEnter& e : f.events().read<TriggerEnter>()) {
        ++c.enters;
        c.last_other = e.other;
    }
    c.exits += static_cast<u32>(f.events().read<TriggerExit>().size());
    c.contact_begins += static_cast<u32>(f.events().read<ContactBegin2D>().size());
}

void tick(World& w, u64& t, u32 n) {
    for (u32 i = 0; i < n; ++i) w.tick(kDt, ++t);
}

}  // namespace

TEST_CASE("module: bodies are built from components and poses flow back to LocalTransform") {
    World w;
    install_physics2d(w);
    w.spawn_with(body(BodyType::Static), box({20, 0.5f}), at({0, -0.5f}));
    const Entity crate = w.spawn_with(body(BodyType::Dynamic), box({0.5f, 0.5f}), at({0, 3}));
    u64 t = 0;
    tick(w, t, 1);
    World2D& pw = w.resource<World2D>();
    CHECK(pw.has_body(crate));
    CHECK(w.try_get<LocalTransform>(crate)->position.y < 3.0f);  // exported after the first step
    tick(w, t, 240);
    const LocalTransform* lt = w.try_get<LocalTransform>(crate);
    CHECK(lt->position.y == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(lt->scale.x == 1.0f);  // untouched

    // Component edits flow in place: turn the crate into a sensor-free static.
    w.set(crate, body(BodyType::Static));
    tick(w, t, 1);
    BodyState s;
    REQUIRE(pw.body_state(crate, s));
    CHECK(s.type == BodyType::Static);

    // Removing the body component frees it (hook at the sync point).
    w.commands().remove<RigidBody2D>(crate);
    tick(w, t, 1);
    CHECK_FALSE(pw.has_body(crate));
    CHECK(pw.stats().bodies == 1);
}

TEST_CASE("module: trigger events are readable in FixedUpdate_Post; destroy synthesizes exit") {
    World w;
    install_physics2d(w);
    w.set_resource(Counters{});
    w.add_system(Phase::FixedUpdate_Post, "count_events", &count_events);
    const Entity sensor = w.spawn_with(body(BodyType::Static), box({1, 1}, true), at({0, 0}));
    const Entity ball = w.spawn_with(body(BodyType::Dynamic), box({0.2f, 0.2f}), at({0, 4}));
    u64 t = 0;
    tick(w, t, 120);  // falls through the sensor (nothing below)
    Counters& c = w.resource<Counters>();
    CHECK(c.enters == 1);
    CHECK(c.exits == 1);
    CHECK(c.last_other == ball);

    // Park a kinematic body inside, then destroy the sensor entity with a command.
    const Entity parked = w.spawn_with(body(BodyType::Kinematic), box({0.2f, 0.2f}), at({0, 0}));
    tick(w, t, 2);
    CHECK(c.enters == 2);
    CHECK(c.last_other == parked);
    w.commands().destroy(sensor);
    tick(w, t, 2);
    CHECK(c.exits == 2);
    CHECK_FALSE(w.resource<World2D>().has_body(sensor));
}

namespace {
struct PlayerTag {
    u32 ticks = 0;
};
void drive_player(Frame& f) {
    World2D& pw = f.resource<World2D>();
    f.query<PlayerTag&>().each([&](Entity e, PlayerTag& p) {
        ++p.ticks;
        const f32 dx = (p.ticks / 60) % 2 == 0 ? 0.05f : -0.05f;
        pw.move_and_collide(e, {dx, -0.1f});
    });
}
}  // namespace

TEST_CASE("module: controllers get CharacterGround2D written each tick") {
    World w;
    install_physics2d(w);
    w.add_system(Phase::FixedUpdate_Pre, "drive_player", &drive_player);
    const Entity ground = w.spawn_with(body(BodyType::Static), box({20, 0.5f}), at({0, -0.5f}));
    const Entity player = w.spawn_with(body(BodyType::Kinematic), box({0.25f, 0.5f}), CharacterController2D{},
                                       PlayerTag{}, at({0, 2}));
    u64 t = 0;
    tick(w, t, 1);  // body created; CharacterGround2D added at the physics sync point
    CHECK(w.has<CharacterGround2D>(player));
    tick(w, t, 60);
    const CharacterGround2D* g = w.try_get<CharacterGround2D>(player);
    REQUIRE(g);
    CHECK((g->flags & ControllerFlags::Ground) != 0);
    CHECK(g->ground_body == ground);
    CHECK(w.try_get<LocalTransform>(player)->position.y == doctest::Approx(0.505f).epsilon(1e-3));
}

TEST_CASE("module: malformed colliders are rejected without crashing, and recover when fixed") {
    World w;
    install_physics2d(w);
    Collider2D bad;
    bad.shape = ColliderShape::convex({});
    const Entity e = w.spawn_with(body(BodyType::Dynamic), bad, at({0, 0}));
    u64 t = 0;
    tick(w, t, 3);
    CHECK_FALSE(w.resource<World2D>().has_body(e));
    w.set(e, box({0.5f, 0.5f}));
    tick(w, t, 1);
    CHECK(w.resource<World2D>().has_body(e));
}

namespace {
struct Rng {
    u64 s;
    f32 next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<f32>((s >> 33) % 10000) / 10000.0f;
    }
};
void spawner(Frame& f) {
    if (f.tick % 10 != 0 || f.tick > 400) return;
    Rng& r = f.resource<Rng>();
    const vec2 p{r.next() * 16.0f - 8.0f, 8.0f + r.next() * 4.0f};
    Collider2D c;
    c.restitution = r.next() * 0.5f;
    c.shape = r.next() < 0.5f ? ColliderShape::circle(0.2f + r.next() * 0.3f)
                              : ColliderShape::box({0.2f + r.next() * 0.3f, 0.2f + r.next() * 0.3f});
    LocalTransform lt;
    lt.position = {p.x, p.y, 0.0f};
    lt.rotation = rotation_z(r.next());
    f.commands().spawn(body(BodyType::Dynamic), c, lt);
}
void reaper(Frame& f) {
    // Destroy whatever touches the sensor strip (exercises destroy-during-contact paths).
    for (const TriggerEnter& e : f.events().read<TriggerEnter>()) f.commands().destroy(e.other);
}

u64 run_sim(u64 seed, u32 ticks) {
    World w;
    install_physics2d(w);
    w.set_resource(Rng{seed});
    w.add_system(Phase::FixedUpdate_Pre, "spawner", &spawner);
    w.add_system(Phase::FixedUpdate_Pre, "drive_player", &drive_player, "spawner");
    w.add_system(Phase::FixedUpdate_Post, "reaper", &reaper);
    w.spawn_with(body(BodyType::Static), box({10, 0.5f}), at({0, -0.5f}));
    w.spawn_with(body(BodyType::Static), box({0.5f, 6}), at({-10, 5}));
    w.spawn_with(body(BodyType::Static), box({0.5f, 6}), at({10, 5}));
    w.spawn_with(body(BodyType::Static), box({1.5f, 0.2f}, true), at({6, 0.3f}));
    w.spawn_with(body(BodyType::Kinematic), box({0.25f, 0.5f}), CharacterController2D{}, PlayerTag{}, at({0, 2}));
    for (u32 i = 1; i <= ticks; ++i) w.tick(kDt, i);
    return hash_combine(w.state_hash(), w.resource<World2D>().hash_state(kFnv64Offset));
}

}  // namespace

TEST_CASE("module: full-tick determinism hash is stable across runs (FR-RUN-040)") {
    const u64 a = run_sim(3, 900);
    const u64 b = run_sim(3, 900);
    CHECK(a == b);
    CHECK(run_sim(4, 900) != a);
}
