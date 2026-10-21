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

TEST_CASE("queries: sphere and box overlaps list every touching body in creation order") {
    World3D w;
    add_floor(w);
    REQUIRE(add_box(w, ent(3), BodyType3D::Static, {2, 3, 0}, {0.5f, 0.5f, 0.5f}));
    REQUIRE(add_box(w, ent(1), BodyType3D::Dynamic, {-2, 3, 0}, {0.5f, 0.5f, 0.5f}));
    REQUIRE(add_box(w, ent(2), BodyType3D::Kinematic, {0, 3, 4}, {0.5f, 0.5f, 0.5f}));

    Entity out[8];
    // A sphere at y = 3 reaching both side boxes' faces (1.5 m away) but not the floor or the far box.
    CHECK(w.overlap_sphere({0, 3, 0}, 1.6f, out) == 2);
    CHECK(out[0] == ent(3));  // created first
    CHECK(out[1] == ent(1));
    CHECK(w.overlap_sphere({0, 3, 0}, 1.4f, out) == 0);
        // Touching the floor too, and the count when `out` is short.
    CHECK(w.overlap_sphere({0, 2.5f, 0}, 2.6f, Span<Entity>(out, 1)) == 3);
    CHECK(out[0] == ent(1000));

    // Filters: no dynamic bodies, an excluded entity, a layer nothing lives on.
    QueryFilter3D f;
    f.include_dynamic = false;
    CHECK(w.overlap_sphere({0, 3, 0}, 1.6f, out, f) == 1);
    CHECK(out[0] == ent(3));
    const Entity skip[] = {ent(3)};
    f = {};
    f.exclude = skip;
    CHECK(w.overlap_sphere({0, 3, 0}, 1.6f, out, f) == 1);
    CHECK(out[0] == ent(1));
    f = {};
    f.layer_mask = 1u << 3;
    CHECK(w.overlap_sphere({0, 3, 0}, 1.6f, out, f) == 0);

    // A box turned 45 degrees about Y reaches further along its diagonals.
    CHECK(w.overlap_box({0, 3, 3}, {0.3f, 0.3f, 0.3f}, {}, out) == 0);
    CHECK(w.overlap_box({0, 3, 2.8f}, {0.5f, 0.3f, 0.5f}, math::quat_from_axis_angle({0, 1, 0}, math::kPi / 4.0f), out) == 1);
    CHECK(out[0] == ent(2));
    CHECK(w.overlap_box({0, 3, 0}, {0, 1, 1}, {}, out) == 0);  // a flat box is refused
}

TEST_CASE("queries: characters show in rays and overlaps only when asked") {
    World3D w;
    add_floor(w);
    ControllerParams3D p;
    p.radius = 0.4f;
    p.half_height = 0.5f;
    REQUIRE(w.create_character(ent(5), {0, 0.9f + 0.05f, 5}, p));
    REQUIRE(add_box(w, ent(6), BodyType3D::Static, {0, 1, 10}, {2, 2, 0.5f}));

    // Without include_characters the ray passes the capsule and hits the wall behind it.
    auto hit = w.raycast({0, 1, 0}, {0, 0, 1}, 50.0f);
    REQUIRE(hit);
    CHECK(hit->entity == ent(6));
    QueryFilter3D f;
    f.include_characters = true;
    hit = w.raycast({0, 1, 0}, {0, 0, 1}, 50.0f, f);
    REQUIRE(hit);
    CHECK(hit->entity == ent(5));
    CHECK(hit->t == doctest::Approx(4.6f).epsilon(1e-3));
    CHECK(math::near(hit->normal, {0, 0, -1}, 1e-3f));
    // From above: the top cap.
    hit = w.raycast({0, 5, 5}, {0, -1, 0}, 50.0f, f);
    REQUIRE(hit);
    CHECK(hit->entity == ent(5));
    CHECK(hit->point.y == doctest::Approx(0.95f + 0.5f + 0.4f).epsilon(1e-3));
    // A closer body still wins.
    REQUIRE(add_box(w, ent(7), BodyType3D::Static, {0, 1, 2}, {1, 1, 0.1f}));
    hit = w.raycast({0, 1, 0}, {0, 0, 1}, 50.0f, f);
    REQUIRE(hit);
    CHECK(hit->entity == ent(7));

    Entity out[4];
    CHECK(w.overlap_sphere({0.8f, 1, 5}, 0.5f, out) == 0);
    CHECK(w.overlap_sphere({0.8f, 1, 5}, 0.5f, out, f) == 1);
    CHECK(out[0] == ent(5));
    CHECK(w.overlap_sphere({1.0f, 1, 5}, 0.5f, out, f) == 0);
    CHECK(w.overlap_box({0, 2.0f, 5}, {0.5f, 0.2f, 0.5f}, {}, out, f) == 1);  // over the head, touching the cap
    CHECK(w.overlap_box({0, 2.1f, 5}, {0.5f, 0.2f, 0.5f}, {}, out, f) == 0);
    const Entity skip[] = {ent(5)};
    f.exclude = skip;
    CHECK(w.overlap_sphere({0.8f, 1, 5}, 0.5f, out, f) == 0);
}
