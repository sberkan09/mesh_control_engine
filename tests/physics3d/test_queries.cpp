// World3D raycasts, sphere casts and the camera sweep (physics.md §6 test 7; UC-B camera gate).

#include <cmath>

#include "helpers.h"

using namespace phys3_test;

TEST_CASE("queries: raycast hits the closest surface with its normal") {
    World3D w;
    add_floor(w);
    REQUIRE(add_box(w, ent(1), BodyType3D::Static, {0, 1, 5}, {1, 1, 0.5f}));  // wall face at z = 4.5
    auto hit = w.raycast({0, 1, 0}, {0, 0, 2}, 100.0f);  // dir need not be unit
    REQUIRE(hit);
    CHECK(hit->entity == ent(1));
    CHECK(hit->t == doctest::Approx(4.5f).epsilon(1e-3));
    CHECK(math::near(hit->point, {0, 1, 4.5f}, 1e-3f));
    CHECK(math::near(hit->normal, {0, 0, -1}, 1e-3f));

    hit = w.raycast({0, 5, 0}, {0, -1, 0}, 100.0f);
    REQUIRE(hit);
    CHECK(hit->entity == ent(1000));
    CHECK(hit->t == doctest::Approx(5.0f).epsilon(1e-3));
    CHECK(math::near(hit->normal, {0, 1, 0}, 1e-3f));

    CHECK_FALSE(w.raycast({0, 1, 0}, {0, 0, 1}, 4.0f));  // too short
    CHECK_FALSE(w.raycast({0, 1, 0}, {0, 0, 0}, 4.0f));  // no direction

    const Entity skip[] = {ent(1)};
    QueryFilter3D f;
    f.exclude = skip;
    CHECK_FALSE(w.raycast({0, 1, 0}, {0, 0, 1}, 100.0f, f));
    f = {};
    f.layer_mask = 1u << 3;  // nothing lives on layer 3
    CHECK_FALSE(w.raycast({0, 5, 0}, {0, -1, 0}, 100.0f, f));
}

TEST_CASE("queries: raycast against a triangle mesh") {
    World3D w;
    const vec3 verts[] = {{-5, 0, -5}, {5, 0, -5}, {5, 0, 5}, {-5, 0, 5}, {0, 2, 0}};
    // A pyramid's four faces (no base), counter-clockwise from outside.
    const u32 tris[] = {0, 4, 1, 1, 4, 2, 2, 4, 3, 3, 4, 0};
    BodyDesc3D d;
    d.shape = Shape3D::mesh(verts, tris);
    REQUIRE(w.create_body(ent(7), d));
    const auto hit = w.raycast({0, 10, 0}, {0, -1, 0}, 20.0f);
    REQUIRE(hit);
    CHECK(hit->entity == ent(7));
    CHECK(hit->point.y == doctest::Approx(2.0f).epsilon(1e-3));
}

TEST_CASE("queries: sphere cast reports the fraction, contact and safe centre") {
    World3D w;
    REQUIRE(add_box(w, ent(1), BodyType3D::Static, {0, 0, 10}, {2, 2, 0.5f}));  // face at z = 9.5
    const auto hit = w.shape_cast(0.5f, {0, 0, 0}, {0, 0, 20});
    REQUIRE(hit);
    CHECK(hit->entity == ent(1));
    CHECK(hit->t == doctest::Approx(9.0f / 20.0f).epsilon(1e-3));
    CHECK(math::near(hit->position, {0, 0, 9.0f}, 1e-2f));
    CHECK(hit->point.z == doctest::Approx(9.5f).epsilon(1e-3));
    CHECK(math::near(hit->normal, {0, 0, -1}, 1e-3f));
    CHECK_FALSE(w.shape_cast(0.5f, {0, 0, 0}, {0, 0, 8.9f}));
    CHECK_FALSE(w.shape_cast(0.5f, {5, 0, 0}, {5, 0, 20}));  // passes beside the box
}

TEST_CASE("camera_sweep: the boom stops in front of walls, ignores props and characters") {
    World3D w;
    add_floor(w);
    REQUIRE(add_box(w, ent(1), BodyType3D::Static, {0, 2, -4}, {3, 2, 0.25f}));  // wall face z = -3.75
    REQUIRE(add_box(w, ent(2), BodyType3D::Dynamic, {0, 1.6f, -2}, {0.3f, 0.3f, 0.3f}));  // a crate on the boom
    REQUIRE(w.create_character(ent(3), {0, 1.6f, -1}));  // another character on the boom

    const vec3 pivot{0, 1.6f, 0};
    const vec3 desired{0, 1.6f, -6};
    const f32 r = 0.2f;
    const auto hit = w.camera_sweep(pivot, desired, r);
    REQUIRE(hit);
    CHECK(hit->entity == ent(1));
    CHECK(hit->position.z == doctest::Approx(-3.55f).epsilon(1e-2));
    // The camera sphere at the returned centre does not penetrate the wall.
    CHECK(hit->position.z - r >= -3.75f - 1e-3f);

    // A clear boom returns nothing: the camera goes where it was asked to.
    CHECK_FALSE(w.camera_sweep(pivot, {0, 1.6f, 3}, r));
    // Looking down into the floor stops above it.
    const auto down = w.camera_sweep({0, 3, 3}, {0, -3, 3}, r);
    REQUIRE(down);
    CHECK(down->entity == ent(1000));
    CHECK(down->position.y == doctest::Approx(0.2f).epsilon(1e-2));
}
