// Solver hardening (M3.5; physics.md §3.2, §6 test 2; FR-PHY-020): warm starting, islands, small
// stacks, and platform carry through the solver path (dynamic bodies on kinematic platforms, and
// controllers standing on dynamic bodies).

#include <cmath>

#include "helpers.h"

using namespace phys_test;

namespace {

const Entity kGround = ent(1);

void make_ground(World2D& w) { REQUIRE(add_box(w, kGround, BodyType::Static, {0, -0.5f}, {50, 0.5f})); }

PhysicsConfig config(bool warm) {
    PhysicsConfig c;
    c.warm_starting = warm;
    return c;
}

// Three unit boxes stacked on the ground, already at their resting depth.
void make_stack(World2D& w, u32 first, f32 x, u32 n = 3) {
    for (u32 i = 0; i < n; ++i)
        REQUIRE(add_box(w, ent(first + i), BodyType::Dynamic, {x, 0.5f + static_cast<f32>(i) * 0.995f}, {0.5f, 0.5f}));
}

// Largest distance any of the stack's boxes drifts from where it stood after settling, plus the
// largest speed seen, over `ticks` steps.
struct Drift {
    f32 distance = 0.0f;
    f32 speed = 0.0f;
};
Drift measure(World2D& w, u32 first, u32 n, int settle, int ticks) {
    for (int i = 0; i < settle; ++i) w.step(kDt);
    vec2 rest[8];
    for (u32 i = 0; i < n; ++i) rest[i] = state(w, ent(first + i)).position;
    Drift d;
    for (int t = 0; t < ticks; ++t) {
        w.step(kDt);
        for (u32 i = 0; i < n; ++i) {
            const BodyState s = state(w, ent(first + i));
            d.distance = math::max(d.distance, math::length(s.position - rest[i]));
            d.speed = math::max(d.speed, math::length(s.linear_velocity));
        }
    }
    return d;
}

}  // namespace

TEST_CASE("solver: a 3-box stack stands still for 600 ticks with warm starting") {
    World2D w(config(true));
    make_ground(w);
    make_stack(w, 10, 0.0f);
    const Drift warm = measure(w, 10, 3, 120, 600);

    World2D cold(config(false));
    make_ground(cold);
    make_stack(cold, 10, 0.0f);
    const Drift no_warm = measure(cold, 10, 3, 120, 600);

    MESSAGE("warm: drift " << warm.distance << " speed " << warm.speed << "; cold: drift " << no_warm.distance
                           << " speed " << no_warm.speed);
    CHECK(warm.distance < 1e-3f);
    CHECK(warm.speed < 0.02f);
    // Warm starting is what quiets the stack: the cold solver jitters measurably more.
    CHECK(warm.speed < no_warm.speed);
    CHECK(std::fabs(state(w, ent(12)).position.y - 2.5f) < 0.02f);
}

TEST_CASE("solver: a 2-1 pyramid holds and stays bit-reproducible (FR-RUN-040)") {
    auto run = [](u64& hash) {
        World2D w;
        make_ground(w);
        REQUIRE(add_box(w, ent(10), BodyType::Dynamic, {-0.55f, 0.5f}, {0.5f, 0.5f}));
        REQUIRE(add_box(w, ent(11), BodyType::Dynamic, {0.55f, 0.5f}, {0.5f, 0.5f}));
        REQUIRE(add_box(w, ent(12), BodyType::Dynamic, {0.0f, 1.6f}, {0.5f, 0.5f}));
        for (int i = 0; i < 600; ++i) w.step(kDt);
        hash = w.hash_state(0x5eed);
        return state(w, ent(12));
    };
    u64 h1 = 0, h2 = 0;
    const BodyState top = run(h1);
    run(h2);
    CHECK(h1 == h2);
    CHECK(std::fabs(top.position.y - 1.5f) < 0.02f);
    CHECK(std::fabs(top.position.x) < 0.02f);
    CHECK(std::fabs(top.angle) < 0.01f);
}

TEST_CASE("solver: islands are contact-connected dynamic bodies; static ground does not link them") {
    World2D w;
    make_ground(w);
    make_stack(w, 10, -5.0f);                                                   // island 1
    make_stack(w, 20, 5.0f, 2);                                                 // island 2
    REQUIRE(add_box(w, ent(30), BodyType::Dynamic, {0, 20}, {0.5f, 0.5f}));     // island 3: airborne
    w.step(kDt);
    World2D::Stats s = w.stats();
    CHECK(s.islands == 3);
    CHECK(s.contacts == 3 + 2);  // ground-box + box-box per stack

    // Drop the airborne box onto the second stack: it joins that island.
    w.teleport(ent(30), {5.0f, 0.5f + 2.0f * 0.995f}, 0.0f);
    for (int i = 0; i < 5; ++i) w.step(kDt);
    s = w.stats();
    CHECK(s.islands == 2);

    // A disabled body is in no island.
    w.set_enabled(ent(30), false);
    w.step(kDt);
    CHECK(w.stats().islands == 2);
}

TEST_CASE("solver: a crate on an accelerating kinematic platform rides without slipping") {
    World2D w;
    const Entity platform = ent(3), crate = ent(4);
    REQUIRE(add_box(w, platform, BodyType::Kinematic, {0, 0}, {2, 0.25f}));
    REQUIRE(add_box(w, crate, BodyType::Dynamic, {0.5f, 0.25f + 0.5f - 0.005f}, {0.5f, 0.5f}));
    f32 offset = 0.0f;
    f32 worst = 0.0f;
    for (int t = 0; t < 600; ++t) {
        // Accelerating sideways (0.6 m/s^2, well under mu * g) and bobbing up and down.
        w.set_linear_velocity(platform, {0.6f * f32(t) * kDt, 0.5f * std::sin(0.05f * f32(t))});
        w.step(kDt);
        const f32 dx = state(w, crate).position.x - state(w, platform).position.x;
        if (t == 30) offset = dx;
        if (t > 30) worst = math::max(worst, std::fabs(dx - offset));
    }
    MESSAGE("crate slip on platform: " << worst);
    CHECK(worst < 0.01f);
    CHECK(state(w, platform).position.x > 25.0f);  // it really travelled
}

TEST_CASE("controller: standing on a crate that rides a platform carries the controller") {
    World2D w;
    const Entity platform = ent(3), crate = ent(4), player = ent(5);
    REQUIRE(add_box(w, platform, BodyType::Kinematic, {0, 0}, {3, 0.25f}));
    REQUIRE(add_box(w, crate, BodyType::Dynamic, {0, 0.25f + 0.5f - 0.005f}, {0.5f, 0.5f}));
    CharacterController2D cc;
    BodyDesc d;
    d.collider.shape = ColliderShape::box({0.25f, 0.5f});
    d.position = {0.0f, 1.8f};  // feet 0.055 m above the crate's top
    d.controller = &cc;
    REQUIRE(w.create_body(player, d));

    MoveResult2D r;
    f32 offset = 0.0f, worst = 0.0f;
    for (int t = 0; t < 300; ++t) {
        w.set_linear_velocity(platform, {1.5f, 0.0f});
        r = w.move_and_collide(player, {0, -0.1f});
        w.step(kDt);
        const f32 dx = r.position.x - state(w, crate).position.x;
        if (t == 30) {
            CHECK((r.flags & ControllerFlags::Ground) != 0);
            CHECK((r.flags & ControllerFlags::OnMovingPlatform) != 0);
            CHECK(r.ground_body == crate);
            CHECK(r.platform_velocity.x == doctest::Approx(1.5f).epsilon(0.01));
            offset = dx;
        }
        if (t > 30) worst = math::max(worst, std::fabs(dx - offset));
    }
    MESSAGE("controller slip on crate: " << worst);
    CHECK(worst < 0.01f);
    CHECK(state(w, platform).position.x == doctest::Approx(7.5f).epsilon(0.01));
}

TEST_CASE("controller: a resting crate is ground, not a moving platform") {
    World2D w;
    make_ground(w);
    const Entity crate = ent(4), player = ent(5);
    REQUIRE(add_box(w, crate, BodyType::Dynamic, {0, 0.5f - 0.005f}, {0.5f, 0.5f}));
    CharacterController2D cc;
    BodyDesc d;
    d.collider.shape = ColliderShape::box({0.25f, 0.5f});
    d.position = {0.0f, 1.6f};
    d.controller = &cc;
    REQUIRE(w.create_body(player, d));
    MoveResult2D r;
    for (int t = 0; t < 120; ++t) {
        r = w.move_and_collide(player, {0, -0.1f});
        w.step(kDt);
    }
    CHECK((r.flags & ControllerFlags::Ground) != 0);
    CHECK(r.ground_body == crate);
    CHECK((r.flags & ControllerFlags::OnMovingPlatform) == 0);
}
