// World3D joints (M-GAMEPLAY3D step 2): fixed, hinge, slider, distance, cone and six-DOF joints with
// limits, motors, the collide-connected filter, breaking, and determinism.

#include <cmath>

#include "helpers.h"

using namespace phys3_test;

namespace {

// A dynamic 1 x 0.2 x 0.2 bar whose left end is at `pivot` (it hangs to the +x side).
bool add_bar(World3D& w, Entity e, vec3 pivot, f32 density = 1000.0f) {
    BodyDesc3D d;
    d.type = BodyType3D::Dynamic;
    d.shape = Shape3D::box({0.5f, 0.1f, 0.1f});
    d.position = pivot + vec3{0.5f, 0.0f, 0.0f};
    d.density = density;
    return w.create_body(e, d);
}

JointState3D state_of(const World3D& w, u32 joint) {
    JointState3D s;
    REQUIRE(w.joint_state(joint, s));
    return s;
}

void steps(World3D& w, int n) {
    for (int i = 0; i < n; ++i) w.step(kDt);
}

}  // namespace

TEST_CASE("joints: lifecycle, refused descriptions, destroyed with a body") {
    World3D w;
    add_floor(w);
    REQUIRE(add_bar(w, ent(1), {0, 2, 0}));
    REQUIRE(add_bar(w, ent(2), {0, 4, 0}));
    REQUIRE(w.create_character(ent(3), {5, 1, 0}));

    JointDesc3D d;
    d.a = ent(1);
    d.anchor = {0, 2, 0};
    d.axis = {0, 0, 1};
    CHECK(w.create_joint(JointDesc3D{}) == 0);  // no body
    d.b = ent(1);
    CHECK(w.create_joint(d) == 0);  // a == b
    d.b = ent(3);
    CHECK(w.create_joint(d) == 0);  // characters are not bodies
    d.b = ent(1000);
    d.a = ent(1000);
    d.b = Entity::null;
    CHECK(w.create_joint(d) == 0);  // static to the world
    d.a = ent(1);
    d.axis = {};
    CHECK(w.create_joint(d) == 0);  // a hinge needs an axis
    d.axis = {0, 0, 1};

    const u32 j1 = w.create_joint(d);
    d.a = ent(2);
    d.b = ent(1);
    const u32 j2 = w.create_joint(d);
    REQUIRE(j1 != 0);
    REQUIRE(j2 != 0);
    CHECK(j1 != j2);
    CHECK(w.has_joint(j1));
    CHECK(w.stats().joints == 2);

    w.destroy_body(ent(1));  // takes both joints with it
    CHECK_FALSE(w.has_joint(j1));
    CHECK_FALSE(w.has_joint(j2));
    CHECK(w.stats().joints == 0);
    JointState3D s;
    CHECK_FALSE(w.joint_state(j1, s));
    w.destroy_joint(j1);  // stale: no-op

    REQUIRE(add_bar(w, ent(1), {0, 2, 0}));
    d.a = ent(1);
    d.b = Entity::null;
    const u32 j3 = w.create_joint(d);  // reuses the slot under a new id
    REQUIRE(j3 != 0);
    CHECK(j3 != j1);
    CHECK_FALSE(w.has_joint(j1));
    w.destroy_joint(j3);
    CHECK(w.stats().joints == 0);
}

TEST_CASE("joints: a hinge swings within its limits; motors drive it") {
    World3D w;
    REQUIRE(add_bar(w, ent(1), {0, 2, 0}));
    JointDesc3D d;
    d.type = JointType3D::Hinge;
    d.a = ent(1);
    d.anchor = {0, 2, 0};
    d.axis = {0, 0, 1};
    d.limits = true;
    d.limit_min = -0.3f;
    d.limit_max = 0.3f;
    const u32 j = w.create_joint(d);
    REQUIRE(j != 0);
    f32 lowest = 0.0f;
    for (int i = 0; i < 120; ++i) {
        w.step(kDt);
        lowest = math::min(lowest, state_of(w, j).position);
    }
    CHECK(lowest < -0.25f);  // it swung down (clockwise about +z) ...
    CHECK(lowest > -0.33f);  // ... and stopped at the limit
    // The pivot holds: the bar's end stays at the anchor.
    const vec3 p = position_of(w, ent(1));
    CHECK(math::length(p - vec3{0, 2, 0}) == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(state_of(w, j).force > 0.0f);  // it holds the bar up

    // A wheel about +y: a velocity motor spins it at the target rate.
    World3D v;
    BodyDesc3D wheel;
    wheel.type = BodyType3D::Dynamic;
    wheel.shape = Shape3D::box({0.5f, 0.1f, 0.5f});
    wheel.position = {0, 1, 0};
    wheel.gravity_scale = 0.0f;
    REQUIRE(v.create_body(ent(1), wheel));
    JointDesc3D m;
    m.type = JointType3D::Hinge;
    m.a = ent(1);
    m.anchor = {0, 1, 0};
    m.axis = {0, 1, 0};
    m.motor = JointMotorMode3D::Velocity;
    m.motor_target = 2.0f;
    const u32 jm = v.create_joint(m);
    REQUIRE(jm != 0);
    steps(v, 60);
    BodyState3D s;
    REQUIRE(v.body_state(ent(1), s));
    CHECK(s.angular_velocity.y == doctest::Approx(2.0f).epsilon(0.02));
    CHECK(std::fabs(s.angular_velocity.x) < 1e-3f);

    // A position motor turns it to a target angle and holds it.
    v.set_joint_motor(jm, JointMotorMode3D::Position, 1.0f);
    steps(v, 240);
    CHECK(state_of(v, jm).position == doctest::Approx(1.0f).epsilon(0.02));
    // A motor too weak for the job barely moves a heavy load.
    v.set_joint_motor(jm, JointMotorMode3D::Velocity, -5.0f, 0.001f);
    steps(v, 30);
    REQUIRE(v.body_state(ent(1), s));
    CHECK(s.angular_velocity.y > -0.5f);
}

TEST_CASE("joints: a slider moves only along its axis, within limits, and by its motor") {
    World3D w;
    REQUIRE(add_box(w, ent(1), BodyType3D::Dynamic, {0, 2, 0}, {0.25f, 0.25f, 0.25f}));
    JointDesc3D d;
    d.type = JointType3D::Slider;
    d.a = ent(1);
    d.anchor = {0, 2, 0};
    d.axis = {1, 0, 0};
    d.limits = true;
    d.limit_min = -0.5f;
    d.limit_max = 0.5f;
    const u32 j = w.create_joint(d);
    REQUIRE(j != 0);
    w.apply_impulse(ent(1), {500.0f, 0.0f, 300.0f});  // hard along x (and across, which it refuses)
    steps(w, 60);
    const vec3 p = position_of(w, ent(1));
    CHECK(p.x == doctest::Approx(0.5f).epsilon(0.03));  // at the limit
    CHECK(std::fabs(p.y - 2.0f) < 0.01f);               // gravity does not pull it off the rail
    CHECK(std::fabs(p.z) < 0.01f);
    CHECK(state_of(w, j).position == doctest::Approx(0.5f).epsilon(0.03));

    w.set_joint_motor(j, JointMotorMode3D::Position, -0.2f);
    steps(w, 240);
    CHECK(state_of(w, j).position == doctest::Approx(-0.2f).epsilon(0.03));
}

TEST_CASE("joints: distance (a rope), cone and six-DOF joints") {
    // A rope: the box hangs 2 m under the hook and keeps that distance while it swings.
    World3D w;
    REQUIRE(add_box(w, ent(1), BodyType3D::Dynamic, {0, 1, 0}, {0.2f, 0.2f, 0.2f}));
    JointDesc3D rope;
    rope.type = JointType3D::Distance;
    rope.a = ent(1);
    rope.anchor = {0, 1, 0};
    rope.anchor_b = {0, 3, 0};
    rope.limit_min = rope.limit_max = -1.0f;  // the distance at creation
    const u32 j = w.create_joint(rope);
    REQUIRE(j != 0);
    w.apply_impulse(ent(1), {150.0f, 0.0f, 0.0f});
    for (int i = 0; i < 120; ++i) {
        w.step(kDt);
        CHECK(state_of(w, j).position == doctest::Approx(2.0f).epsilon(0.02));
    }
    CHECK(std::fabs(position_of(w, ent(1)).x) > 0.1f);  // it swung

    // A slack rope (0..3 m): the box falls until the rope is taut.
    World3D slack;
    REQUIRE(add_box(slack, ent(1), BodyType3D::Dynamic, {0, 2, 0}, {0.2f, 0.2f, 0.2f}));
    rope.anchor = {0, 2, 0};
    rope.limit_min = 0.0f;
    rope.limit_max = 2.0f;
    REQUIRE(slack.create_joint(rope) != 0);
    steps(slack, 120);
    CHECK(position_of(slack, ent(1)).y == doctest::Approx(1.0f).epsilon(0.03));

    // A cone: the pendulum swings no further than the cone's half angle from straight down.
    World3D c;
    REQUIRE(add_box(c, ent(1), BodyType3D::Dynamic, {0, 1, 0}, {0.1f, 0.5f, 0.1f}));
    JointDesc3D cone;
    cone.type = JointType3D::Cone;
    cone.a = ent(1);
    cone.anchor = {0, 1.5f, 0};
    cone.axis = {0, -1, 0};
    cone.cone_half_angle = 0.3f;
    REQUIRE(c.create_joint(cone) != 0);
    c.apply_impulse(ent(1), {60.0f, 0.0f, 30.0f});
    f32 widest = 0.0f;
    for (int i = 0; i < 120; ++i) {
        c.step(kDt);
        const vec3 down = math::normalize(position_of(c, ent(1)) - vec3{0, 1.5f, 0});
        widest = math::max(widest, std::acos(math::clamp(-down.y, -1.0f, 1.0f)));
    }
    CHECK(widest > 0.2f);
    CHECK(widest < 0.34f);

    // Six-DOF with every axis locked but y: the box falls straight, without turning.
    World3D x;
    add_floor(x);
    REQUIRE(add_box(x, ent(1), BodyType3D::Dynamic, {0, 3, 0}, {0.5f, 0.5f, 0.5f}));
    JointDesc3D six;
    six.type = JointType3D::SixDOF;
    six.a = ent(1);
    six.anchor = {0, 3, 0};
    six.axis = {1, 0, 0};
    six.linear_min = {0, 1, 0};
    six.linear_max = {0, -1, 0};  // y free (min > max), x and z locked
    REQUIRE(x.create_joint(six) != 0);
    x.apply_impulse(ent(1), {300.0f, 0.0f, 300.0f});
    x.set_angular_velocity(ent(1), {3.0f, 1.0f, 0.0f});
    steps(x, 120);
    BodyState3D s;
    REQUIRE(x.body_state(ent(1), s));
    CHECK(s.position.y == doctest::Approx(0.5f).epsilon(0.03));
    CHECK(std::fabs(s.position.x) < 0.01f);
    CHECK(std::fabs(s.position.z) < 0.01f);
    CHECK(std::fabs(s.rotation.w) > 0.999f);

    // Six-DOF limited: y may move between -1 and 0 only, so the box stops 1 m down, in the air.
    World3D lim;
    add_floor(lim);
    REQUIRE(add_box(lim, ent(1), BodyType3D::Dynamic, {0, 3, 0}, {0.5f, 0.5f, 0.5f}));
    six.linear_min = {0, -1, 0};
    six.linear_max = {0, 0, 0};
    REQUIRE(lim.create_joint(six) != 0);
    steps(lim, 120);
    CHECK(position_of(lim, ent(1)).y == doctest::Approx(2.0f).epsilon(0.02));
}

TEST_CASE("joints: fixed joints, and the bodies of a joint do not collide unless asked") {
    // Two overlapping boxes glued together fall as one and stay overlapped.
    World3D w;
    add_floor(w);
    REQUIRE(add_box(w, ent(1), BodyType3D::Dynamic, {0, 2, 0}, {0.5f, 0.5f, 0.5f}));
    REQUIRE(add_box(w, ent(2), BodyType3D::Dynamic, {0.6f, 2, 0}, {0.5f, 0.5f, 0.5f}));
    JointDesc3D d;
    d.type = JointType3D::Fixed;
    d.a = ent(1);
    d.b = ent(2);
    d.anchor = {0.3f, 2, 0};
    const u32 j = w.create_joint(d);
    REQUIRE(j != 0);
    steps(w, 120);
    const vec3 pa = position_of(w, ent(1)), pb = position_of(w, ent(2));
    CHECK(math::length(pb - pa) == doctest::Approx(0.6f).epsilon(0.02));
    CHECK(pa.y == doctest::Approx(0.5f).epsilon(0.05));  // landed

    // With collide_connected the overlap pushes them apart against the joint: they end further off.
    World3D c;
    REQUIRE(add_box(c, ent(1), BodyType3D::Dynamic, {0, 2, 0}, {0.5f, 0.5f, 0.5f}));
    REQUIRE(add_box(c, ent(2), BodyType3D::Dynamic, {0.6f, 2, 0}, {0.5f, 0.5f, 0.5f}));
    d.type = JointType3D::Distance;
    d.anchor = {0, 2, 0};
    d.anchor_b = {0.6f, 2, 0};
    d.limit_min = 0.0f;
    d.limit_max = 2.0f;
    d.collide_connected = true;
    REQUIRE(c.create_joint(d) != 0);
    steps(c, 30);
    CHECK(math::length(position_of(c, ent(2)) - position_of(c, ent(1))) > 0.9f);
}

TEST_CASE("joints: breaking under load, switching off and on") {
    // A 200 kg bar held level by a fixed joint rated for 1000 N breaks at once and falls.
    World3D w;
    REQUIRE(add_bar(w, ent(1), {0, 2, 0}, 5000.0f));  // 0.04 m^3 x 5000 = 200 kg: 1962 N
    JointDesc3D d;
    d.type = JointType3D::Fixed;
    d.a = ent(1);
    d.anchor = {0, 2, 0};
    d.break_force = 1000.0f;
    const u32 j = w.create_joint(d);
    REQUIRE(j != 0);
    steps(w, 2);
    JointState3D s = state_of(w, j);
    CHECK(s.broken);
    CHECK_FALSE(s.enabled);
    steps(w, 30);
    CHECK(position_of(w, ent(1)).y < 1.0f);  // fell
    w.set_joint_enabled(j, true);            // a broken joint stays broken
    CHECK(state_of(w, j).broken);
    CHECK_FALSE(state_of(w, j).enabled);

    // Rated for 10 kN it holds: the force is the weight, the torque the weight's lever (0.5 m).
    World3D h;
    REQUIRE(add_bar(h, ent(1), {0, 2, 0}, 5000.0f));
    d.break_force = 10000.0f;
    const u32 jh = h.create_joint(d);
    steps(h, 60);
    s = state_of(h, jh);
    CHECK_FALSE(s.broken);
    CHECK(s.force == doctest::Approx(1962.0f).epsilon(0.05));
    CHECK(s.torque == doctest::Approx(981.0f).epsilon(0.05));
    // The torque limit breaks it too.
    World3D t;
    REQUIRE(add_bar(t, ent(1), {0, 2, 0}, 5000.0f));
    d.break_torque = 500.0f;
    const u32 jt = t.create_joint(d);
    steps(t, 2);
    CHECK(state_of(t, jt).broken);

    // Switched off, a hinge lets go; switched on again, it catches the bar.
    World3D o;
    REQUIRE(add_bar(o, ent(1), {0, 2, 0}));
    d = JointDesc3D{};
    d.type = JointType3D::Distance;
    d.a = ent(1);
    d.anchor = {0.5f, 2, 0};
    d.anchor_b = {0.5f, 2.3f, 0};
    d.limit_min = 0.0f;
    d.limit_max = 0.8f;
    const u32 jo = o.create_joint(d);
    steps(o, 120);  // hanging on the leash, at rest
    const f32 hung = position_of(o, ent(1)).y;
    CHECK(hung == doctest::Approx(1.5f).epsilon(0.02));
    o.set_joint_enabled(jo, false);
    CHECK_FALSE(state_of(o, jo).enabled);
    steps(o, 15);
    CHECK(position_of(o, ent(1)).y < hung - 0.2f);  // falling
    o.set_joint_enabled(jo, true);
    CHECK(state_of(o, jo).enabled);
    steps(o, 120);
    CHECK(state_of(o, jo).position <= 0.82f);  // pulled back onto its leash
    CHECK(position_of(o, ent(1)).y > 1.4f);
}

TEST_CASE("joints: replays bit-identically, and the hash sees joints") {
    auto run = [](bool with_joint) {
        World3D w;
        add_floor(w);
        REQUIRE(add_bar(w, ent(1), {0, 3, 0}));
        REQUIRE(add_box(w, ent(2), BodyType3D::Dynamic, {1.5f, 3, 0}, {0.2f, 0.2f, 0.2f}));
        if (with_joint) {
            JointDesc3D d;
            d.type = JointType3D::Hinge;
            d.a = ent(1);
            d.anchor = {0, 3, 0};
            d.axis = {0, 0, 1};
            d.motor = JointMotorMode3D::Velocity;
            d.motor_target = 1.5f;
            d.motor_max_force = 50.0f;
            REQUIRE(w.create_joint(d) != 0);
            JointDesc3D r;
            r.type = JointType3D::Distance;
            r.a = ent(2);
            r.b = ent(1);
            r.anchor = {1.5f, 3, 0};
            r.anchor_b = {1.0f, 3, 0};
            r.limit_min = r.limit_max = -1.0f;
            r.break_torque = 1.0f;
            REQUIRE(w.create_joint(r) != 0);
        }
        steps(w, 180);
        return w.hash_state(0x1234);
    };
    const u64 a = run(true);
    CHECK(a == run(true));
    CHECK(a != run(false));
}
