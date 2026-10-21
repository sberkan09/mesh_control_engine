// Box2D v3 behavior oracle (physics.md §6 test 14, D9; ADR-0016). Identical deterministic scenes
// are stepped in mce_physics2d and in Box2D v3, and the outcomes compared within the tolerances
// documented per scene. The oracle validates the in-house solver; it does not replace the
// hand-computed cases in test_collision.cpp / test_world.cpp.
//
// The two solvers are different algorithms on purpose (ours: Box2D 2.x sequential impulse with
// NGS position correction; Box2D v3: soft step with 4 substeps and relax/restitution passes), so
// tolerances describe agreement of behavior, not of bits. This is the only file in the repository
// allowed to name Box2D symbols (the Box2D wall in scripts/ci/check_walls.py).

#include <box2d/box2d.h>

#include <cmath>

#include "../helpers.h"

using namespace phys_test;

namespace {

constexpr int kSubsteps = 4;  // Box2D v3's recommended default

struct Spec {
    BodyType type = BodyType::Dynamic;
    bool circle = false;
    vec2 half{0.5f, 0.5f};
    f32 radius = 0.5f;
    vec2 pos;
    vec2 vel;
    f32 friction = 0.6f;
    f32 restitution = 0.0f;
    bool fixed_rotation = false;
};

struct Pose {
    vec2 p;
    f32 angle = 0.0f;
    vec2 v;
};

// The same scene in both engines; kinematic bodies are driven by `drive(tick, index) -> velocity`.
class Twin {
public:
    explicit Twin(vec2 gravity = {0.0f, -9.81f}) : mce_(make_config(gravity)) {
        b2WorldDef def = b2DefaultWorldDef();
        def.gravity = {gravity.x, gravity.y};
        def.enableSleep = false;  // compare live velocities, not sleep thresholds
        b2_ = b2CreateWorld(&def);
    }
    ~Twin() { b2DestroyWorld(b2_); }
    Twin(const Twin&) = delete;
    Twin& operator=(const Twin&) = delete;

    u32 add(const Spec& s) {
        const u32 i = count_++;
        REQUIRE(i < kMax);
        specs_[i] = s;

        BodyDesc d;
        d.body.type = s.type;
        d.body.fixed_rotation = s.fixed_rotation;
        d.collider.shape = s.circle ? ColliderShape::circle(s.radius) : ColliderShape::box(s.half);
        d.collider.friction = s.friction;
        d.collider.restitution = s.restitution;
        d.position = s.pos;
        REQUIRE(mce_.create_body(ent(i), d));
        mce_.set_linear_velocity(ent(i), s.vel);

        b2BodyDef bd = b2DefaultBodyDef();
        bd.type = s.type == BodyType::Static ? b2_staticBody
                  : s.type == BodyType::Kinematic ? b2_kinematicBody
                                                  : b2_dynamicBody;
        bd.position = {s.pos.x, s.pos.y};
        bd.linearVelocity = {s.vel.x, s.vel.y};
        bd.fixedRotation = s.fixed_rotation;
        ids_[i] = b2CreateBody(b2_, &bd);
        b2ShapeDef sd = b2DefaultShapeDef();
        sd.density = 1.0f;
        sd.material.friction = s.friction;
        sd.material.restitution = s.restitution;
        if (s.circle) {
            const b2Circle c{{0.0f, 0.0f}, s.radius};
            b2CreateCircleShape(ids_[i], &sd, &c);
        } else {
            const b2Polygon box = b2MakeBox(s.half.x, s.half.y);
            b2CreatePolygonShape(ids_[i], &sd, &box);
        }
        return i;
    }

    void set_velocity(u32 i, vec2 v) {
        mce_.set_linear_velocity(ent(i), v);
        b2Body_SetLinearVelocity(ids_[i], {v.x, v.y});
    }

    void step() {
        mce_.step(kDt);
        b2World_Step(b2_, kDt, kSubsteps);
    }

    Pose ours(u32 i) const {
        const BodyState s = state(mce_, ent(i));
        return {s.position, s.angle, s.linear_velocity};
    }
    Pose oracle(u32 i) const {
        const b2Vec2 p = b2Body_GetPosition(ids_[i]);
        const b2Vec2 v = b2Body_GetLinearVelocity(ids_[i]);
        return {{p.x, p.y}, b2Rot_GetAngle(b2Body_GetRotation(ids_[i])), {v.x, v.y}};
    }

private:
    static constexpr u32 kMax = 16;
    static PhysicsConfig make_config(vec2 g) {
        PhysicsConfig c;
        c.gravity = g;
        return c;
    }
    World2D mce_;
    b2WorldId b2_{};
    b2BodyId ids_[kMax]{};
    Spec specs_[kMax];
    u32 count_ = 0;
};

Spec ground(f32 half_width = 20.0f) {
    Spec s;
    s.type = BodyType::Static;
    s.half = {half_width, 0.5f};
    s.pos = {0.0f, 0.0f};  // top face at y = 0.5
    return s;
}

f32 dist(vec2 a, vec2 b) { return math::length(a - b); }

}  // namespace

TEST_CASE("oracle: drop — a box falls, lands and rests at the same height as Box2D v3") {
    Twin t;
    t.add(ground());
    Spec box;
    box.pos = {0.0f, 4.0f};
    const u32 b = t.add(box);

    // Free fall (first 0.5 s, before contact): semi-implicit Euler with 1 step vs 4 substeps per
    // tick lands lower by exactly 3/8 * g * dt * t (0.0307 m at t = 0.5 s); velocities agree.
    for (int i = 0; i < 30; ++i) t.step();
    CHECK(std::fabs(t.oracle(b).p.y - t.ours(b).p.y - 0.375f * 9.81f * kDt * 0.5f) < 1e-3f);
    CHECK(dist(t.ours(b).v, t.oracle(b).v) < 1e-3f);

    for (int i = 0; i < 150; ++i) t.step();
    const Pose o = t.ours(b), r = t.oracle(b);
    // Resting height: 1.0 minus each solver's allowed penetration (linear slop 0.005 m).
    CHECK(std::fabs(o.p.y - r.p.y) < 0.01f);
    CHECK(std::fabs(o.p.y - 1.0f) < 0.01f);
    CHECK(std::fabs(o.p.x - r.p.x) < 1e-3f);
    CHECK(std::fabs(o.angle - r.angle) < 1e-3f);
    CHECK(math::length(o.v) < 0.01f);
    CHECK(math::length(r.v) < 0.01f);
}

TEST_CASE("oracle: slide — friction stops a sliding box within 5% of Box2D v3 and of v^2 / (2 mu g)") {
    Twin t;
    Spec g = ground(40.0f);
    g.friction = 0.5f;
    t.add(g);
    Spec box;
    box.pos = {-20.0f, 1.0f - 0.005f};  // resting depth: no landing transient
    box.vel = {5.0f, 0.0f};
    box.friction = 0.5f;
    box.fixed_rotation = true;  // pure slide: no tipping torque from the friction couple
    const u32 b = t.add(box);
    for (int i = 0; i < 180; ++i) t.step();

    const f32 ours = t.ours(b).p.x - box.pos.x;
    const f32 oracle = t.oracle(b).p.x - box.pos.x;
    const f32 analytic = 25.0f / (2.0f * 0.5f * 9.81f);  // 2.548 m
    MESSAGE("slide distance: ours " << ours << ", Box2D " << oracle << ", analytic " << analytic);
    CHECK(std::fabs(ours - oracle) < 0.05f * analytic);
    CHECK(std::fabs(ours - analytic) < 0.05f * analytic);
    CHECK(std::fabs(t.ours(b).v.x) < 1e-3f);
    CHECK(std::fabs(t.oracle(b).v.x) < 1e-3f);
}

TEST_CASE("oracle: restitution — rebound heights agree with Box2D v3 within 10%") {
    for (const f32 e : {0.3f, 0.6f, 0.9f}) {
        CAPTURE(e);
        Twin t;
        t.add(ground());
        Spec ball;
        ball.circle = true;
        ball.radius = 0.5f;
        ball.pos = {0.0f, 6.0f};
        ball.restitution = e;
        const u32 b = t.add(ball);

        // Track the apex after the first bounce in both engines.
        f32 apex_ours = 0.0f, apex_oracle = 0.0f;
        bool bounced_ours = false, bounced_oracle = false;
        for (int i = 0; i < 240; ++i) {
            t.step();
            const Pose o = t.ours(b), r = t.oracle(b);
            bounced_ours = bounced_ours || o.v.y > 0.0f;
            bounced_oracle = bounced_oracle || r.v.y > 0.0f;
            if (bounced_ours) apex_ours = math::max(apex_ours, o.p.y);
            if (bounced_oracle) apex_oracle = math::max(apex_oracle, r.p.y);
            if (bounced_ours && o.v.y < 0.0f && bounced_oracle && r.v.y < 0.0f) break;
        }
        REQUIRE(bounced_ours);
        REQUIRE(bounced_oracle);
        // Rebound height above the resting centre (y = 1.0): ideal is e^2 * 5 m.
        const f32 h_ours = apex_ours - 1.0f, h_oracle = apex_oracle - 1.0f;
        MESSAGE("e " << e << ": rebound ours " << h_ours << ", Box2D " << h_oracle << ", ideal " << e * e * 5.0f);
        CHECK(std::fabs(h_ours - h_oracle) < 0.1f * h_oracle + 0.02f);
    }
}

TEST_CASE("oracle: pyramid-lite — a 2-1 pyramid and a 3-box stack settle where Box2D v3 puts them") {
    Twin t;
    t.add(ground());
    // Pyramid: two boxes on the ground with a small gap, one on top across them.
    Spec box;
    box.pos = {-0.55f, 1.0f};
    const u32 l = t.add(box);
    box.pos = {0.55f, 1.0f};
    const u32 r = t.add(box);
    box.pos = {0.0f, 2.0f};
    const u32 top = t.add(box);
    // Stack: three boxes, slightly offset so the solver has to hold friction, not just normals.
    u32 stack[3];
    for (u32 i = 0; i < 3; ++i) {
        box.pos = {5.0f + 0.05f * static_cast<f32>(i), 1.0f + static_cast<f32>(i)};
        stack[i] = t.add(box);
    }

    for (int i = 0; i < 300; ++i) t.step();
    for (const u32 i : {l, r, top, stack[0], stack[1], stack[2]}) {
        CAPTURE(i);
        const Pose o = t.ours(i), q = t.oracle(i);
        CHECK(dist(o.p, q.p) < 0.02f);
        CHECK(std::fabs(o.angle - q.angle) < 0.02f);
        CHECK(math::length(o.v) < 0.02f);
        CHECK(math::length(q.v) < 0.02f);
    }
    // Nothing toppled: the top box still sits on the two below, the stack is still a stack.
    CHECK(std::fabs(t.ours(top).p.y - 2.0f) < 0.02f);
    CHECK(std::fabs(t.ours(stack[2]).p.y - 3.0f) < 0.03f);
}

TEST_CASE("oracle: platform ride — a crate on a moving kinematic platform is carried like Box2D v3") {
    Twin t;
    Spec plat;
    plat.type = BodyType::Kinematic;
    plat.half = {2.0f, 0.25f};
    plat.pos = {0.0f, 0.0f};
    const u32 p = t.add(plat);
    Spec crate;
    crate.pos = {0.0f, 0.75f - 0.005f};
    const u32 c = t.add(crate);

    // Ramp up along x, follow one lap of a circle of radius 1.5 m (the "rotating-ish" path of the
    // M3.5 demo), ramp down and stop. Every acceleration stays below mu * g so a non-slipping
    // crate is the right answer. Velocities are set identically in both engines every tick.
    const f32 radius = 1.5f, omega = math::kTau / 4.0f, speed = radius * omega;
    for (int i = 0; i < 540; ++i) {
        vec2 v;
        if (i < 120) v = {speed * static_cast<f32>(i) / 120.0f, 0.0f};
        else if (i < 360) {
            const f32 theta = omega * static_cast<f32>(i - 120) * kDt;
            v = {speed * std::cos(theta), speed * std::sin(theta)};
        } else if (i < 480) v = {speed * static_cast<f32>(480 - i) / 120.0f, 0.0f};
        t.set_velocity(p, v);
        t.step();
    }
    const vec2 off_ours = t.ours(c).p - t.ours(p).p;
    const vec2 off_oracle = t.oracle(c).p - t.oracle(p).p;
    MESSAGE("crate offset: ours (" << off_ours.x << ", " << off_ours.y << "), Box2D (" << off_oracle.x << ", "
                                   << off_oracle.y << ")");
    CHECK(dist(t.ours(p).p, t.oracle(p).p) < 1e-3f);  // same platform path in both
    CHECK(dist(off_ours, off_oracle) < 0.05f);
    CHECK(std::fabs(off_ours.x) < 0.05f);  // no slip relative to the platform
}
