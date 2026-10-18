// M-DEBUGVIEW 1: gameplay_debug_lines turns 3D gameplay components into world-space segments. Exact counts and
// endpoints per category, the category mask, the lit trigger and skidding wheel colours, and that reading the
// lines leaves the World's state untouched.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/debug_lines.h"
#include "mce/kit/joints3d.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/vehicles3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

u32 count(const Array<GameplayLine>& l, u32 category) {
    u32 n = 0;
    for (u32 i = 0; i < l.size(); ++i) n += l[i].category == category ? 1u : 0u;
    return n;
}

struct Scene {
    World w;
    Scene() {
        w.register_components<LocalTransform>();
        install_bodies3d(w, nullptr);
        install_vehicles3d(w, nullptr);
    }
};

}  // namespace

TEST_CASE("debug_lines: a box collider is twelve edges at its corners") {
    Scene s;
    BoxCollider3D b;
    b.half = {1.0f, 2.0f, 3.0f};
    s.w.spawn_with(at(10, 0, 0), b);
    Array<GameplayLine> l;
    gameplay_debug_lines(s.w, kDebugColliders, l);
    REQUIRE(l.size() == 12u);
    f32 min_x = 1e9f, max_x = -1e9f, max_y = -1e9f;
    for (u32 i = 0; i < l.size(); ++i) {
        for (const math::vec3 v : {l[i].a, l[i].b}) {
            min_x = std::fmin(min_x, v.x);
            max_x = std::fmax(max_x, v.x);
            max_y = std::fmax(max_y, v.y);
        }
        CHECK(l[i].category == kDebugColliders);
    }
    CHECK(min_x == doctest::Approx(9.0f));
    CHECK(max_x == doctest::Approx(11.0f));
    CHECK(max_y == doctest::Approx(2.0f));
}

TEST_CASE("debug_lines: the mask picks categories and shapes have fixed segment counts") {
    Scene s;
    RigidBody3D box;
    s.w.spawn_with(at(0, 1, 0), box);
    RigidBody3D ball;
    ball.shape = BodyShape3D::Sphere;
    s.w.spawn_with(at(3, 1, 0), ball);
    RigidBody3D cap;
    cap.shape = BodyShape3D::Capsule;
    s.w.spawn_with(at(6, 1, 0), cap);
    CharacterController3D c;
    s.w.spawn_with(at(9, 1, 0), c);
    Array<GameplayLine> l;
    gameplay_debug_lines(s.w, kDebugBodies, l);
    CHECK(l.size() == 12u + 36u + 30u);  // box, three circles, capsule: two circles, four sides, two cap spikes
    CHECK(count(l, kDebugCharacters) == 0u);
    gameplay_debug_lines(s.w, kDebugCharacters, l);
    CHECK(l.size() == 30u);
    // Airborne (not grounded) capsules are orange, grounded yellow.
    CHECK(l[0].color.g == doctest::Approx(0.7f));
    gameplay_debug_lines(s.w, 0, l);
    CHECK(l.size() == 0u);
    CHECK(gameplay_category_bit("triggers") == kDebugTriggers);
    CHECK(gameplay_category_bit("nope") == 0u);
}

TEST_CASE("debug_lines: a trigger lights up while something is inside") {
    Scene s;
    Trigger3D t;
    const Entity e = s.w.spawn_with(at(0, 0, 0), t);
    Array<GameplayLine> l;
    gameplay_debug_lines(s.w, kDebugTriggers, l);
    REQUIRE(l.size() == 12u);
    CHECK(l[0].color.g == doctest::Approx(0.4f));
    s.w.try_get_mut<Trigger3D>(e)->inside = 1;
    gameplay_debug_lines(s.w, kDebugTriggers, l);
    CHECK(l[0].color.g == doctest::Approx(1.0f));
}

TEST_CASE("debug_lines: joints, vehicles and waypoint paths") {
    Scene s;
    Joint3D j;
    j.anchor = {0.0f, 0.5f, 0.0f};
    s.w.spawn_with(at(0, 0, 0), j);
    Vehicle3D v;
    v.skidding = 0b0100;  // rear left
    s.w.spawn_with(at(0, 0, 20), v);
    const Entity w2 = s.w.spawn_with(at(10, 0, 0), Waypoint3D{});
    Waypoint3D first;
    first.next = w2;
    s.w.spawn_with(at(0, 0, 0), first);
    Array<GameplayLine> l;
    gameplay_debug_lines(s.w, kDebugJoints, l);
    CHECK(l.size() == 4u);  // a three-line cross and the axis
    gameplay_debug_lines(s.w, kDebugVehicles, l);
    REQUIRE(l.size() == 4u);
    u32 red = 0;
    for (u32 i = 0; i < l.size(); ++i) red += l[i].color.g < 0.2f ? 1u : 0u;
    CHECK(red == 1u);
    CHECK(l[2].a.x == doctest::Approx(-0.8f));  // wheel 2: rear left, track 0.8
    CHECK(l[2].a.z == doctest::Approx(20.0f + 1.3f - 0.35f));
    gameplay_debug_lines(s.w, kDebugPaths, l);
    REQUIRE(l.size() == 1u);
    CHECK(l[0].b.x == doctest::Approx(10.0f));
}

TEST_CASE("debug_lines: reading the lines does not change the world") {
    Scene s;
    s.w.spawn_with(at(0, 0, 0), BoxCollider3D{});
    s.w.spawn_with(at(0, 2, 0), Trigger3D{});
    const u64 before = s.w.state_hash();
    Array<GameplayLine> l;
    gameplay_debug_lines(s.w, kDebugAll, l);
    CHECK(l.size() == 24u);
    CHECK(s.w.state_hash() == before);
}
