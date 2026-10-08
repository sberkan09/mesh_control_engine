// World3D body lifecycle, stepping and determinism (physics.md §6 tests 10, 13; ADR-0003).

#include <cmath>

#include "helpers.h"
#include "mce/memory/allocator.h"

using namespace phys3_test;

TEST_CASE("world3d: body lifecycle, stale handles and malformed shapes") {
    World3D w;
    CHECK(w.stats().bodies == 0);
    CHECK(add_box(w, ent(1), BodyType3D::Static, {0, 0, 0}, {1, 1, 1}));
    CHECK(w.has_body(ent(1)));
    CHECK_FALSE(add_box(w, ent(1), BodyType3D::Static, {0, 0, 0}, {1, 1, 1}));  // already has one
    CHECK_FALSE(w.has_body(Entity{1, 2}));                                       // other generation
    CHECK_FALSE(add_box(w, ent(2), BodyType3D::Static, {0, 0, 0}, {0, 1, 1}));   // degenerate box
    CHECK_FALSE(w.create_body(Entity::null, BodyDesc3D{}));

    BodyDesc3D bad;
    bad.shape = Shape3D::sphere(-1.0f);
    CHECK_FALSE(w.create_body(ent(3), bad));
    const vec3 three[] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    bad.shape = Shape3D::convex(three);
    CHECK_FALSE(w.create_body(ent(3), bad));
    const u32 out_of_range[] = {0, 1, 7};
    bad.shape = Shape3D::mesh(three, out_of_range);
    CHECK_FALSE(w.create_body(ent(3), bad));
    const u32 tri[] = {0, 1, 2};
    bad.shape = Shape3D::mesh(three, tri);
    bad.type = BodyType3D::Dynamic;  // Jolt meshes are static/kinematic only
    CHECK_FALSE(w.create_body(ent(3), bad));
    bad.type = BodyType3D::Static;
    CHECK(w.create_body(ent(3), bad));
    CHECK(w.stats().bodies == 2);

    w.destroy_body(ent(1));
    CHECK_FALSE(w.has_body(ent(1)));
    w.destroy_body(ent(1));  // stale: no-op
    w.set_linear_velocity(ent(1), {1, 0, 0});
    BodyState3D s;
    CHECK_FALSE(w.body_state(ent(1), s));
    CHECK(w.stats().bodies == 1);
    // The freed slot is reused by the next body.
    CHECK(add_box(w, Entity{1, 2}, BodyType3D::Dynamic, {0, 5, 0}, {0.5f, 0.5f, 0.5f}));
    CHECK(w.body_state(Entity{1, 2}, s));
    CHECK(s.type == BodyType3D::Dynamic);
}

TEST_CASE("world3d: a dynamic box falls under gravity and comes to rest on the floor") {
    World3D w;
    add_floor(w);
    REQUIRE(add_box(w, ent(1), BodyType3D::Dynamic, {0, 3, 0}, {0.5f, 0.5f, 0.5f}));
    // Free fall for 0.25 s: Jolt integrates symplectic Euler, y = y0 - g * dt^2 * n(n+1)/2.
    for (int i = 0; i < 15; ++i) w.step(kDt);
    const f32 expected = 3.0f - 9.81f * kDt * kDt * 15.0f * 16.0f * 0.5f;
    CHECK(position_of(w, ent(1)).y == doctest::Approx(expected).epsilon(0.01));
    for (int i = 0; i < 180; ++i) w.step(kDt);
    const vec3 p = position_of(w, ent(1));
    CHECK(p.y == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(std::fabs(p.x) < 1e-3f);
    CHECK(std::fabs(p.z) < 1e-3f);
}

TEST_CASE("world3d: kinematic velocity, impulses and teleport") {
    World3D w;
    REQUIRE(add_box(w, ent(1), BodyType3D::Kinematic, {0, 0, 0}, {1, 0.1f, 1}));
    w.set_linear_velocity(ent(1), {1.2f, 0, 0});
    for (int i = 0; i < 60; ++i) w.step(kDt);
    CHECK(position_of(w, ent(1)).x == doctest::Approx(1.2f).epsilon(1e-3));
    CHECK(position_of(w, ent(1)).y == doctest::Approx(0.0f));  // kinematic: no gravity

    PhysicsConfig3D zero_g;
    zero_g.gravity = {0, 0, 0};
    World3D space(zero_g);
    REQUIRE(add_box(space, ent(2), BodyType3D::Dynamic, {0, 0, 0}, {0.5f, 0.5f, 0.5f}));
    // 1 m^3 of density 1000 = 1000 kg; a 1000 N s impulse gives 1 m/s (damping 0.05/s aside).
    space.apply_impulse(ent(2), {0, 0, 1000.0f});
    space.step(kDt);
    BodyState3D s;
    REQUIRE(space.body_state(ent(2), s));
    CHECK(s.linear_velocity.z == doctest::Approx(1.0f).epsilon(0.01));

    space.teleport(ent(2), {5, 6, 7}, math::quat_from_axis_angle({0, 1, 0}, 1.0f));
    REQUIRE(space.body_state(ent(2), s));
    CHECK(math::near(s.position, {5, 6, 7}, 1e-5f));
    CHECK(math::near(s.rotation, math::quat_from_axis_angle({0, 1, 0}, 1.0f), 1e-5f));
}

TEST_CASE("world3d: step asserts on a non-fixed dt (physics.md D7)") {
    World3D w;
    test::ExpectAssert expect;
    w.step(1.0f / 30.0f);
}

TEST_CASE("world3d: layers and masks filter collisions") {
    World3D w;
    BodyDesc3D floor;
    floor.shape = Shape3D::box({10, 0.5f, 10});
    floor.position = {0, -0.5f, 0};
    floor.layer = 1;
    REQUIRE(w.create_body(ent(1), floor));
    BodyDesc3D ghost;
    ghost.type = BodyType3D::Dynamic;
    ghost.shape = Shape3D::sphere(0.5f);
    ghost.position = {0, 2, 0};
    ghost.mask = 0xFFFF & ~(1u << 1);  // ignores layer 1: falls through the floor
    REQUIRE(w.create_body(ent(2), ghost));
    ghost.position = {3, 2, 0};
    ghost.mask = 0xFFFF;
    REQUIRE(w.create_body(ent(3), ghost));
    for (int i = 0; i < 120; ++i) w.step(kDt);
    CHECK(position_of(w, ent(2)).y < -5.0f);
    CHECK(position_of(w, ent(3)).y == doctest::Approx(0.5f).epsilon(0.02));
}

namespace {

// Rain of mixed bodies onto a floor + a ramp; returns the hash after `ticks`.
u64 run_scene(int ticks) {
    World3D w;
    add_floor(w);
    REQUIRE(add_box(w, ent(1), BodyType3D::Static, {4, 1, 0}, {3, 0.2f, 2},
                    math::quat_from_axis_angle({0, 0, 1}, 0.4f)));
    u32 next = 10;
    for (int i = 0; i < ticks; ++i) {
        if (i % 10 == 0 && next < 60) {
            BodyDesc3D d;
            d.type = BodyType3D::Dynamic;
            d.shape = (next % 3 == 0) ? Shape3D::sphere(0.3f)
                    : (next % 3 == 1) ? Shape3D::box({0.3f, 0.2f, 0.4f})
                                      : Shape3D::capsule(0.3f, 0.2f);
            d.position = {static_cast<f32>(next % 7) - 3.0f, 6.0f, static_cast<f32>(next % 5) - 2.0f};
            d.rotation = math::quat_from_axis_angle(math::normalize(vec3{1, 1, 0}), static_cast<f32>(next));
            d.restitution = 0.2f;
            REQUIRE(w.create_body(ent(next++), d));
        }
        w.step(kDt);
    }
    return w.hash_state(kFnv64Offset);
}

}  // namespace

TEST_CASE("world3d: same inputs give the same hash (same-binary determinism, test 13)") {
    const u64 a = run_scene(400);
    const u64 b = run_scene(400);
    CHECK(a == b);
    CHECK(a != run_scene(399));
}

TEST_CASE("world3d: Jolt allocations go through the MCE heap and are released with the last world") {
    const usize before = mem::heap_live_allocations();
    {
        World3D w;
        add_floor(w);
        CHECK(mem::heap_live_allocations() > before);
        World3D moved = std::move(w);
        CHECK(moved.has_body(ent(1000)));
    }
    CHECK(mem::heap_live_allocations() == before);
}
