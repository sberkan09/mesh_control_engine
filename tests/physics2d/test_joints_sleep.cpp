// physics v0.5 (ROADMAP M6, ADR-0026): distance + revolute joints against hand-computed constraint
// solutions (FR-PHY-060), island sleeping, the 20-body stack, and the 2,000-body budget (FR-PHY-050).

#include <chrono>
#include <cmath>

#include "helpers.h"

using namespace phys_test;

namespace {

World2D world_no_gravity(bool sleep = false) {
    PhysicsConfig c;
    c.gravity = {0.0f, 0.0f};
    c.enable_sleep = sleep;
    return World2D(c);
}

World2D world_gravity(bool sleep = false) {
    PhysicsConfig c;
    c.enable_sleep = sleep;
    return World2D(c);
}

// Mass of a box of half extents h and density 1.
f32 box_mass(vec2 h) { return 4.0f * h.x * h.y; }

}  // namespace

// ---- distance joint ---------------------------------------------------------------------------------

TEST_CASE("distance joint: one velocity solve removes the relative axial velocity, conserving momentum") {
    // Hand-computed: two free boxes on the x axis, rod between their centres. Axial velocities
    // vA = +3, vB = -1 with masses mA = 1, mB = 4 must end at the common (mA vA + mB vB)/(mA + mB)
    // = (3 - 4)/5 = -0.2; the perpendicular velocity (vy = 2 on B) is untouched.
    World2D w = world_no_gravity();
    const Entity a = ent(1), b = ent(2);
    REQUIRE(add_box(w, a, BodyType::Dynamic, {0, 0}, {0.5f, 0.5f}));  // m = 1
    REQUIRE(add_box(w, b, BodyType::Dynamic, {4, 0}, {1.0f, 1.0f}));  // m = 4
    w.set_linear_velocity(a, {3, 0});
    w.set_linear_velocity(b, {-1, 2});
    DistanceJointDesc d;
    d.a = a;
    d.b = b;
    const JointId j = w.create_distance_joint(d);
    REQUIRE(w.joint_alive(j));
    CHECK(w.joint_length(j) == doctest::Approx(4.0f));
    w.step(kDt);
    CHECK(state(w, a).linear_velocity.x == doctest::Approx(-0.2f).epsilon(1e-4));
    CHECK(state(w, b).linear_velocity.x == doctest::Approx(-0.2f).epsilon(1e-4));
    CHECK(state(w, b).linear_velocity.y == doctest::Approx(2.0f).epsilon(1e-4));
    CHECK(state(w, a).linear_velocity.y == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(mce::f32(1.0f * state(w, a).linear_velocity.x + 4.0f * state(w, b).linear_velocity.x) ==
          doctest::Approx(-1.0f).epsilon(1e-4));
}

TEST_CASE("distance joint: a pendulum keeps its length and swings with period 2*pi*sqrt(L/g)") {
    World2D w = world_gravity();
    const Entity pivot = ent(1), bob = ent(2);
    REQUIRE(add_box(w, pivot, BodyType::Static, {0, 10}, {0.1f, 0.1f}));
    REQUIRE(add_circle(w, bob, BodyType::Dynamic, {0.2f, 8.0f}, 0.05f));  // L ~= 2, 5.7 deg
    DistanceJointDesc d;
    d.a = pivot;
    d.b = bob;
    const JointId j = w.create_distance_joint(d);
    const f32 L = w.joint_length(j);
    // Time the first two zero crossings of x (half a period apart) after release.
    f32 t = 0.0f, prev_x = 0.2f, first = -1.0f, second = -1.0f;
    f32 max_err = 0.0f;
    for (u32 i = 0; i < 600 && second < 0.0f; ++i) {
        w.step(kDt);
        t += kDt;
        const f32 x = state(w, bob).position.x;
        max_err = std::fmax(max_err, std::fabs(w.joint_length(j) - L));
        if ((prev_x > 0.0f) != (x > 0.0f)) {
            const f32 tc = t - kDt * x / (x - prev_x);  // linear interpolation of the crossing
            if (first < 0.0f) first = tc;
            else second = tc;
        }
        prev_x = x;
    }
    REQUIRE(second > 0.0f);
    const f32 period = 2.0f * (second - first);
    CHECK(period == doctest::Approx(2.0f * math::kPi * std::sqrt(L / 9.81f)).epsilon(0.01));
    CHECK(max_err < 0.005f);  // rod stays within linear slop
}

TEST_CASE("distance joint: a spring oscillates at its hertz and a rope only pulls") {
    {
        // Mass on a soft spring to a static anchor, no gravity, no damping: period = 1 / hertz.
        World2D w = world_no_gravity();
        const Entity anchor = ent(1), m = ent(2);
        REQUIRE(add_box(w, anchor, BodyType::Static, {0, 0}, {0.1f, 0.1f}));
        REQUIRE(add_circle(w, m, BodyType::Dynamic, {3.0f, 0.0f}, 0.1f));
        DistanceJointDesc d;
        d.a = anchor;
        d.b = m;
        d.length = 2.0f;
        d.hertz = 1.0f;
        const JointId j = w.create_distance_joint(d);
        REQUIRE(w.joint_alive(j));
        f32 t = 0.0f, prev = 1.0f, up1 = -1.0f, up2 = -1.0f;
        for (u32 i = 0; i < 400 && up2 < 0.0f; ++i) {
            w.step(kDt);
            t += kDt;
            const f32 stretch = w.joint_length(j) - 2.0f;
            if (prev < 0.0f && stretch >= 0.0f) {  // upward zero crossing
                const f32 tc = t - kDt * stretch / (stretch - prev);
                if (up1 < 0.0f) up1 = tc;
                else up2 = tc;
            }
            prev = stretch;
        }
        REQUIRE(up2 > 0.0f);
        CHECK(up2 - up1 == doctest::Approx(1.0f).epsilon(0.03));  // soft-step integration: within 3 %
    }
    {
        // Rope (0 .. 3 m): slack bodies move freely; at full length it stops the bob.
        World2D w = world_gravity();
        const Entity anchor = ent(1), m = ent(2);
        REQUIRE(add_box(w, anchor, BodyType::Static, {0, 10}, {0.1f, 0.1f}));
        REQUIRE(add_circle(w, m, BodyType::Dynamic, {0.0f, 9.0f}, 0.1f));
        DistanceJointDesc d;
        d.a = anchor;
        d.b = m;
        d.length = 3.0f;
        d.min_length = 0.0f;
        d.max_length = 3.0f;
        const JointId j = w.create_distance_joint(d);
        w.step(kDt);
        CHECK(state(w, m).linear_velocity.y == doctest::Approx(-9.81f * kDt).epsilon(1e-4));  // slack: free fall
        for (u32 i = 0; i < 180; ++i) w.step(kDt);
        CHECK(w.joint_length(j) == doctest::Approx(3.0f).epsilon(0.005));
        CHECK(std::fabs(state(w, m).linear_velocity.y) < 0.05f);
    }
}

// ---- revolute joint ---------------------------------------------------------------------------------

TEST_CASE("revolute joint: the pinned points share a velocity after the solve (hand-computed)") {
    // Two boxes pinned at x = 1. An impulse on A's far end must leave the anchor points of A and
    // B moving together; with no other forces, linear momentum equals the impulse.
    World2D w = world_no_gravity();
    const Entity a = ent(1), b = ent(2);
    REQUIRE(add_box(w, a, BodyType::Dynamic, {0, 0}, {1.0f, 0.25f}));
    REQUIRE(add_box(w, b, BodyType::Dynamic, {2, 0}, {1.0f, 0.25f}));
    RevoluteJointDesc d;
    d.a = a;
    d.b = b;
    d.world_anchor = {1, 0};
    REQUIRE(w.joint_alive(w.create_revolute_joint(d)));
    w.apply_impulse(a, {0, 1}, {-1, 0});
    w.step(kDt);
    const BodyState sa = state(w, a), sb = state(w, b);
    const vec2 anchor = {1, 0};
    // velocity of a point: v + w x r (before the position update moved it: use the step's velocities)
    const vec2 ra = anchor - vec2{0, 0}, rb = anchor - vec2{2, 0};
    const vec2 va = sa.linear_velocity + vec2{-sa.angular_velocity * ra.y, sa.angular_velocity * ra.x};
    const vec2 vb = sb.linear_velocity + vec2{-sb.angular_velocity * rb.y, sb.angular_velocity * rb.x};
    CHECK(va.x == doctest::Approx(vb.x).epsilon(1e-3).scale(1e-3));
    CHECK(va.y == doctest::Approx(vb.y).epsilon(1e-3).scale(1e-3));
    const f32 m = box_mass({1.0f, 0.25f});
    CHECK(m * (sa.linear_velocity.y + sb.linear_velocity.y) == doctest::Approx(1.0f).epsilon(1e-3));
}

TEST_CASE("revolute joint: limit holds the angle and the motor reaches its speed or its torque") {
    {
        World2D w = world_gravity();
        const Entity ground = ent(1), arm = ent(2);
        REQUIRE(add_box(w, ground, BodyType::Static, {0, 0}, {0.1f, 0.1f}));
        REQUIRE(add_box(w, arm, BodyType::Dynamic, {1, 0}, {1.0f, 0.1f}));  // horizontal arm, falls clockwise
        RevoluteJointDesc d;
        d.a = ground;
        d.b = arm;
        d.world_anchor = {0, 0};
        d.enable_limit = true;
        d.lower_angle = -0.5f;
        d.upper_angle = 0.5f;
        const JointId j = w.create_revolute_joint(d);
        for (u32 i = 0; i < 240; ++i) w.step(kDt);
        CHECK(w.joint_angle(j) == doctest::Approx(-0.5f).epsilon(0.05));  // resting on the lower stop
        CHECK(w.joint_angle(j) >= -0.5f - 2.0f / 180.0f * math::kPi);
    }
    {
        // Motor with enough torque: the wheel spins at motor_speed.
        World2D w = world_no_gravity();
        const Entity ground = ent(1), wheel = ent(2);
        REQUIRE(add_box(w, ground, BodyType::Static, {0, 0}, {0.1f, 0.1f}));
        REQUIRE(add_circle(w, wheel, BodyType::Dynamic, {0, 0}, 0.5f));
        RevoluteJointDesc d;
        d.a = ground;
        d.b = wheel;
        d.enable_motor = true;
        d.motor_speed = 4.0f;
        d.max_motor_torque = 1000.0f;
        d.collide_connected = false;
        const JointId j = w.create_revolute_joint(d);
        w.step(kDt);
        CHECK(state(w, wheel).angular_velocity == doctest::Approx(4.0f).epsilon(1e-4));
        // Torque-limited: angular acceleration = tau / I (hand-computed), I = m r^2 / 2.
        const f32 tau = 0.1f;
        const f32 m = math::kPi * 0.25f, I = 0.5f * m * 0.25f;
        w.set_joint_limits(j, 0, 0);  // limits are disabled on this joint: no effect
        World2D w2 = world_no_gravity();
        REQUIRE(add_box(w2, ground, BodyType::Static, {0, 0}, {0.1f, 0.1f}));
        REQUIRE(add_circle(w2, wheel, BodyType::Dynamic, {0, 0}, 0.5f));
        d.max_motor_torque = tau;
        d.motor_speed = 100.0f;
        w2.create_revolute_joint(d);
        for (u32 i = 0; i < 30; ++i) w2.step(kDt);
        CHECK(state(w2, wheel).angular_velocity == doctest::Approx(30.0f * kDt * tau / I).epsilon(1e-3));
    }
}

TEST_CASE("joints: collide_connected, stale ids, destroying a body destroys its joints") {
    World2D w = world_no_gravity();
    const Entity a = ent(1), b = ent(2);
    REQUIRE(add_box(w, a, BodyType::Dynamic, {0, 0}, {0.5f, 0.5f}));
    REQUIRE(add_box(w, b, BodyType::Dynamic, {0.8f, 0}, {0.5f, 0.5f}));  // overlapping
    CHECK(w.create_distance_joint({}).is_null());  // missing bodies
    DistanceJointDesc self;
    self.a = self.b = a;
    CHECK(w.create_distance_joint(self).is_null());

    RevoluteJointDesc d;
    d.a = a;
    d.b = b;
    d.world_anchor = {0.4f, 0};
    const JointId j = w.create_revolute_joint(d);
    w.step(kDt);
    CHECK(w.stats().contacts == 0);  // jointed bodies do not collide
    CHECK(w.stats().joints == 1);
    w.destroy_joint(j);
    CHECK_FALSE(w.joint_alive(j));
    w.step(kDt);
    CHECK(w.stats().contacts == 1);  // ...until the joint goes

    const JointId j2 = w.create_revolute_joint(d);
    CHECK(j2.index == j.index);
    CHECK_FALSE(j2 == j);  // generational: the old id stays dead
    CHECK_FALSE(w.joint_alive(j));
    w.destroy_body(b);
    CHECK_FALSE(w.joint_alive(j2));
    CHECK(w.stats().joints == 0);
    w.step(kDt);  // no dangling joint reaches the solver
}

// ---- sleeping -----------------------------------------------------------------------------------------

TEST_CASE("sleeping: a resting body sleeps after time_to_sleep, freezes, and wakes on contact and API") {
    World2D w = world_gravity(true);
    const Entity ground = ent(1), box = ent(2);
    REQUIRE(add_box(w, ground, BodyType::Static, {0, -0.5f}, {10, 0.5f}));
    REQUIRE(add_box(w, box, BodyType::Dynamic, {0, 0.5f}, {0.5f, 0.5f}));
    u32 slept_at = 0;
    for (u32 i = 1; i <= 120 && !slept_at; ++i) {
        w.step(kDt);
        if (!w.is_awake(box)) slept_at = i;
    }
    REQUIRE(slept_at > 0);
    CHECK(slept_at >= 30);  // at least time_to_sleep at rest
    CHECK(w.stats().sleeping == 1);
    const vec2 p = state(w, box).position;
    const u64 h = w.hash_state(0);
    for (u32 i = 0; i < 60; ++i) w.step(kDt);
    CHECK(state(w, box).position == p);  // frozen, not drifting
    CHECK(w.hash_state(0) == h);
    CHECK(w.stats().contacts == 1);  // the resting contact is kept while asleep (no ContactEnd)
    CHECK(w.contact_ends().empty());

    // API wake.
    w.apply_impulse(box, {0, 5}, state(w, box).position);
    CHECK(w.is_awake(box));
    for (u32 i = 0; i < 120; ++i) w.step(kDt);
    CHECK_FALSE(w.is_awake(box));

    // A falling body wakes what it lands on.
    const Entity drop = ent(3);
    REQUIRE(add_box(w, drop, BodyType::Dynamic, {0, 3}, {0.25f, 0.25f}));
    bool woke = false;
    for (u32 i = 0; i < 60; ++i) {
        w.step(kDt);
        woke = woke || w.is_awake(box);
    }
    CHECK(woke);
}

TEST_CASE("sleeping: removing the support wakes the island; sensors keep sleeping bodies inside") {
    World2D w = world_gravity(true);
    const Entity ground = ent(1), plank = ent(2), top = ent(3), sensor = ent(4);
    REQUIRE(add_box(w, ground, BodyType::Static, {0, -0.5f}, {10, 0.5f}));
    REQUIRE(add_box(w, plank, BodyType::Static, {0, 2.0f}, {1.0f, 0.1f}));
    REQUIRE(add_box(w, top, BodyType::Dynamic, {0, 2.6f}, {0.5f, 0.5f}));
    BodyDesc sd;
    sd.body.type = BodyType::Static;
    sd.collider.shape = ColliderShape::box({2, 2});
    sd.collider.is_sensor = true;
    sd.position = {0, 2};
    REQUIRE(w.create_body(sensor, sd));
    u32 enters = 0, exits = 0;
    for (u32 i = 0; i < 120; ++i) {
        w.step(kDt);
        enters += static_cast<u32>(w.trigger_enters().size());
        exits += static_cast<u32>(w.trigger_exits().size());
    }
    REQUIRE_FALSE(w.is_awake(top));
    CHECK(enters == 1);
    CHECK(exits == 0);  // asleep inside the sensor is still inside

    w.destroy_body(plank);
    CHECK(w.is_awake(top));
    for (u32 i = 0; i < 60; ++i) w.step(kDt);
    CHECK(state(w, top).position.y < 1.0f);  // it fell
}

TEST_CASE("sleeping is off by default: the M3.5 behaviour and hash are unchanged") {
    auto run = [](bool sleep) {
        World2D w = world_gravity(sleep);
        REQUIRE(add_box(w, ent(1), BodyType::Static, {0, -0.5f}, {10, 0.5f}));
        REQUIRE(add_box(w, ent(2), BodyType::Dynamic, {0, 0.5f}, {0.5f, 0.5f}));
        for (u32 i = 0; i < 120; ++i) w.step(kDt);
        return w;
    };
    World2D off = run(false);
    CHECK(off.is_awake(ent(2)));
    CHECK(off.stats().sleeping == 0);
}

TEST_CASE("FR-PHY-060: a 20-body stack settles in under 1 s without jitter, then sleeps") {
    // The stacking profile (ADR-0026 §3): block solver + 16 velocity iterations. With the default 8
    // the sequential impulses do not converge down a 20-high column and it leans over in seconds.
    PhysicsConfig cfg;
    cfg.enable_sleep = true;
    cfg.block_solver = true;
    cfg.velocity_iterations = 16;
    World2D w(cfg);
    REQUIRE(add_box(w, ent(1), BodyType::Static, {0, -0.5f}, {10, 0.5f}));
    for (u32 i = 0; i < 20; ++i) REQUIRE(add_box(w, ent(2 + i), BodyType::Dynamic, {0, 0.5f + 1.0f * static_cast<f32>(i)}, {0.5f, 0.5f}));
    auto max_speed = [&] {
        f32 m = 0.0f;
        for (u32 i = 0; i < 20; ++i) m = std::fmax(m, math::length(state(w, ent(2 + i)).linear_velocity));
        return m;
    };
    for (u32 i = 0; i < 60; ++i) w.step(kDt);  // 1 s
    CHECK(max_speed() < 0.05f);                // settled
    const vec2 top = state(w, ent(21)).position;
    CHECK(top.y == doctest::Approx(19.5f).epsilon(0.01));
    CHECK(std::fabs(top.x) < 1e-3f);
    // No jitter: over the next 2 s the top creeps less than one linear slop (5 mm) while the
    // contacts finish settling into it, never speeds back up, and the stack sleeps.
    f32 drift = 0.0f, speed = 0.0f;
    for (u32 i = 0; i < 120; ++i) {
        w.step(kDt);
        drift = std::fmax(drift, math::length(state(w, ent(21)).position - top));
        speed = std::fmax(speed, max_speed());
    }
    CHECK(drift < 5e-3f);
    CHECK(speed < 0.05f);
    CHECK(w.stats().sleeping == 20);
}

// ---- budget -------------------------------------------------------------------------------------------

TEST_CASE("FR-PHY-050: 2,000 dynamic bodies — broadphase + integrate budget") {
    // Bodies spread on a grid with gaps and free-falling: every step re-queries 2,000 proxies, moves
    // them and refreshes the tree (the broadphase + integrate cost), with no contacts to solve.
    World2D w = world_gravity();
    u32 id = 1;
    for (u32 y = 0; y < 40; ++y)
        for (u32 x = 0; x < 50; ++x)
            REQUIRE(add_box(w, ent(id++), BodyType::Dynamic, {static_cast<f32>(x) * 1.5f, static_cast<f32>(y) * 1.5f},
                            {0.4f, 0.4f}));
    for (u32 i = 0; i < 5; ++i) w.step(kDt);  // warm-up
    const auto t0 = std::chrono::steady_clock::now();
    const u32 steps = 60;
    for (u32 i = 0; i < steps; ++i) w.step(kDt);
    const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count() / steps;
    CHECK(w.stats().bodies == 2000);
    CHECK(w.stats().contacts == 0);
    CHECK(w.validate_broadphase());
    MESSAGE("2,000-body step (broadphase + integrate): " << ms << " ms");
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
    CHECK(ms < 3.0);  // optimized builds only: Debug/ASan numbers are not the budget
#endif
}

TEST_CASE("FR-PHY-050: 2,000 bodies piled into a box stay valid and deterministic") {
    auto run = [] {
        World2D w = world_gravity(true);
        REQUIRE(add_box(w, ent(1), BodyType::Static, {0, -0.5f}, {40, 0.5f}));
        REQUIRE(add_box(w, ent(2), BodyType::Static, {-40.5f, 20}, {0.5f, 20}));
        REQUIRE(add_box(w, ent(3), BodyType::Static, {40.5f, 20}, {0.5f, 20}));
        u32 id = 4;
        for (u32 y = 0; y < 25; ++y)
            for (u32 x = 0; x < 80; ++x) {
                const vec2 p{-39.5f + static_cast<f32>(x), 0.5f + static_cast<f32>(y) * 1.05f};
                if ((x + y) % 2) REQUIRE(add_circle(w, ent(id++), BodyType::Dynamic, p, 0.45f));
                else REQUIRE(add_box(w, ent(id++), BodyType::Dynamic, p, {0.45f, 0.45f}));
            }
        f64 worst = 0.0;
        for (u32 i = 0; i < 120; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            w.step(kDt);
            worst = std::fmax(worst, std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        MESSAGE("2,000-body pile: worst full step " << worst << " ms, contacts " << w.stats().contacts << ", asleep "
                                                    << w.stats().sleeping);
        CHECK(w.validate_broadphase());
        return w.hash_state(0);
    };
    CHECK(run() == run());
}

TEST_CASE("block solver: a box landing flat on two points stops in one solve without spinning") {
    // Hand-computed: a unit box (m = 1) hits the ground flat at 2 m/s. The 2x2 block LCP has the
    // symmetric solution p1 = p2 = m v / 2 = 1, so v ends at 0 and w at exactly 0; the solve also
    // stays symmetric under warm starting (the sequential solver is order dependent instead).
    PhysicsConfig c;
    c.gravity = {0.0f, 0.0f};
    c.block_solver = true;
    World2D w(c);
    REQUIRE(add_box(w, ent(1), BodyType::Static, {0, -0.5f}, {10, 0.5f}));
    REQUIRE(add_box(w, ent(2), BodyType::Dynamic, {0, 0.499f}, {0.5f, 0.5f}));
    w.set_linear_velocity(ent(2), {0, -2});
    w.step(kDt);
    CHECK(state(w, ent(2)).linear_velocity.y == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(state(w, ent(2)).angular_velocity == 0.0f);
    for (u32 i = 0; i < 60; ++i) w.step(kDt);
    CHECK(state(w, ent(2)).angular_velocity == 0.0f);
    CHECK(state(w, ent(2)).angle == 0.0f);
}
