// World2D: integration, contact response, sensors, events, queries, lifecycle (physics.md §6
// tests 2, 4, 5, 11). Expected values are derived by hand from the semi-implicit Euler step.

#include <cmath>

#include "helpers.h"
#include "mce/hash.h"

using namespace phys_test;

namespace {

Entity make_ground(World2D& w, u32 index = 1) {
    const Entity g = ent(index);
    REQUIRE(add_box(w, g, BodyType::Static, {0, -0.5f}, {50, 0.5f}));  // top at y = 0
    return g;
}

}  // namespace

TEST_CASE("world: free fall follows semi-implicit Euler exactly") {
    World2D w;
    const Entity b = ent(1);
    REQUIRE(add_box(w, b, BodyType::Dynamic, {0, 10}, {0.5f, 0.5f}));
    for (int i = 0; i < 30; ++i) w.step(kDt);
    // y_n = y0 - g dt^2 n (n + 1) / 2, v_n = -g dt n
    const f32 g = 9.81f;
    CHECK(state(w, b).position.y == doctest::Approx(10.0f - g * kDt * kDt * 465.0f).epsilon(1e-4));
    CHECK(state(w, b).linear_velocity.y == doctest::Approx(-g * kDt * 30.0f).epsilon(1e-4));
}

TEST_CASE("world: gravity scale, damping, kinematic and static bodies") {
    World2D w;
    BodyDesc d;
    d.body.type = BodyType::Dynamic;
    d.body.gravity_scale = 0.0f;
    d.body.linear_damping = 1.0f;
    d.collider.shape = ColliderShape::circle(0.5f);
    REQUIRE(w.create_body(ent(1), d));
    w.set_linear_velocity(ent(1), {6, 0});
    w.step(kDt);
    CHECK(state(w, ent(1)).linear_velocity.x == doctest::Approx(6.0f / (1.0f + kDt)));
    CHECK(state(w, ent(1)).linear_velocity.y == 0.0f);

    REQUIRE(add_box(w, ent(2), BodyType::Kinematic, {0, 0}, {1, 1}));
    w.set_linear_velocity(ent(2), {0, 3});
    for (int i = 0; i < 60; ++i) w.step(kDt);
    CHECK(state(w, ent(2)).position.y == doctest::Approx(3.0f).epsilon(1e-4));  // no gravity

    REQUIRE(add_box(w, ent(3), BodyType::Static, {5, 5}, {1, 1}));
    w.set_linear_velocity(ent(3), {1, 1});
    w.step(kDt);
    CHECK(state(w, ent(3)).position == vec2{5, 5});
}

TEST_CASE("world: a dropped box comes to rest on the ground") {
    World2D w;
    make_ground(w);
    const Entity b = ent(2);
    REQUIRE(add_box(w, b, BodyType::Dynamic, {0, 3}, {0.5f, 0.5f}));
    for (int i = 0; i < 240; ++i) w.step(kDt);
    const BodyState s = state(w, b);
    CHECK(s.position.y > 0.49f);
    CHECK(s.position.y < 0.505f);
    CHECK(std::fabs(s.linear_velocity.y) < 0.05f);
    CHECK(std::fabs(s.angle) < 1e-3f);
    CHECK(w.stats().contacts == 1);
}

TEST_CASE("world: dropped circle rests; dynamic bodies collide with each other") {
    World2D w;
    make_ground(w);
    REQUIRE(add_circle(w, ent(2), BodyType::Dynamic, {0, 2}, 0.5f));
    REQUIRE(add_box(w, ent(3), BodyType::Dynamic, {3, 1}, {0.5f, 0.5f}));
    REQUIRE(add_box(w, ent(4), BodyType::Dynamic, {3, 3}, {0.5f, 0.5f}));  // lands on ent(3)
    for (int i = 0; i < 300; ++i) w.step(kDt);
    CHECK(state(w, ent(2)).position.y == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(state(w, ent(3)).position.y == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(state(w, ent(4)).position.y == doctest::Approx(1.5f).epsilon(0.02));
}

TEST_CASE("world: restitution rebound and contact-begin approach speed") {
    World2D w;
    make_ground(w);
    const Entity ball = ent(2);
    REQUIRE(add_circle(w, ball, BodyType::Dynamic, {0, 3}, 0.5f, 0.0f, 0.5f));  // mixed e = max = 0.5
    f32 v_prev = 0.0f;
    bool bounced = false;
    for (int i = 0; i < 120 && !bounced; ++i) {
        w.step(kDt);
        const f32 v = state(w, ball).linear_velocity.y;
        if (v > 0.0f) {
            // Impact step: gravity first (v_prev - g dt), then the bias restores e * |v_rel|.
            const f32 impact = -(v_prev - 9.81f * kDt);
            CHECK(v == doctest::Approx(0.5f * impact).epsilon(0.02));
            REQUIRE(w.contact_begins().size() == 1);
            CHECK(w.contact_begins()[0].approach_speed == doctest::Approx(impact).epsilon(0.01));
            bounced = true;
        }
        v_prev = v;
    }
    CHECK(bounced);
}

TEST_CASE("world: below the restitution threshold there is no bounce") {
    World2D w;
    make_ground(w);
    REQUIRE(add_circle(w, ent(2), BodyType::Dynamic, {0, 0.52f}, 0.5f, 0.0f, 1.0f));
    for (int i = 0; i < 30; ++i) {
        w.step(kDt);
        CHECK(state(w, ent(2)).linear_velocity.y <= 0.0f + 1e-3f);
    }
}

TEST_CASE("world: Coulomb friction stops a sliding box near v^2 / (2 mu g)") {
    World2D w;
    make_ground(w);
    const Entity b = ent(2);
    REQUIRE(add_box(w, b, BodyType::Dynamic, {0, 0.5f}, {0.5f, 0.5f}, 0.5f));
    // Settle, then launch.
    for (int i = 0; i < 30; ++i) w.step(kDt);
    const f32 x0 = state(w, b).position.x;
    w.set_linear_velocity(b, {5, 0});
    for (int i = 0; i < 180; ++i) w.step(kDt);
    const f32 dist = state(w, b).position.x - x0;
    // Mixed friction sqrt(0.5 * 0.6) (ground keeps the default 0.6). Each step removes mu g dt of
    // speed, so the discrete distance is v0^2 / (2 mu g) - v0 dt / 2.
    const f32 mu = std::sqrt(0.5f * 0.6f);
    const f32 expected = 25.0f / (2.0f * mu * 9.81f) - 5.0f * kDt * 0.5f;  // 2.285 m
    CHECK(dist == doctest::Approx(expected).epsilon(0.01));
    CHECK(std::fabs(state(w, b).linear_velocity.x) < 1e-3f);
    CHECK(std::fabs(state(w, b).angle) < 0.01f);

    // Frictionless: keeps (almost) all its speed.
    World2D w2;
    make_ground(w2);
    REQUIRE(add_box(w2, b, BodyType::Dynamic, {0, 0.5f}, {0.5f, 0.5f}, 0.0f));
    for (int i = 0; i < 30; ++i) w2.step(kDt);
    w2.set_linear_velocity(b, {5, 0});
    for (int i = 0; i < 60; ++i) w2.step(kDt);
    CHECK(state(w2, b).linear_velocity.x == doctest::Approx(5.0f).epsilon(1e-3));
}

TEST_CASE("world: impulses and forces") {
    World2D w;
    BodyDesc d;
    d.body.type = BodyType::Dynamic;
    d.body.gravity_scale = 0.0f;
    d.collider.shape = ColliderShape::box({0.5f, 0.5f});  // mass 1, I = 1/6
    REQUIRE(w.create_body(ent(1), d));
    w.apply_impulse(ent(1), {2, 0}, {0, 0});
    CHECK(state(w, ent(1)).linear_velocity.x == doctest::Approx(2.0f));
    CHECK(state(w, ent(1)).angular_velocity == 0.0f);
    w.apply_impulse(ent(1), {0, 1}, {0.5f, 0});  // off-centre: torque arm 0.5
    CHECK(state(w, ent(1)).angular_velocity == doctest::Approx(0.5f * 6.0f));
    w.apply_force(ent(1), {60, 0});  // dv = F/m * dt = 1
    w.step(kDt);
    CHECK(state(w, ent(1)).linear_velocity.x == doctest::Approx(3.0f));
    w.step(kDt);  // forces are cleared after a step
    CHECK(state(w, ent(1)).linear_velocity.x == doctest::Approx(3.0f));
}

TEST_CASE("world: one-way platform stops falling bodies, lets rising ones through") {
    World2D w;
    BodyDesc d;
    d.body.type = BodyType::Static;
    d.collider.shape = ColliderShape::box({2, 0.1f});
    d.collider.one_way = true;
    d.position = {0, 0};
    REQUIRE(w.create_body(ent(1), d));
    REQUIRE(add_box(w, ent(2), BodyType::Dynamic, {0, -1.5f}, {0.25f, 0.25f}));
    w.set_linear_velocity(ent(2), {0, 8});
    for (int i = 0; i < 300; ++i) w.step(kDt);
    // Went up through it and landed on top.
    CHECK(state(w, ent(2)).position.y == doctest::Approx(0.35f).epsilon(0.03));
}

TEST_CASE("world: sensors report enter/exit exactly once, and exit on destroy") {
    World2D w;
    BodyDesc s;
    s.body.type = BodyType::Static;
    s.collider.shape = ColliderShape::box({1, 1});
    s.collider.is_sensor = true;
    s.position = {5, 0};
    const Entity sensor = ent(1), mover = ent(2);
    REQUIRE(w.create_body(sensor, s));
    REQUIRE(add_box(w, mover, BodyType::Kinematic, {0, 0}, {0.25f, 0.25f}));
    w.set_linear_velocity(mover, {3, 0});
    u32 enters = 0, exits = 0;
    for (int i = 0; i < 300; ++i) {
        w.step(kDt);
        for (const TriggerEnter& e : w.trigger_enters()) {
            CHECK(e.sensor == sensor);
            CHECK(e.other == mover);
            ++enters;
        }
        for (const TriggerExit& e : w.trigger_exits()) {
            CHECK(e.sensor == sensor);
            ++exits;
        }
    }
    CHECK(enters == 1);
    CHECK(exits == 1);

    // Re-enter, then destroy the mover while inside: exactly one exit, delivered next step.
    w.teleport(mover, {5, 0}, 0.0f);
    w.set_linear_velocity(mover, {0, 0});
    w.step(kDt);
    CHECK(w.trigger_enters().size() == 1);
    w.destroy_body(mover);
    w.step(kDt);
    REQUIRE(w.trigger_exits().size() == 1);
    CHECK(w.trigger_exits()[0].other == mover);
    w.step(kDt);
    CHECK(w.trigger_exits().empty());
    CHECK(w.stats().sensor_pairs == 0);
}

TEST_CASE("world: sensors never push and are skipped by queries by default") {
    World2D w;
    make_ground(w);
    BodyDesc s;
    s.body.type = BodyType::Static;
    s.collider.shape = ColliderShape::box({1, 1});
    s.collider.is_sensor = true;
    s.position = {0, 2};
    REQUIRE(w.create_body(ent(2), s));
    REQUIRE(add_box(w, ent(3), BodyType::Dynamic, {0, 4}, {0.25f, 0.25f}));
    for (int i = 0; i < 120; ++i) w.step(kDt);
    CHECK(state(w, ent(3)).position.y == doctest::Approx(0.25f).epsilon(0.03));  // fell through it
    CHECK_FALSE(w.raycast({0, 5}, {0, -1}, 10.0f).value().entity == ent(2));
    QueryFilter f;
    f.include_sensors = true;
    CHECK(w.raycast({0, 5}, {0, -1}, 10.0f, f).value().entity == ent(2));
}

TEST_CASE("world: contact begin/end events") {
    World2D w;
    const Entity g = make_ground(w);
    REQUIRE(add_box(w, ent(2), BodyType::Dynamic, {0, 1}, {0.5f, 0.5f}));
    u32 begins = 0;
    for (int i = 0; i < 120; ++i) {
        w.step(kDt);
        for (const ContactBegin2D& c : w.contact_begins()) {
            CHECK(c.a == g);  // lower body slot first
            CHECK(c.b == ent(2));
            CHECK(c.normal.y == doctest::Approx(1.0f));
            ++begins;
        }
        CHECK(w.contact_ends().empty());
    }
    CHECK(begins == 1);
    w.destroy_body(ent(2));
    w.step(kDt);
    CHECK(w.contact_ends().size() == 1);
}

TEST_CASE("world: queries are sorted, filtered and exact") {
    World2D w;
    for (u32 i = 0; i < 3; ++i) REQUIRE(add_box(w, ent(i + 1), BodyType::Static, {2.0f + 2.0f * f32(i), 0}, {0.5f, 0.5f}));

    auto hit = w.raycast({0, 0}, {1, 0}, 10.0f);
    REQUIRE(hit);
    CHECK(hit->entity == ent(1));
    CHECK(hit->t == doctest::Approx(1.5f));
    CHECK(hit->point.x == doctest::Approx(1.5f));
    CHECK(hit->normal.x == doctest::Approx(-1.0f));
    CHECK_FALSE(w.raycast({0, 0}, {1, 0}, 1.0f));      // too short
    CHECK_FALSE(w.raycast({0, 0}, {-1, 0}, 10.0f));    // wrong way
    CHECK_FALSE(w.raycast({0, 0}, {0, 0}, 10.0f));     // degenerate direction

    Array<RayHit2D> all;
    w.raycast_all({0, 0}, {2, 0}, 10.0f, all);  // direction is normalized
    REQUIRE(all.size() == 3);
    CHECK(all[0].t == doctest::Approx(1.5f));
    CHECK(all[1].t == doctest::Approx(3.5f));
    CHECK(all[2].t == doctest::Approx(5.5f));

    const Entity skip[] = {ent(1)};
    QueryFilter f;
    f.exclude = skip;
    CHECK(w.raycast({0, 0}, {1, 0}, 10.0f, f)->entity == ent(2));

    RigidBody2D rb;
    rb.type = BodyType::Static;
    rb.layer = 3;
    REQUIRE(w.set_body_config(ent(2), rb));
    QueryFilter layers;
    layers.layer_mask = 0x1;
    layers.exclude = skip;
    CHECK(w.raycast({0, 0}, {1, 0}, 10.0f, layers)->entity == ent(3));

    QueryFilter pred;
    pred.predicate = [](Entity e, void* user) { return !(e == *static_cast<Entity*>(user)); };
    Entity veto = ent(1);
    pred.user = &veto;
    CHECK(w.raycast({0, 0}, {1, 0}, 10.0f, pred)->entity == ent(2));

    CHECK(w.overlap_point({4, 0.4f}));
    CHECK_FALSE(w.overlap_point({3, 0}));

    Array<Entity> found;
    CHECK(w.overlap_aabb({{1.6f, -1}, {4.6f, 1}}, found) == 2);
    CHECK(found[0] == ent(1));
    CHECK(found[1] == ent(2));

    auto cast = w.shape_cast(ColliderShape::circle(0.25f), {0, 0}, {10, 0});
    REQUIRE(cast);
    CHECK(cast->entity == ent(1));
    CHECK((cast->t * 10.0f) == doctest::Approx(1.25f - 0.005f).epsilon(0.002));
    CHECK(cast->normal.x == doctest::Approx(-1.0f).epsilon(1e-3));
}

TEST_CASE("world: 10k seeded queries are reproducible (FR-PHY-040)") {
    auto run = [] {
        World2D w;
        u64 s = 99;
        auto rnd = [&s] {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            return static_cast<f32>((s >> 33) % 100000) / 1000.0f;  // 0..100
        };
        for (u32 i = 1; i <= 120; ++i) {
            const vec2 p{rnd(), rnd()};
            if (i % 3) REQUIRE(add_box(w, ent(i), BodyType::Static, p, {0.2f + rnd() * 0.02f, 0.3f}, 0.6f, 0.0f, rnd()));
            else REQUIRE(add_circle(w, ent(i), BodyType::Static, p, 0.1f + rnd() * 0.01f));
        }
        Hasher64 h;
        Array<RayHit2D> all;
        Array<Entity> ents;
        for (u32 q = 0; q < 10000; ++q) {
            const vec2 o{rnd(), rnd()};
            const vec2 d{rnd() - 50.0f, rnd() - 50.0f};
            switch (q % 4) {
                case 0:
                    if (auto r = w.raycast(o, d, 30.0f)) {
                        h.u32v(r->entity.index);
                        h.f32v(r->t);
                        h.f32v(r->normal.x);
                    }
                    break;
                case 1:
                    w.raycast_all(o, d, 30.0f, all);
                    for (const RayHit2D& r : all) h.u32v(r.entity.index);
                    break;
                case 2:
                    w.overlap_aabb({o, o + vec2{5, 5}}, ents);
                    for (const Entity& e : ents) h.u32v(e.index);
                    break;
                default:
                    if (auto r = w.shape_cast(ColliderShape::box({0.3f, 0.3f}), o, o + 0.2f * d)) {
                        h.u32v(r->entity.index);
                        h.f32v(r->t);
                    }
                    break;
            }
        }
        return h.value();
    };
    CHECK(run() == run());
}

TEST_CASE("world: stale handles, malformed shapes, disabled bodies") {
    World2D w;
    make_ground(w);
    const Entity a{2, 1};
    REQUIRE(add_box(w, a, BodyType::Dynamic, {0, 2}, {0.5f, 0.5f}));
    CHECK_FALSE(add_box(w, a, BodyType::Dynamic, {0, 2}, {0.5f, 0.5f}));  // already has a body
    w.destroy_body(a);
    CHECK_FALSE(w.has_body(a));
    w.destroy_body(a);  // twice: no-op
    w.set_linear_velocity(a, {1, 1});
    w.teleport(a, {1, 1}, 0.0f);
    CHECK(w.move_and_collide(a, {1, 0}).position == vec2{});

    const Entity a2{2, 2};  // same index, new generation
    REQUIRE(add_box(w, a2, BodyType::Dynamic, {0, 2}, {0.5f, 0.5f}));
    CHECK(w.has_body(a2));
    CHECK_FALSE(w.has_body(a));
    BodyState st;
    CHECK_FALSE(w.body_state(a, st));

    BodyDesc bad;
    bad.collider.shape = ColliderShape::box({0, 0});
    CHECK_FALSE(w.create_body(ent(9), bad));
    CHECK_FALSE(w.has_body(ent(9)));
    CHECK_FALSE(w.set_collider(a2, bad.collider));  // keeps old shape
    CHECK(w.has_body(a2));

    w.set_enabled(a2, false);
    for (int i = 0; i < 30; ++i) w.step(kDt);
    CHECK(state(w, a2).position.y == doctest::Approx(2.0f));  // frozen
    CHECK_FALSE(w.raycast({0, 5}, {0, -1}, 10.0f)->entity == a2);
    w.set_enabled(a2, true);
    for (int i = 0; i < 120; ++i) w.step(kDt);
    CHECK(state(w, a2).position.y == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(w.validate_broadphase());
}

TEST_CASE("world: a pile of bodies is bit-reproducible (FR-RUN-040)") {
    auto run = [] {
        World2D w;
        make_ground(w);
        REQUIRE(add_box(w, ent(2), BodyType::Static, {-6, 2}, {0.25f, 3}));
        REQUIRE(add_box(w, ent(3), BodyType::Static, {6, 2}, {0.25f, 3}));
        u32 n = 10;
        for (u32 i = 0; i < 40; ++i) {
            const u32 col = i % 8;
            const u32 row = i / 8;
            const vec2 p{-4.0f + f32(col), 2.0f + 1.2f * f32(row)};
            if (i % 2) REQUIRE(add_box(w, ent(n++), BodyType::Dynamic, p, {0.3f, 0.3f}, 0.5f, 0.1f, 0.1f * f32(i)));
            else REQUIRE(add_circle(w, ent(n++), BodyType::Dynamic, p, 0.3f, 0.5f, 0.2f));
        }
        for (int i = 0; i < 600; ++i) w.step(kDt);
        CHECK(w.validate_broadphase());
        return w.hash_state(kFnv64Offset);
    };
    CHECK(run() == run());
}
