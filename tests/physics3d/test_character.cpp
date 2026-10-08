// The CharacterVirtual controller suite (physics.md §6 test 6; FR-PHY-080): ground, walls, steps,
// slopes, ceilings, moving platforms, pushing props.

#include <cmath>

#include "helpers.h"

using namespace phys3_test;

namespace {

const Entity kPlayer = ent(1);
// Default capsule: radius 0.3, cylinder half height 0.6 => centre stands 0.9 m above the ground.
constexpr f32 kStand = 0.9f;

// One gameplay tick: horizontal intent plus gravity integrated by the caller (gameplay owns it).
struct Walker {
    World3D& w;
    Entity e;
    f32 vy = 0.0f;
    MoveResult3D tick(vec3 horizontal_velocity, const ControllerParams3D& p = {}) {
        vy = (vy - 9.81f * kDt);
        MoveResult3D r = w.move_character(e, {horizontal_velocity.x * kDt, vy * kDt, horizontal_velocity.z * kDt}, p);
        if (has(r.flags, ControllerFlags3D::Ground) && vy < 0.0f) vy = 0.0f;
        if (r.hit_ceiling && vy > 0.0f) vy = 0.0f;
        return r;
    }
    MoveResult3D run(int ticks, vec3 v, const ControllerParams3D& p = {}) {
        MoveResult3D r;
        for (int i = 0; i < ticks; ++i) {
            r = tick(v, p);
            w.step(kDt);
        }
        return r;
    }
};

}  // namespace

TEST_CASE("character: falls onto the floor, reports Ground and stands at its half height") {
    World3D w;
    add_floor(w);
    REQUIRE(w.create_character(kPlayer, {0, 3, 0}));
    CHECK_FALSE(w.create_character(kPlayer, {0, 3, 0}));
    CHECK(w.stats().characters == 1);
    Walker walker{w, kPlayer};
    const MoveResult3D r = walker.run(120, {});
    CHECK(has(r.flags, ControllerFlags3D::Ground));
    CHECK(r.ground_body == ent(1000));
    CHECK(math::near(r.ground_normal, {0, 1, 0}, 1e-3f));
    CHECK(r.position.y == doctest::Approx(kStand).epsilon(0.02));
    MoveResult3D stored;
    REQUIRE(w.controller_state(kPlayer, stored));
    CHECK(stored.flags == r.flags);
    // Standing still does not sink.
    const MoveResult3D still = walker.run(60, {});
    CHECK(still.position.y == doctest::Approx(r.position.y).epsilon(1e-3));
    // Jumping leaves the ground.
    walker.vy = 5.0f;
    CHECK_FALSE(has(walker.tick({}).flags, ControllerFlags3D::Ground));
}

TEST_CASE("character: walls stop it without clipping, and it slides along them") {
    World3D w;
    add_floor(w);
    REQUIRE(add_box(w, ent(2), BodyType3D::Static, {0, 2, 3}, {5, 2, 0.5f}));  // wall face z = 2.5
    REQUIRE(w.create_character(kPlayer, {0, kStand, 0}));
    Walker walker{w, kPlayer};
    MoveResult3D r = walker.run(120, {0, 0, 4});
    CHECK(has(r.flags, ControllerFlags3D::Wall));
    CHECK(r.position.z < 2.5f - 0.3f + 1e-3f);  // never inside the wall
    CHECK(r.position.z > 2.5f - 0.3f - 0.05f);  // but right against it (padding)
    // Diagonal input slides along the wall: x keeps moving, z stays put.
    const f32 z = r.position.z;
    r = walker.run(60, {3, 0, 3});
    CHECK(r.position.x > 2.5f);
    CHECK(r.position.z == doctest::Approx(z).epsilon(0.01));
}

TEST_CASE("character: climbs steps up to step_height, not higher") {
    World3D w;
    add_floor(w);
    REQUIRE(add_box(w, ent(2), BodyType3D::Static, {0, 0.125f, 4}, {2, 0.125f, 2}));  // 0.25 m step, z 2..6
    REQUIRE(add_box(w, ent(3), BodyType3D::Static, {6, 0.3f, 4}, {2, 0.3f, 2}));      // 0.6 m ledge
    REQUIRE(w.create_character(kPlayer, {0, kStand, 0}));
    Walker walker{w, kPlayer};
    MoveResult3D r = walker.run(90, {0, 0, 3});
    CHECK(r.position.z > 3.0f);
    CHECK(r.position.y == doctest::Approx(0.25f + kStand).epsilon(0.02));
    CHECK(r.ground_body == ent(2));

    w.teleport(kPlayer, {6, kStand, 0}, {});
    r = walker.run(90, {0, 0, 3});
    CHECK(r.position.z < 2.0f - 0.3f + 1e-3f);  // stopped by the ledge
    CHECK(r.position.y == doctest::Approx(kStand).epsilon(0.02));
}

TEST_CASE("character: walks up gentle slopes and is held back by steep ones") {
    World3D w;
    add_floor(w);
    // Ramps rising along +z: 20 degrees (walkable) at x = 0, 65 degrees (too steep) at x = 8.
    const f32 gentle = 20.0f * 3.14159265f / 180.0f;
    const f32 steep = 65.0f * 3.14159265f / 180.0f;
    // Each ramp is a 8 m plank whose low end rests on the floor.
    REQUIRE(add_box(w, ent(2), BodyType3D::Static, {0, 4.0f * std::sin(gentle), 1 + 4.0f * std::cos(gentle)},
                    {1.5f, 0.1f, 4}, math::quat_from_axis_angle({1, 0, 0}, -gentle)));
    REQUIRE(add_box(w, ent(3), BodyType3D::Static, {8, 4.0f * std::sin(steep), 1 + 4.0f * std::cos(steep)},
                    {1.5f, 0.1f, 4}, math::quat_from_axis_angle({1, 0, 0}, -steep)));
    REQUIRE(w.create_character(kPlayer, {0, kStand, 0}));
    Walker walker{w, kPlayer};
    MoveResult3D r = walker.run(150, {0, 0, 2});
    CHECK(r.position.y > kStand + 1.0f);  // climbed well up the 20 degree ramp
    CHECK(has(r.flags, ControllerFlags3D::Ground));
    CHECK(r.ground_body == ent(2));

    w.teleport(kPlayer, {8, kStand, 0}, {});
    walker.vy = 0.0f;
    r = walker.run(150, {0, 0, 2});
    CHECK(r.position.y < kStand + 0.5f);  // cannot walk up 65 degrees
}

TEST_CASE("character: a low ceiling reports Ceiling and stops a jump") {
    World3D w;
    add_floor(w);
    REQUIRE(add_box(w, ent(2), BodyType3D::Static, {0, 2.5f, 0}, {3, 0.25f, 3}));  // underside y = 2.25
    REQUIRE(w.create_character(kPlayer, {0, kStand, 0}));
    Walker walker{w, kPlayer};
    walker.run(10, {});
    walker.vy = 8.0f;
    bool ceiling = false;
    f32 top = 0.0f;
    for (int i = 0; i < 30; ++i) {
        const MoveResult3D r = walker.tick({});
        w.step(kDt);
        ceiling = ceiling || r.hit_ceiling;
        top = math::max(top, r.position.y);
    }
    CHECK(ceiling);
    CHECK(top + kStand <= 2.25f + 1e-3f);
}

TEST_CASE("character: rides a moving platform and reports its velocity") {
    World3D w;
    add_floor(w);
    REQUIRE(add_box(w, ent(2), BodyType3D::Kinematic, {0, 1, 0}, {2, 0.1f, 2}));  // top y = 1.1
    REQUIRE(w.create_character(kPlayer, {0, 1.1f + kStand + 0.05f, 0}));
    Walker walker{w, kPlayer};
    walker.run(30, {});
    w.set_linear_velocity(ent(2), {1.5f, 0, 0});
    MoveResult3D r;
    for (int i = 0; i < 60; ++i) {
        // Platform carry is gameplay's: add the ground velocity to the intent (Jolt's documented loop).
        r = walker.tick(r.ground_velocity);
        w.step(kDt);
    }
    CHECK(has(r.flags, ControllerFlags3D::OnMovingBody));
    CHECK(r.ground_body == ent(2));
    CHECK(math::near(r.ground_velocity, {1.5f, 0, 0}, 1e-3f));
    CHECK(r.position.x == doctest::Approx(position_of(w, ent(2)).x).epsilon(0.05));
}

TEST_CASE("character: pushes dynamic props and does not overlap other characters") {
    World3D w;
    add_floor(w);
    // A light crate (12.5 kg): its floor friction (~61 N) is below the controller's 100 N push.
    BodyDesc3D crate;
    crate.type = BodyType3D::Dynamic;
    crate.shape = Shape3D::box({0.25f, 0.25f, 0.25f});
    crate.position = {0, 0.25f, 2};
    crate.density = 100.0f;
    REQUIRE(w.create_body(ent(2), crate));
    REQUIRE(w.create_character(kPlayer, {0, kStand, 0}));
    Walker walker{w, kPlayer};
    walker.run(120, {0, 0, 2});
    CHECK(position_of(w, ent(2)).z > 2.5f);

    const Entity other = ent(3);
    REQUIRE(w.create_character(other, {-3, kStand, 0}));
    w.teleport(kPlayer, {-6, kStand, 0}, {});
    Walker a{w, kPlayer};
    Walker b{w, other};
    MoveResult3D ra, rb;
    for (int i = 0; i < 120; ++i) {
        ra = a.tick({2, 0, 0});
        rb = b.tick({});
        w.step(kDt);
    }
    CHECK(rb.position.x - ra.position.x >= 2.0f * 0.3f - 1e-2f);

    w.destroy_body(other);
    CHECK(w.stats().characters == 1);
    CHECK(w.move_character(other, {1, 0, 0}).position == vec3{});  // stale: zero result
}

TEST_CASE("character: same-binary determinism of a scripted walk") {
    const auto run = [] {
        World3D w;
        add_floor(w);
        REQUIRE(add_box(w, ent(2), BodyType3D::Static, {0, 0.125f, 3}, {2, 0.125f, 1}));
        REQUIRE(add_box(w, ent(3), BodyType3D::Dynamic, {1, 0.25f, 1}, {0.25f, 0.25f, 0.25f}));
        REQUIRE(w.create_character(kPlayer, {0, kStand, 0}));
        Walker walker{w, kPlayer};
        for (int i = 0; i < 300; ++i) {
            const f32 a = static_cast<f32>(i) * 0.03f;
            if (i % 70 == 0) walker.vy = 4.0f;
            walker.tick({2.0f * std::sin(a), 0, 2.0f * std::cos(a)});
            w.step(kDt);
        }
        return w.hash_state(kFnv64Offset);
    };
    CHECK(run() == run());
}
