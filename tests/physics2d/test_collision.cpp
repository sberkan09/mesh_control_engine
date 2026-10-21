// Narrowphase / geometry cases, each worked out by hand first (physics.md §6 test 1, FR-PHY-010).
// The full 20-case table is M3.5; these pin the code paths the M3 solver relies on.

#include <doctest/doctest.h>

#include "collision.h"
#include "dynamic_tree.h"

using namespace mce;
using namespace mce::physics;
using namespace mce::physics::detail;
using math::vec2;

namespace {

Shape box(vec2 half, vec2 offset = {}, f32 rot = 0.0f) {
    Shape s;
    REQUIRE(build_shape(ColliderShape::box(half, offset, rot), s));
    return s;
}
Shape circle(f32 r, vec2 offset = {}) {
    Shape s;
    REQUIRE(build_shape(ColliderShape::circle(r, offset), s));
    return s;
}
Xf at(vec2 p, f32 angle = 0.0f) { return {p, Rot::of(angle)}; }

struct Contact {
    u32 count = 0;
    WorldManifold wm;
};
Contact contact(const Shape& a, const Xf& xa, const Shape& b, const Xf& xb) {
    Manifold m;
    bool swapped = false;
    collide_any(a, xa, b, xb, m, &swapped);
    Contact c;
    c.count = m.count;
    if (m.count == 0) return c;
    if (swapped) {
        world_manifold(m, xb, b.radius, xa, a.radius, c.wm);
        c.wm.normal = -c.wm.normal;
    } else {
        world_manifold(m, xa, a.radius, xb, b.radius, c.wm);
    }
    return c;
}

constexpr f32 kEps = 1e-4f;

}  // namespace

TEST_CASE("manifold: box on box, face contact gives two clipped points") {
    // A: 2x2 at origin. B: 2x2 at (0, 1.9) -> 0.1 overlap along +y, full-width face contact.
    const Contact c = contact(box({1, 1}), at({0, 0}), box({1, 1}), at({0, 1.9f}));
    REQUIRE(c.count == 2);
    CHECK(math::near(c.wm.normal.x, 0.0f, kEps));
    CHECK(math::near(c.wm.normal.y, 1.0f, kEps));
    CHECK(math::near(c.wm.separations[0], -0.1f, kEps));
    CHECK(math::near(c.wm.separations[1], -0.1f, kEps));
    // Midpoints between the two surfaces: y = (1 + 0.9) / 2, x at the face ends.
    const f32 xs = c.wm.points[0].x + c.wm.points[1].x;
    CHECK(math::near(xs, 0.0f, kEps));
    CHECK(math::near(std::fabs(c.wm.points[0].x), 1.0f, kEps));
    CHECK(math::near(c.wm.points[0].y, 0.95f, kEps));
}

TEST_CASE("manifold: offset boxes clip to the overlap span") {
    // B shifted right by 1.5: faces overlap on x in [0.5, 1].
    const Contact c = contact(box({1, 1}), at({0, 0}), box({1, 1}), at({1.5f, 1.95f}));
    REQUIRE(c.count == 2);
    const f32 lo = math::min(c.wm.points[0].x, c.wm.points[1].x);
    const f32 hi = math::max(c.wm.points[0].x, c.wm.points[1].x);
    CHECK(math::near(lo, 0.5f, kEps));
    CHECK(math::near(hi, 1.0f, kEps));
    CHECK(math::near(c.wm.normal.y, 1.0f, kEps));
    CHECK(math::near(c.wm.separations[0], -0.05f, kEps));
}

TEST_CASE("manifold: separated boxes have no points") {
    CHECK(contact(box({1, 1}), at({0, 0}), box({1, 1}), at({0, 2.01f})).count == 0);
    CHECK(contact(box({1, 1}), at({0, 0}), box({1, 1}), at({2.5f, 0})).count == 0);
}

TEST_CASE("manifold: rotated box vertex into a face gives one point") {
    // B: 1x1 rotated 45 deg; its lowest vertex is 0.7071 below its centre. Place it 0.1 deep.
    const f32 h = 0.70710678f;
    const Contact c = contact(box({1, 1}), at({0, 0}), box({0.5f, 0.5f}), at({0, 1.0f + h - 0.1f}, math::kPi / 4));
    REQUIRE(c.count == 1);
    CHECK(math::near(c.wm.normal.y, 1.0f, kEps));
    CHECK(math::near(c.wm.separations[0], -0.1f, kEps));
    CHECK(math::near(c.wm.points[0].x, 0.0f, kEps));
}

TEST_CASE("manifold: circle vs circle") {
    // r=1 at origin, r=1 at (1.5, 0): depth 0.5, contact midway between surfaces at x=0.75.
    const Contact c = contact(circle(1), at({0, 0}), circle(1), at({1.5f, 0}));
    REQUIRE(c.count == 1);
    CHECK(math::near(c.wm.normal.x, 1.0f, kEps));
    CHECK(math::near(c.wm.separations[0], -0.5f, kEps));
    CHECK(math::near(c.wm.points[0].x, 0.75f, kEps));
    CHECK(contact(circle(1), at({0, 0}), circle(1), at({2.01f, 0})).count == 0);
}

TEST_CASE("manifold: circle on a box face and on a box corner") {
    // Face: r=0.5 at (0, 1.4) over a 2x2 box: depth 0.1, normal +y.
    Contact c = contact(box({1, 1}), at({0, 0}), circle(0.5f), at({0, 1.4f}));
    REQUIRE(c.count == 1);
    CHECK(math::near(c.wm.normal.y, 1.0f, kEps));
    CHECK(math::near(c.wm.separations[0], -0.1f, kEps));
    CHECK(math::near(c.wm.points[0].y, 0.95f, kEps));

    // Corner region: centre (1.3, 1.3) is 0.3*sqrt2 = 0.42426 from corner (1,1).
    c = contact(box({1, 1}), at({0, 0}), circle(0.5f), at({1.3f, 1.3f}));
    REQUIRE(c.count == 1);
    CHECK(math::near(c.wm.normal.x, 0.70710678f, kEps));
    CHECK(math::near(c.wm.normal.y, 0.70710678f, kEps));
    CHECK(math::near(c.wm.separations[0], 0.42426407f - 0.5f, kEps));

    // Same pair given circle-first: normal is reported from the circle toward the box.
    c = contact(circle(0.5f), at({0, 1.4f}), box({1, 1}), at({0, 0}));
    REQUIRE(c.count == 1);
    CHECK(math::near(c.wm.normal.y, -1.0f, kEps));

    // Corner region but out of reach.
    CHECK(contact(box({1, 1}), at({0, 0}), circle(0.4f), at({1.3f, 1.3f})).count == 0);
}

TEST_CASE("shapes: convex hull welds, orders CCW, rejects degenerate input") {
    const vec2 pts[] = {{1, 1}, {-1, -1}, {1, -1}, {0, 0}, {-1, 1}, {1, 1}};  // dup + interior point
    Shape s;
    REQUIRE(build_shape(ColliderShape::convex(pts), s));
    CHECK(s.count == 4);
    for (u32 i = 0; i < s.count; ++i) {
        const vec2 e = s.v[(i + 1) % s.count] - s.v[i];
        const vec2 e2 = s.v[(i + 2) % s.count] - s.v[(i + 1) % s.count];
        CHECK(cross(e, e2) > 0.0f);  // CCW
    }
    const vec2 line[] = {{0, 0}, {1, 0}, {2, 0}};
    CHECK_FALSE(build_shape(ColliderShape::convex(line), s));
    const vec2 two[] = {{0, 0}, {1, 0}};
    CHECK_FALSE(build_shape(ColliderShape::convex(two), s));
    CHECK_FALSE(build_shape(ColliderShape::box({0, 1}), s));
    CHECK_FALSE(build_shape(ColliderShape::circle(-1.0f), s));
    ColliderShape bad = ColliderShape::box({1, 1});
    bad.half.x = std::nanf("");
    CHECK_FALSE(build_shape(bad, s));
}

TEST_CASE("shapes: mass properties") {
    // 2x1 box, density 2: mass 4, I_centre = m (w^2 + h^2) / 12 = 4 * 5 / 12.
    MassData md = compute_mass(box({1, 0.5f}), 2.0f);
    CHECK(math::near(md.mass, 4.0f, kEps));
    CHECK(math::near(md.inertia, 4.0f * 5.0f / 12.0f, kEps));
    // Offset box: parallel-axis term about the body origin.
    md = compute_mass(box({1, 0.5f}, {3, 0}), 2.0f);
    CHECK(math::near(md.center.x, 3.0f, kEps));
    CHECK(math::near(md.inertia, 4.0f * 5.0f / 12.0f + 4.0f * 9.0f, 1e-3f));
    // Circle r=1, density 1: m = pi, I = m r^2 / 2.
    md = compute_mass(circle(1.0f), 1.0f);
    CHECK(math::near(md.mass, math::kPi, kEps));
    CHECK(math::near(md.inertia, math::kPi * 0.5f, kEps));
}

TEST_CASE("distance: GJK between separated and overlapping shapes") {
    DistanceResult d = distance(box({1, 1}), at({0, 0}), box({1, 1}), at({4, 0}));
    CHECK(math::near(d.distance, 2.0f, kEps));
    CHECK(math::near(d.point_a.x, 1.0f, kEps));
    CHECK(math::near(d.point_b.x, 3.0f, kEps));
    // Corner to corner: (1,1) to (2,2).
    d = distance(box({1, 1}), at({0, 0}), box({1, 1}), at({3, 3}));
    CHECK(math::near(d.distance, std::sqrt(2.0f), kEps));
    // Circle radii are applied.
    d = distance(circle(0.5f), at({0, 0}), box({1, 1}), at({3, 0}));
    CHECK(math::near(d.distance, 1.5f, kEps));
    d = distance(box({1, 1}), at({0, 0}), box({1, 1}), at({1, 0}));
    CHECK(d.distance == 0.0f);
}

TEST_CASE("raycast: box and circle, including start-inside misses") {
    f32 frac = 0.0f;
    vec2 n;
    REQUIRE(raycast(box({1, 1}), at({0, 0}), {-5, 0}, {5, 0}, frac, n));
    CHECK(math::near(frac, 0.4f, kEps));  // hits x=-1 at 4/10
    CHECK(math::near(n.x, -1.0f, kEps));
    REQUIRE(raycast(circle(1), at({0, 3}), {0, 0}, {0, 10}, frac, n));
    CHECK(math::near(frac, 0.2f, kEps));  // hits y=2
    CHECK(math::near(n.y, -1.0f, kEps));
    CHECK_FALSE(raycast(box({1, 1}), at({0, 0}), {0, 0}, {5, 0}, frac, n));   // starts inside
    CHECK_FALSE(raycast(box({1, 1}), at({0, 0}), {-5, 3}, {5, 3}, frac, n));  // passes above
    // Rotated box: 45 deg square of half 1 has its left vertex at x = -sqrt2.
    REQUIRE(raycast(box({1, 1}), at({0, 0}, math::kPi / 4), {-5, 0}, {5, 0}, frac, n));
    CHECK(math::near(frac * 10.0f - 5.0f, -std::sqrt(2.0f), 1e-3f));
}

TEST_CASE("shape cast: conservative advancement stops at the target separation") {
    // 1x1 box at y=5 sweeping down 10 onto a box whose top is y=0: lands with bottom at y=target.
    const Shape mover = box({0.5f, 0.5f});
    const Shape ground = box({5, 1});
    CastResult r = shape_cast(mover, at({0, 5}), {0, -10}, ground, at({0, -1}), 0.01f);
    REQUIRE(r.kind == CastResult::Kind::Hit);
    const f32 bottom = 5.0f - 10.0f * r.t - 0.5f;
    CHECK(bottom == doctest::Approx(0.01f).epsilon(0.002));
    CHECK(math::near(r.normal.y, 1.0f, kEps));

    // Moving away or parallel: no hit, even when already within the target distance.
    r = shape_cast(mover, at({0, 0.505f}), {0, 1}, ground, at({0, -1}), 0.01f);
    CHECK(r.kind == CastResult::Kind::Miss);
    r = shape_cast(mover, at({0, 0.505f}), {3, 0}, ground, at({0, -1}), 0.01f);
    CHECK(r.kind == CastResult::Kind::Miss);
    // Overlapping at the start is reported as such.
    r = shape_cast(mover, at({0, 0}), {0, -1}, ground, at({0, -1}), 0.01f);
    CHECK(r.kind == CastResult::Kind::Overlap);
    // Circle sweeping sideways into a wall.
    r = shape_cast(circle(0.5f), at({0, 0}), {10, 0}, box({1, 5}), at({5, 0}), 0.0f);
    REQUIRE(r.kind == CastResult::Kind::Hit);
    CHECK((10.0f * r.t) == doctest::Approx(3.5f).epsilon(0.002));
    CHECK(math::near(r.normal.x, -1.0f, 1e-3f));
}

TEST_CASE("dynamic tree: insert/move/remove keeps a valid balanced tree; queries are exact") {
    DynamicTree t;
    i32 ids[200];
    u64 s = 12345;
    auto rnd = [&s] {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<f32>((s >> 33) % 10000) / 100.0f;
    };
    for (u32 i = 0; i < 200; ++i) {
        const vec2 p{rnd(), rnd()};
        ids[i] = t.create_proxy({p, p + vec2{1, 1}}, i);
    }
    CHECK(t.validate());
    CHECK(t.proxy_count() == 200);
    CHECK(t.height() < 20);
    for (u32 round = 0; round < 5; ++round)
        for (u32 i = 0; i < 200; i += 3) {
            const vec2 p{rnd(), rnd()};
            t.move_proxy(ids[i], {p, p + vec2{1, 1}});
        }
    CHECK(t.validate());
    for (u32 i = 0; i < 200; i += 2) t.destroy_proxy(ids[i]);
    CHECK(t.validate());
    CHECK(t.proxy_count() == 100);

    // Query equals brute force.
    const math::aabb2 q{{20, 20}, {60, 60}};
    u32 hits = 0, brute = 0;
    t.query(q, [&](u32) {
        ++hits;
        return true;
    });
    for (u32 i = 1; i < 200; i += 2)
        if (t.fat_aabb(ids[i]).overlaps(q)) ++brute;
    CHECK(hits == brute);
}
