// Joint2D (M-JOINT2D step 1): rods, ropes, springs and hinges built from a component, rebuilt when
// it changes, and removed with the component, a body or `enabled`.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/physics/joint2d.h"
#include "mce/physics/module.h"

using namespace mce;
using namespace mce::physics;
using math::vec2;

namespace {

constexpr f64 kDt = 1.0 / 60.0;

LocalTransform at(vec2 p) {
    LocalTransform t;
    t.position = {p.x, p.y, 0.0f};
    return t;
}

RigidBody2D body(BodyType type, f32 gravity_scale = 1.0f) {
    RigidBody2D rb;
    rb.type = type;
    rb.gravity_scale = gravity_scale;
    rb.mask = 0;  // the joint bodies never collide with anything in these tests
    return rb;
}

Collider2D box(vec2 half) {
    Collider2D c;
    c.shape = ColliderShape::box(half);
    return c;
}

void tick(World& w, u64& t, u32 n) {
    for (u32 i = 0; i < n; ++i) w.tick(kDt, ++t);
}

vec2 pos(World& w, Entity e) {
    const LocalTransform* t = w.try_get<LocalTransform>(e);
    return {t->position.x, t->position.y};
}

f32 dist(vec2 a, vec2 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

}  // namespace

TEST_CASE("joint2d: a rod keeps its length while the weight swings") {
    World w;
    install_physics2d(w);
    const Entity post = w.spawn_with(body(BodyType::Static), box({0.2f, 0.2f}), at({0, 5}));
    Joint2D j;
    j.kind = JointKind2D::Distance;
    j.connected = post;
    const Entity weight = w.spawn_with(body(BodyType::Dynamic), box({0.3f, 0.3f}), at({3, 5}), j);
    u64 t = 0;
    tick(w, t, 120);
    const Joint2D* got = w.try_get<Joint2D>(weight);
    CHECK(got->length_now == doctest::Approx(3.0f).epsilon(0.02));
    CHECK(dist(pos(w, weight), pos(w, post)) == doctest::Approx(3.0f).epsilon(0.03));
    CHECK(pos(w, weight).y < 5.0f);  // it fell along the arc
    CHECK(w.resource<World2D>().stats().joints == 1);
}

TEST_CASE("joint2d: a rope goes slack inside min .. max and holds at max") {
    World w;
    install_physics2d(w);
    const Entity post = w.spawn_with(body(BodyType::Static), box({0.2f, 0.2f}), at({0, 5}));
    Joint2D j;
    j.kind = JointKind2D::Distance;
    j.connected = post;
    j.min_length = 1.0f;
    j.max_length = 3.0f;
    const Entity weight = w.spawn_with(body(BodyType::Dynamic), box({0.3f, 0.3f}), at({0, 4}), j);
    u64 t = 0;
    tick(w, t, 10);
    CHECK(w.try_get<Joint2D>(weight)->length_now < 2.0f);  // still slack
    tick(w, t, 240);
    CHECK(w.try_get<Joint2D>(weight)->length_now == doctest::Approx(3.0f).epsilon(0.03));
    CHECK(pos(w, weight).y == doctest::Approx(2.0f).epsilon(0.05));  // hanging straight down at max length
}

TEST_CASE("joint2d: a spring settles below its rest length under gravity") {
    World w;
    install_physics2d(w);
    const Entity post = w.spawn_with(body(BodyType::Static), box({0.2f, 0.2f}), at({0, 6}));
    Joint2D j;
    j.kind = JointKind2D::Distance;
    j.connected = post;
    j.length = 2.0f;
    j.min_length = 0.5f;
    j.max_length = 6.0f;
    j.hertz = 2.0f;
    j.damping_ratio = 0.7f;
    const Entity weight = w.spawn_with(body(BodyType::Dynamic), box({0.3f, 0.3f}), at({0, 4}), j);
    u64 t = 0;
    tick(w, t, 600);
    const f32 len = w.try_get<Joint2D>(weight)->length_now;
    CHECK(len > 2.0f);
    CHECK(len < 6.0f);
}

TEST_CASE("joint2d: a hinge swings a door down to its lower limit") {
    World w;
    install_physics2d(w);
    const Entity post = w.spawn_with(body(BodyType::Static), box({0.1f, 0.1f}), at({0, 0}));
    Joint2D j;
    j.connected = post;
    j.anchor = {-1.0f, 0.0f};  // the door's left edge is the pivot
    j.limits = 1;
    j.lower = -45.0f;
    j.upper = 45.0f;
    const Entity door = w.spawn_with(body(BodyType::Dynamic), box({1.0f, 0.1f}), at({1, 0}), j);
    u64 t = 0;
    tick(w, t, 300);
    const f32 a = w.try_get<Joint2D>(door)->angle_now;
    CHECK(a < -40.0f);
    CHECK(a > -47.0f);
}

TEST_CASE("joint2d: without limits the same door swings past 45 degrees") {
    World w;
    install_physics2d(w);
    const Entity post = w.spawn_with(body(BodyType::Static), box({0.1f, 0.1f}), at({0, 0}));
    Joint2D j;
    j.connected = post;
    j.anchor = {-1.0f, 0.0f};
    const Entity door = w.spawn_with(body(BodyType::Dynamic), box({1.0f, 0.1f}), at({1, 0}), j);
    u64 t = 0;
    tick(w, t, 120);
    CHECK(w.try_get<Joint2D>(door)->angle_now < -60.0f);
}

TEST_CASE("joint2d: a motor turns the hinge, and a new speed updates in place") {
    World w;
    install_physics2d(w);
    const Entity post = w.spawn_with(body(BodyType::Static), box({0.1f, 0.1f}), at({0, 0}));
    Joint2D j;
    j.connected = post;
    j.anchor = {-1.0f, 0.0f};
    j.motor = 1;
    j.motor_speed = 45.0f;
    j.max_motor_torque = 100000.0f;
    const Entity wheel = w.spawn_with(body(BodyType::Dynamic, 0.0f), box({1.0f, 0.1f}), at({1, 0}), j);
    u64 t = 0;
    tick(w, t, 60);
    const f32 a1 = w.try_get<Joint2D>(wheel)->angle_now;
    CHECK(a1 == doctest::Approx(45.0f).epsilon(0.15));

    w.try_get_mut<Joint2D>(wheel)->motor_speed = -45.0f;
    tick(w, t, 60);
    const f32 a2 = w.try_get<Joint2D>(wheel)->angle_now;
    CHECK(a2 < a1 - 30.0f);                                 // it reversed
    CHECK(w.resource<World2D>().stats().joints == 1);       // updated in place, not duplicated
}

TEST_CASE("joint2d: enabled 0, a removed component and a destroyed body each remove the joint") {
    World w;
    install_physics2d(w);
    const Entity post = w.spawn_with(body(BodyType::Static), box({0.2f, 0.2f}), at({0, 5}));
    Joint2D j;
    j.kind = JointKind2D::Distance;
    j.connected = post;
    const Entity a = w.spawn_with(body(BodyType::Dynamic), box({0.3f, 0.3f}), at({2, 5}), j);
    const Entity b = w.spawn_with(body(BodyType::Dynamic), box({0.3f, 0.3f}), at({-2, 5}), j);
    u64 t = 0;
    tick(w, t, 2);
    World2D& pw = w.resource<World2D>();
    CHECK(pw.stats().joints == 2);

    w.try_get_mut<Joint2D>(a)->enabled = 0;
    tick(w, t, 1);
    CHECK(pw.stats().joints == 1);
    w.try_get_mut<Joint2D>(a)->enabled = 1;  // back on
    tick(w, t, 1);
    CHECK(pw.stats().joints == 2);

    w.commands().remove<Joint2D>(a);
    tick(w, t, 1);
    CHECK(pw.stats().joints == 1);

    w.destroy(post);  // b's other end
    tick(w, t, 2);
    CHECK(pw.stats().joints == 0);
    CHECK(w.try_get<Joint2D>(b) != nullptr);  // the component stays; it simply has nothing to join
}

TEST_CASE("joint2d: changing a setting rebuilds the joint from the current poses") {
    World w;
    install_physics2d(w);
    const Entity post = w.spawn_with(body(BodyType::Static), box({0.2f, 0.2f}), at({0, 5}));
    Joint2D j;
    j.kind = JointKind2D::Distance;
    j.connected = post;
    const Entity weight = w.spawn_with(body(BodyType::Dynamic), box({0.3f, 0.3f}), at({3, 5}), j);
    u64 t = 0;
    tick(w, t, 60);
    CHECK(w.try_get<Joint2D>(weight)->length_now == doctest::Approx(3.0f).epsilon(0.03));
    w.try_get_mut<Joint2D>(weight)->length = 2.0f;
    w.try_get_mut<Joint2D>(weight)->min_length = 2.0f;
    w.try_get_mut<Joint2D>(weight)->max_length = 2.0f;
    tick(w, t, 120);
    CHECK(w.try_get<Joint2D>(weight)->length_now == doctest::Approx(2.0f).epsilon(0.05));
}

TEST_CASE("joint2d: no connected entity, or a connected entity without a body, builds nothing") {
    World w;
    install_physics2d(w);
    Joint2D j;
    j.kind = JointKind2D::Distance;
    const Entity lone = w.spawn_with(body(BodyType::Dynamic), box({0.3f, 0.3f}), at({0, 5}), j);
    const Entity bare = w.spawn_with(at({4, 5}));  // no body
    u64 t = 0;
    tick(w, t, 5);
    CHECK(w.resource<World2D>().stats().joints == 0);
    w.try_get_mut<Joint2D>(lone)->connected = bare;
    tick(w, t, 5);
    CHECK(w.resource<World2D>().stats().joints == 0);
}

TEST_CASE("joint2d: non-finite settings are repaired once, not rebuilt every tick") {
    World w;
    install_physics2d(w);
    const Entity post = w.spawn_with(body(BodyType::Static), box({0.1f, 0.1f}), at({0, 0}));
    Joint2D j;
    j.connected = post;
    j.anchor = {-1.0f, 0.0f};
    j.upper = std::nanf("");  // limits are off, so only a repair is visible
    j.max_motor_torque = std::numeric_limits<f32>::infinity();
    const Entity door = w.spawn_with(body(BodyType::Dynamic), box({1.0f, 0.1f}), at({1, 0}), j);
    u64 t = 0;
    tick(w, t, 120);
    // A joint rebuilt every tick would restart its rest angle and never get far from 0.
    CHECK(w.try_get<Joint2D>(door)->angle_now < -60.0f);
    CHECK(w.resource<World2D>().stats().joints == 1);
}

TEST_CASE("joint2d: a negative spring, swapped limits and swapped rope lengths are repaired") {
    World w;
    install_physics2d(w);
    const Entity post = w.spawn_with(body(BodyType::Static), box({0.2f, 0.2f}), at({0, 5}));
    Joint2D rod;
    rod.kind = JointKind2D::Distance;
    rod.connected = post;
    rod.hertz = -3.0f;  // negative: a rigid rod
    const Entity weight = w.spawn_with(body(BodyType::Dynamic), box({0.3f, 0.3f}), at({3, 5}), rod);

    const Entity hinge_post = w.spawn_with(body(BodyType::Static), box({0.1f, 0.1f}), at({20, 0}));
    Joint2D hinge;
    hinge.connected = hinge_post;
    hinge.anchor = {-1.0f, 0.0f};
    hinge.limits = 1;
    hinge.lower = 45.0f;  // lower > upper: swapped to -45 .. 45
    hinge.upper = -45.0f;
    const Entity door = w.spawn_with(body(BodyType::Dynamic), box({1.0f, 0.1f}), at({21, 0}), hinge);
    u64 t = 0;
    tick(w, t, 300);
    CHECK(w.try_get<Joint2D>(weight)->length_now == doctest::Approx(3.0f).epsilon(0.03));
    const f32 a = w.try_get<Joint2D>(door)->angle_now;
    CHECK(a < -40.0f);
    CHECK(a > -47.0f);
}

TEST_CASE("joint2d: a refused joint is tried again when a body becomes dynamic") {
    World w;
    install_physics2d(w);
    const Entity post = w.spawn_with(body(BodyType::Static), box({0.2f, 0.2f}), at({0, 5}));
    Joint2D j;
    j.kind = JointKind2D::Distance;
    j.connected = post;
    const Entity other = w.spawn_with(body(BodyType::Static), box({0.3f, 0.3f}), at({2, 5}), j);
    u64 t = 0;
    tick(w, t, 5);
    CHECK(w.resource<World2D>().stats().joints == 0);  // two static bodies: refused
    w.set(other, body(BodyType::Dynamic));
    tick(w, t, 5);
    CHECK(w.resource<World2D>().stats().joints == 1);
}

TEST_CASE("joint2d: the hinge pivot comes from the body's pose, even for a child entity") {
    World w;
    install_physics2d(w);
    const Entity parent = w.spawn_with(at({100, 100}));
    const Entity post = w.spawn_with(body(BodyType::Static), box({0.1f, 0.1f}), at({0, 0}));
    Joint2D j;
    j.connected = post;
    j.anchor = {-1.0f, 0.0f};
    j.limits = 1;
    j.enabled = 0;  // the body exists first, the joint is built later
    const Entity door = w.spawn_with(body(BodyType::Dynamic, 0.0f), box({1.0f, 0.1f}), at({1, 0}), j, ChildOf{parent});
    u64 t = 0;
    tick(w, t, 2);
    w.try_get_mut<LocalTransform>(door)->position = {50, 50, 0};  // not a pose the world holds for the body
    w.try_get_mut<Joint2D>(door)->enabled = 1;
    w.try_get_mut<Joint2D>(door)->motor = 1;
    w.try_get_mut<Joint2D>(door)->motor_speed = -30.0f;
    w.try_get_mut<Joint2D>(door)->max_motor_torque = 100000.0f;
    tick(w, t, 30);
    CHECK(w.resource<World2D>().stats().joints == 1);
    const f32 a = w.try_get<Joint2D>(door)->angle_now;
    CHECK(a == doctest::Approx(-15.0f).epsilon(0.2));  // half a second at -30 deg/s around the right pivot
}
