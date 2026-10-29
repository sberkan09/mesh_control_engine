// LaunchVelocity2D, Impulse2D and ConstantForce2D (M-PHYSDRIVE2D step 1): pushing 2D bodies from
// components.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "mce/ecs/transform.h"
#include "mce/physics/force2d.h"
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

RigidBody2D body(BodyType type, f32 gravity_scale = 0.0f) {
    RigidBody2D rb;
    rb.type = type;
    rb.gravity_scale = gravity_scale;
    rb.mask = 0;
    return rb;
}

Collider2D box() {
    Collider2D c;
    c.shape = ColliderShape::box({0.5f, 0.5f});
    return c;
}

void tick(World& w, u64& t, u32 n) {
    for (u32 i = 0; i < n; ++i) w.tick(kDt, ++t);
}

vec2 pos(World& w, Entity e) {
    const LocalTransform* t = w.try_get<LocalTransform>(e);
    return {t->position.x, t->position.y};
}

}  // namespace

TEST_CASE("force2d: a launch velocity sets the speed once") {
    World w;
    install_physics2d(w);
    LaunchVelocity2D l;
    l.linear = {6.0f, 0.0f};
    const Entity e = w.spawn_with(body(BodyType::Dynamic), box(), at({0, 0}), l);
    u64 t = 0;
    tick(w, t, 60);
    CHECK(pos(w, e).x == doctest::Approx(6.0f).epsilon(0.05));
    CHECK((*w.try_get<LaunchVelocity2D>(e)).applied == 1);
    // Applied once: pushing the body back to rest afterwards is not undone by the component.
    w.resource<World2D>().set_linear_velocity(e, {0, 0});
    const f32 x = pos(w, e).x;
    tick(w, t, 30);
    CHECK(pos(w, e).x == doctest::Approx(x).epsilon(0.01));
}

TEST_CASE("force2d: a launch velocity is ignored on a static body and spins a dynamic one") {
    World w;
    install_physics2d(w);
    LaunchVelocity2D l;
    l.linear = {5.0f, 0.0f};
    l.angular = 90.0f;
    const Entity wall = w.spawn_with(body(BodyType::Static), box(), at({0, 0}), l);
    const Entity wheel = w.spawn_with(body(BodyType::Dynamic), box(), at({0, 10}), l);
    u64 t = 0;
    tick(w, t, 30);
    CHECK(pos(w, wall).x == 0.0f);
    CHECK((*w.try_get<LaunchVelocity2D>(wall)).applied == 0);
    BodyState bs;
    REQUIRE(w.resource<World2D>().body_state(wheel, bs));
    CHECK(bs.angle == doctest::Approx(1.5707963f * 0.5f).epsilon(0.05));
}

TEST_CASE("force2d: an impulse pushes once, then reads zero") {
    World w;
    install_physics2d(w);
    Impulse2D im;
    im.impulse = {0.0f, 4.0f};
    const Entity e = w.spawn_with(body(BodyType::Dynamic), box(), at({0, 0}), im);
    u64 t = 0;
    tick(w, t, 2);
    CHECK((*w.try_get<Impulse2D>(e)).impulse.y == 0.0f);
    const f32 v = w.resource<World2D>().linear_velocity(e).y;
    CHECK(v > 0.5f);  // density 1, 1x1 box: m = 1, so about 4 m/s
    tick(w, t, 10);
    CHECK(w.resource<World2D>().linear_velocity(e).y == doctest::Approx(v).epsilon(0.01));
    w.try_get_mut<Impulse2D>(e)->impulse = {0.0f, 4.0f};  // a new value pushes again
    tick(w, t, 1);
    CHECK(w.resource<World2D>().linear_velocity(e).y > v * 1.5f);
}

TEST_CASE("force2d: an impulse off the centre spins the body") {
    World w;
    install_physics2d(w);
    Impulse2D im;
    im.impulse = {0.0f, 3.0f};
    im.point = {0.5f, 0.0f};
    const Entity e = w.spawn_with(body(BodyType::Dynamic), box(), at({0, 0}), im);
    u64 t = 0;
    tick(w, t, 5);
    BodyState bs;
    REQUIRE(w.resource<World2D>().body_state(e, bs));
    CHECK(bs.angular_velocity > 0.1f);
}

TEST_CASE("force2d: a constant force accelerates a dynamic body and leaves a kinematic one alone") {
    World w;
    install_physics2d(w);
    ConstantForce2D cf;
    cf.force = {2.0f, 0.0f};
    const Entity ball = w.spawn_with(body(BodyType::Dynamic), box(), at({0, 0}), cf);
    const Entity platform = w.spawn_with(body(BodyType::Kinematic), box(), at({0, 5}), cf);
    u64 t = 0;
    tick(w, t, 60);
    CHECK(pos(w, ball).x == doctest::Approx(1.0f).epsilon(0.1));  // 0.5 * (2 / 1) * 1^2
    CHECK(pos(w, platform).x == 0.0f);
    w.remove<ConstantForce2D>(ball);
    const f32 v = w.resource<World2D>().linear_velocity(ball).x;
    tick(w, t, 30);
    CHECK(w.resource<World2D>().linear_velocity(ball).x == doctest::Approx(v).epsilon(0.01));
}

TEST_CASE("force2d: non-finite values count as zero and do not poison the body") {
    World w;
    install_physics2d(w);
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    LaunchVelocity2D l;
    l.linear = {nan, 1.0f};
    l.angular = nan;
    Impulse2D im;
    im.impulse = {nan, 1.0f};
    ConstantForce2D cf;
    cf.force = {1.0f, std::numeric_limits<f32>::infinity()};
    const Entity e = w.spawn_with(body(BodyType::Dynamic), box(), at({0, 0}), l, im, cf);
    u64 t = 0;
    tick(w, t, 20);
    const vec2 p = pos(w, e);
    CHECK(std::isfinite(p.x));
    CHECK(std::isfinite(p.y));
    CHECK((*w.try_get<Impulse2D>(e)).impulse.x == 0.0f);
}
