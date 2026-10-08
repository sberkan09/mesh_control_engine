// Character controller suite (physics.md §3.6, §6 test 3; FR-PHY-030).

#include <cmath>

#include "helpers.h"

using namespace phys_test;

namespace {

constexpr f32 kSkin = 0.005f;
const Entity kGround = ent(1);
const Entity kPlayer = ent(2);

void make_player(World2D& w, vec2 pos, CharacterController2D cc = {}) {
    BodyDesc d;
    d.body.type = BodyType::Dynamic;  // overridden: controllers are kinematic
    d.collider.shape = ColliderShape::box({0.25f, 0.5f});
    d.position = pos;
    d.controller = &cc;
    REQUIRE(w.create_body(kPlayer, d));
    CHECK(state(w, kPlayer).type == BodyType::Kinematic);
}

void make_ground(World2D& w) { REQUIRE(add_box(w, kGround, BodyType::Static, {0, -0.5f}, {50, 0.5f})); }

bool has(u32 flags, u32 f) { return (flags & f) != 0; }

}  // namespace

TEST_CASE("controller: falls onto the ground and reports Ground") {
    World2D w;
    make_ground(w);
    make_player(w, {0, 2});
    MoveResult2D r;
    for (int i = 0; i < 30; ++i) r = w.move_and_collide(kPlayer, {0, -0.1f});
    CHECK(has(r.flags, ControllerFlags::Ground));
    CHECK(r.ground_body == kGround);
    CHECK(r.ground_normal.y == doctest::Approx(1.0f));
    CHECK(r.position.y == doctest::Approx(0.5f + kSkin).epsilon(1e-3));
    // Standing still still reports ground (probe), and never sinks.
    r = w.move_and_collide(kPlayer, {0, 0});
    CHECK(has(r.flags, ControllerFlags::Ground));
    CHECK(r.position.y == doctest::Approx(0.5f + kSkin).epsilon(1e-3));
    MoveResult2D stored;
    REQUIRE(w.controller_state(kPlayer, stored));
    CHECK(stored.flags == r.flags);
    // Jumping clears it.
    r = w.move_and_collide(kPlayer, {0, 0.2f});
    CHECK_FALSE(has(r.flags, ControllerFlags::Ground));
}

TEST_CASE("controller: walls stop horizontal motion at skin distance") {
    World2D w;
    make_ground(w);
    REQUIRE(add_box(w, ent(3), BodyType::Static, {3, 5}, {0.5f, 5}));   // left face x = 2.5
    REQUIRE(add_box(w, ent(4), BodyType::Static, {-3, 5}, {0.5f, 5}));  // right face x = -2.5
    make_player(w, {0, 0.505f});
    MoveResult2D r;
    bool wall = false;
    for (int i = 0; i < 40; ++i) {
        r = w.move_and_collide(kPlayer, {0.1f, -0.1f});
        wall = wall || has(r.flags, ControllerFlags::WallRight);
        CHECK(has(r.flags, ControllerFlags::Ground));
    }
    CHECK(wall);
    CHECK(has(r.flags, ControllerFlags::WallRight));
    CHECK(r.position.x == doctest::Approx(2.5f - 0.25f - kSkin).epsilon(1e-3));
    for (int i = 0; i < 60; ++i) r = w.move_and_collide(kPlayer, {-0.1f, -0.1f});
    CHECK(has(r.flags, ControllerFlags::WallLeft));
    CHECK(r.position.x == doctest::Approx(-2.5f + 0.25f + kSkin).epsilon(1e-3));
}

TEST_CASE("controller: ceiling bump") {
    World2D w;
    make_ground(w);
    REQUIRE(add_box(w, ent(3), BodyType::Static, {0, 3}, {5, 0.5f}));  // bottom face y = 2.5
    make_player(w, {0, 0.505f});
    MoveResult2D r;
    bool ceiling = false;
    for (int i = 0; i < 20; ++i) {
        r = w.move_and_collide(kPlayer, {0, 0.2f});
        ceiling = ceiling || r.hit_ceiling;
    }
    CHECK(ceiling);
    CHECK(has(r.flags, ControllerFlags::Ceiling));
    CHECK(r.position.y + 0.5f == doctest::Approx(2.5f - kSkin).epsilon(1e-3));
}

TEST_CASE("controller: walks up a walkable slope, treats a steep one as a wall") {
    const f32 tan30 = std::tan(math::radians(30.0f));
    {
        World2D w;
        make_ground(w);
        const vec2 ramp[] = {{0, 0}, {10, 0}, {10, 10 * tan30}};
        BodyDesc d;
        d.collider.shape = ColliderShape::convex(ramp);
        REQUIRE(w.create_body(ent(3), d));
        make_player(w, {-2, 0.505f});
        MoveResult2D r;
        for (int i = 0; i < 160; ++i) r = w.move_and_collide(kPlayer, {0.05f, -0.05f});
        CHECK(r.position.x == doctest::Approx(6.0f).epsilon(0.02));
        // Bottom-right corner rests on the ramp surface.
        CHECK((r.position.y - 0.5f) == doctest::Approx((r.position.x + 0.25f) * tan30).epsilon(0.02));
        CHECK(has(r.flags, ControllerFlags::Ground));
        CHECK(r.ground_body == ent(3));
        CHECK(r.ground_normal.y == doctest::Approx(std::cos(math::radians(30.0f))).epsilon(1e-3));
        // ...and back down without leaving the ground.
        for (int i = 0; i < 40; ++i) {
            r = w.move_and_collide(kPlayer, {-0.05f, 0.0f});
            CHECK(has(r.flags, ControllerFlags::Ground));
        }
    }
    {
        World2D w;
        make_ground(w);
        const f32 tan60 = std::tan(math::radians(60.0f));
        const vec2 ramp[] = {{0, 0}, {3, 0}, {3, 3 * tan60}};
        BodyDesc d;
        d.collider.shape = ColliderShape::convex(ramp);
        REQUIRE(w.create_body(ent(3), d));
        make_player(w, {-2, 0.505f});
        MoveResult2D r;
        bool wall = false;
        for (int i = 0; i < 80; ++i) {
            r = w.move_and_collide(kPlayer, {0.05f, -0.05f});
            wall = wall || has(r.flags, ControllerFlags::WallRight);
        }
        CHECK(wall);
        CHECK(r.position.x < 0.0f);
        CHECK(r.position.y < 0.6f);
    }
}

TEST_CASE("controller: snap-down keeps ground contact on a stair descent") {
    World2D w;
    REQUIRE(add_box(w, kGround, BodyType::Static, {-10, -0.5f}, {10, 0.5f}));       // top y = 0, x < 0
    REQUIRE(add_box(w, ent(3), BodyType::Static, {10, -0.53f}, {10, 0.5f}));        // top y = -0.03, x > 0
    make_player(w, {-1, 0.505f});
    MoveResult2D r = w.move_and_collide(kPlayer, {0, -0.01f});
    REQUIRE(has(r.flags, ControllerFlags::Ground));
    for (int i = 0; i < 60; ++i) {
        r = w.move_and_collide(kPlayer, {0.05f, 0.0f});  // no gravity: only snap keeps contact
        CHECK(has(r.flags, ControllerFlags::Ground));
    }
    CHECK(r.ground_body == ent(3));
    CHECK(r.position.y == doctest::Approx(-0.03f + 0.5f + kSkin).epsilon(1e-3));
}

TEST_CASE("controller: one-way platform blocks from above only; drop-through via filter") {
    World2D w;
    make_ground(w);
    BodyDesc d;
    d.collider.shape = ColliderShape::box({2, 0.1f});
    d.collider.one_way = true;
    d.position = {0, 1.9f};  // top y = 2
    const Entity platform = ent(3);
    REQUIRE(w.create_body(platform, d));
    make_player(w, {0, 0.505f});

    MoveResult2D r;
    for (int i = 0; i < 15; ++i) {
        r = w.move_and_collide(kPlayer, {0, 0.2f});  // up through it
        CHECK_FALSE(has(r.flags, ControllerFlags::Ceiling));
    }
    CHECK(r.position.y == doctest::Approx(3.505f).epsilon(1e-3));
    for (int i = 0; i < 30; ++i) r = w.move_and_collide(kPlayer, {0, -0.1f});
    CHECK(has(r.flags, ControllerFlags::Ground));
    CHECK(r.ground_body == platform);
    CHECK(r.position.y == doctest::Approx(2.0f + 0.5f + kSkin).epsilon(1e-3));

    // Walking sideways off it is unaffected, and drop-through excludes it for a few ticks.
    const Entity skip[] = {platform};
    QueryFilter drop;
    drop.exclude = skip;
    for (int i = 0; i < 6; ++i) r = w.move_and_collide(kPlayer, {0, -0.1f}, drop);
    for (int i = 0; i < 30; ++i) r = w.move_and_collide(kPlayer, {0, -0.1f});
    CHECK(r.ground_body == kGround);
    CHECK(r.position.y == doctest::Approx(0.5f + kSkin).epsilon(1e-3));
}

TEST_CASE("controller: rides an accelerating moving platform for 600 ticks with zero net slide") {
    World2D w;
    const Entity platform = ent(3);
    REQUIRE(add_box(w, platform, BodyType::Kinematic, {0, 0}, {2, 0.1f}));  // top y = 0.1
    make_player(w, {0.5f, 0.7f});
    f32 offset = 0.0f;
    MoveResult2D r;
    for (int t = 0; t < 600; ++t) {
        // Accelerating horizontally, bobbing vertically.
        const vec2 v{0.5f + 0.01f * f32(t), 0.8f * std::sin(0.05f * f32(t))};
        w.set_linear_velocity(platform, v);
        r = w.move_and_collide(kPlayer, {0, -0.1f});  // gravity-ish intent, no walking
        if (t > 10) {
            CHECK(has(r.flags, ControllerFlags::Ground));
            CHECK(has(r.flags, ControllerFlags::OnMovingPlatform));
            CHECK(r.ground_body == platform);
            CHECK(r.platform_velocity.x == doctest::Approx(v.x));
        }
        w.step(kDt);
        if (t == 10) offset = r.position.x - state(w, platform).position.x;
        if (t > 10) {
            const BodyState p = state(w, platform);
            // Player moved by exactly what the platform moves this step.
            CHECK((r.position.x - p.position.x) == doctest::Approx(offset).epsilon(1e-4));
            CHECK((r.position.y - p.position.y) == doctest::Approx(0.1f + 0.5f + kSkin).epsilon(2e-3));
        }
    }
    CHECK(state(w, platform).position.x == doctest::Approx(35.0f).epsilon(0.01));  // it really travelled
}

TEST_CASE("controller: depenetrates after a teleport into a wall; dynamic bodies get pushed") {
    World2D w;
    make_ground(w);
    REQUIRE(add_box(w, ent(3), BodyType::Static, {3, 5}, {0.5f, 5}));
    make_player(w, {0, 0.505f});
    w.teleport(kPlayer, {2.4f, 0.505f}, 0.0f);  // 0.35 deep in the wall
    MoveResult2D r = w.move_and_collide(kPlayer, {0, -0.01f});
    CHECK(r.position.x <= 2.5f - 0.25f + 1e-4f);
    CHECK(has(r.flags, ControllerFlags::Ground));

    // Pushing a crate: the controller is kinematic, so the solver moves the crate out of its way.
    World2D w2;
    make_ground(w2);
    make_player(w2, {0, 0.505f});
    REQUIRE(add_box(w2, ent(4), BodyType::Dynamic, {0.8f, 0.3f}, {0.3f, 0.3f}, 0.2f));
    for (int i = 0; i < 30; ++i) w2.step(kDt);
    const f32 x0 = state(w2, ent(4)).position.x;
    for (int i = 0; i < 60; ++i) {
        w2.move_and_collide(kPlayer, {0.03f, -0.05f});
        w2.step(kDt);
    }
    CHECK(state(w2, ent(4)).position.x > x0 + 0.5f);
}
