// The 20 hand-computed contact-manifold cases (physics.md §6 test 1, FR-PHY-010; ROADMAP risk 3's
// early-warning suite). Every row was solved on paper before it was typed in: expected normal
// (from A to B), point count, world points (the midpoint between the two surfaces, Box2D 2.x
// world-manifold convention) and separations. Points are matched order-independently because
// the reference/incident face choice may swap on ties without changing the world result.

#include <doctest/doctest.h>

#include <cmath>

#include "collision.h"

using namespace mce;
using namespace mce::physics;
using namespace mce::physics::detail;
using math::vec2;

namespace {

constexpr f32 kS2 = 0.70710678f;  // sqrt(2) / 2
constexpr f32 kDeg30 = math::kPi / 6.0f;

struct Body {
    ColliderShape shape;
    vec2 pos;
    f32 angle = 0.0f;
};

Body box(vec2 half, vec2 pos, f32 angle = 0.0f) { return {ColliderShape::box(half), pos, angle}; }
Body circle(f32 r, vec2 pos) { return {ColliderShape::circle(r), pos, 0.0f}; }
Body convex(std::initializer_list<vec2> pts, vec2 pos) {
    return {ColliderShape::convex(Span<const vec2>(pts.begin(), pts.size())), pos, 0.0f};
}

struct Case {
    const char* name;
    Body a, b;
    u32 count;
    vec2 normal;            // from A to B
    vec2 points[2];         // world contact points (midpoints)
    f32 separation;         // same for every point of the case
};

// The rotated face-face case (18): n = (-sin 30°, cos 30°), t = (cos 30°, sin 30°); B sits 0.95
// along n, so the faces overlap by 0.05 and the midpoints lie 0.475 along n at ±1 along t.
const vec2 kN30{-0.5f, 0.8660254f};
const vec2 kT30{0.8660254f, 0.5f};

const Case kCases[] = {
    // ---- box–box --------------------------------------------------------------------------------
    {"01 box-box face, full width", box({1, 1}, {0, 0}), box({1, 1}, {0, 1.9f}), 2, {0, 1}, {{-1, 0.95f}, {1, 0.95f}}, -0.1f},
    {"02 box-box face, offset clips to the overlap", box({1, 1}, {0, 0}), box({1, 1}, {1.5f, 1.9f}), 2, {0, 1},
     {{0.5f, 0.95f}, {1, 0.95f}}, -0.1f},
    {"03 box-box separated", box({1, 1}, {0, 0}), box({1, 1}, {0, 2.1f}), 0, {}, {}, 0},
    {"04 box-box side face (+x)", box({1, 1}, {0, 0}), box({1, 1}, {1.95f, 0}), 2, {1, 0},
     {{0.975f, -1}, {0.975f, 1}}, -0.05f},
    {"05 small box on a wide box", box({2, 0.5f}, {0, 0}), box({0.5f, 0.5f}, {0.3f, 0.98f}), 2, {0, 1},
     {{-0.2f, 0.49f}, {0.8f, 0.49f}}, -0.02f},
    {"06 rotated box vertex into a face", box({1, 1}, {0, 0}), box({0.5f, 0.5f}, {0, 1.0f + kS2 - 0.05f}, math::kPi / 4), 1,
     {0, 1}, {{0, 0.975f}}, -0.05f},
    {"07 rotated A vertex into B's face", box({0.5f, 0.5f}, {0, 0}, math::kPi / 4), box({1, 0.5f}, {0, kS2 + 0.5f - 0.04f}), 1,
     {0, 1}, {{0, kS2 - 0.02f}}, -0.04f},
    {"18 rotated boxes, face to face", box({1, 0.5f}, {0, 0}, kDeg30), box({1, 0.5f}, 0.95f * kN30, kDeg30), 2, kN30,
     {kT30 + 0.475f * kN30, -1.0f * kT30 + 0.475f * kN30}, -0.05f},
    {"19 tall box on the left (-x)", box({0.5f, 0.5f}, {0, 0}), box({0.5f, 2}, {-0.9f, 0}), 2, {-1, 0},
     {{-0.45f, -0.5f}, {-0.45f, 0.5f}}, -0.1f},
    {"20 boxes exactly touching", box({1, 1}, {0, 0}), box({1, 1}, {0, 2}), 2, {0, 1}, {{-1, 1}, {1, 1}}, 0.0f},
    // ---- circle–circle ----------------------------------------------------------------------------
    {"08 circles overlap on x", circle(1, {0, 0}), circle(0.5f, {1.2f, 0}), 1, {1, 0}, {{0.85f, 0}}, -0.3f},
    {"09 circles overlap on the diagonal", circle(0.5f, {0, 0}), circle(0.5f, {0.6f, 0.6f}), 1, {kS2, kS2}, {{0.3f, 0.3f}},
     0.6f * 1.41421356f - 1.0f},
    {"10 circles separated", circle(0.5f, {0, 0}), circle(0.5f, {1.01f, 0}), 0, {}, {}, 0},
    // ---- box–circle -------------------------------------------------------------------------------
    {"11 circle on a box face", box({1, 1}, {0, 0}), circle(0.5f, {0.3f, 1.4f}), 1, {0, 1}, {{0.3f, 0.95f}}, -0.1f},
    {"12 circle on a box corner", box({1, 1}, {0, 0}), circle(0.5f, {1.24f, 1.32f}), 1, {0.6f, 0.8f}, {{0.97f, 0.96f}}, -0.1f},
    {"13 circle clear of a box", box({1, 1}, {0, 0}), circle(0.5f, {0, 1.6f}), 0, {}, {}, 0},
    {"14 circle first (swapped pair)", circle(0.5f, {0, 1.4f}), box({1, 1}, {0, 0}), 1, {0, -1}, {{0, 0.95f}}, -0.1f},
    {"15 box resting on a circle", circle(1, {0, 0}), box({0.5f, 0.5f}, {0, 1.45f}), 1, {0, 1}, {{0, 0.975f}}, -0.05f},
    // ---- polygon–polygon ----------------------------------------------------------------------
    {"16 triangle apex into a box face", convex({{-1, 0}, {1, 0}, {0, 1}}, {0, 0}), box({0.5f, 0.5f}, {0, 1.45f}), 1, {0, 1},
     {{0, 0.975f}}, -0.05f},
    {"17 triangle base on a box face", box({1, 1}, {0, 0}), convex({{-0.5f, 0}, {0.5f, 0}, {0, 0.8f}}, {0.2f, 0.97f}), 2,
     {0, 1}, {{-0.3f, 0.985f}, {0.7f, 0.985f}}, -0.03f},
};

constexpr f32 kTol = 2e-4f;

bool near(vec2 a, vec2 b) { return math::near(a.x, b.x, kTol) && math::near(a.y, b.y, kTol); }

}  // namespace

TEST_CASE("manifold table: 20 hand-computed cases (FR-PHY-010)") {
    static_assert(sizeof(kCases) / sizeof(kCases[0]) == 20);
    for (const Case& k : kCases) {
        CAPTURE(k.name);
        Shape sa, sb;
        REQUIRE(build_shape(k.a.shape, sa));
        REQUIRE(build_shape(k.b.shape, sb));
        const Xf xa{k.a.pos, Rot::of(k.a.angle)};
        const Xf xb{k.b.pos, Rot::of(k.b.angle)};

        Manifold m;
        bool swapped = false;
        collide_any(sa, xa, sb, xb, m, &swapped);
        REQUIRE(m.count == k.count);
        if (k.count == 0) continue;

        WorldManifold wm;
        if (swapped) {
            world_manifold(m, xb, sb.radius, xa, sa.radius, wm);
            wm.normal = -wm.normal;
        } else {
            world_manifold(m, xa, sa.radius, xb, sb.radius, wm);
        }
        CHECK(near(wm.normal, k.normal));
        for (u32 i = 0; i < k.count; ++i) {
            CAPTURE(i);
            CHECK(math::near(wm.separations[i], k.separation, kTol));
            bool found = false;
            for (u32 j = 0; j < k.count; ++j) found = found || near(wm.points[i], k.points[j]);
            CHECK_MESSAGE(found, "point (" << wm.points[i].x << ", " << wm.points[i].y << ")");
        }
        // Feature ids are what warm starting matches on: two points of one manifold never share one.
        if (k.count == 2) CHECK(m.points[0].id != m.points[1].id);
    }
}
