// Headless 2D physics scene for the determinism harness (FR-RUN-040, physics.md §6 test 8/9).
// Not a ladder sample: 03_platformer is M3's demo; this is the physics-only slice it builds on.
//
// An arena with a ramp, a one-way ledge, a kinematic lift and a sensor "drain", a spawner
// raining boxes/circles/convex shards, and a patrolling, jumping character controller. Prints the
// combined ECS + physics state hash after N ticks.
//
//   sample_physics2d_headless -ticks 1200

#include <chrono>
#include <cmath>
#include <cstdio>

#include "mce/core.h"
#include "mce/ecs/transform.h"
#include "mce/physics/joint2d.h"
#include "mce/physics/module.h"

using namespace mce;
using namespace mce::physics;
using math::vec2;

namespace {

MCE_CVAR(cv_ticks, "ticks", 1200, "Ticks to simulate", CVarFlags::None);

struct Rng {
    u64 s = 0x9E3779B97F4A7C15ull;
    f32 next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<f32>((s >> 33) % 100000) / 100000.0f;
    }
};

struct Player {
    u32 ticks = 0;
    f32 vy = 0.0f;
};
struct Lift {
    f32 phase = 0.0f;
};

LocalTransform at(vec2 p, f32 angle = 0.0f) {
    LocalTransform t;
    t.position = {p.x, p.y, 0.0f};
    t.rotation = rotation_z(angle);
    return t;
}
RigidBody2D body(BodyType type) {
    RigidBody2D rb;
    rb.type = type;
    return rb;
}
Collider2D collider(const ColliderShape& s, bool sensor = false, bool one_way = false) {
    Collider2D c;
    c.shape = s;
    c.is_sensor = sensor;
    c.one_way = one_way;
    return c;
}

void spawner(Frame& f) {
    if (f.tick % 6 != 0) return;
    Rng& r = f.resource<Rng>();
    Collider2D c;
    c.restitution = r.next() * 0.4f;
    c.friction = 0.2f + r.next() * 0.6f;
    const f32 k = r.next();
    if (k < 0.4f) {
        c.shape = ColliderShape::circle(0.15f + r.next() * 0.25f);
    } else if (k < 0.8f) {
        c.shape = ColliderShape::box({0.15f + r.next() * 0.3f, 0.15f + r.next() * 0.3f});
    } else {
        const vec2 pts[] = {{0, 0}, {0.6f, 0.1f}, {0.4f, 0.5f}, {r.next() * 0.2f, 0.4f}};
        c.shape = ColliderShape::convex(pts);
    }
    f.commands().spawn(body(BodyType::Dynamic), c, at({r.next() * 16.0f - 8.0f, 12.0f + r.next() * 3.0f}, r.next() * 6.0f));
}

void lift(Frame& f) {
    World2D& pw = f.resource<World2D>();
    f.query<Lift&>().each([&](Entity e, Lift& l) {
        l.phase += f.dt;
        pw.set_linear_velocity(e, {0.0f, 1.5f * std::sin(l.phase)});
    });
}

void player(Frame& f) {
    World2D& pw = f.resource<World2D>();
    f.query<Player&, const CharacterGround2D&>().each([&](Entity e, Player& p, const CharacterGround2D& g) {
        ++p.ticks;
        const bool grounded = (g.flags & ControllerFlags::Ground) != 0;
        if (grounded) p.vy = (p.ticks % 90 == 0) ? 7.0f : 0.0f;
        p.vy -= 20.0f * f.dt;
        const f32 vx = ((p.ticks / 240) % 2 == 0) ? 3.0f : -3.0f;
        const MoveResult2D r = pw.move_and_collide(e, {vx * f.dt, p.vy * f.dt});
        if (r.hit_ceiling && p.vy > 0.0f) p.vy = 0.0f;
    });
}

void drain(Frame& f) {
    for (const TriggerEnter& ev : f.events().read<TriggerEnter>())
        if (!f.world().has<Player>(ev.other)) f.commands().destroy(ev.other);
}

f64 now_ms() {
    using namespace std::chrono;
    return duration<f64, std::milli>(steady_clock::now().time_since_epoch()).count();
}

class PhysicsApp final : public app::Application {
public:
    World world;
    f64 tick_ms_total = 0;
    u64 ticks = 0;

    ResultV on_init(app::Context&) override {
        install_physics2d(world);
        world.set_resource(Rng{});
        world.add_system(Phase::FixedUpdate_Pre, "spawner", &spawner);
        world.add_system(Phase::FixedUpdate_Pre, "lift", &lift, "spawner");
        world.add_system(Phase::FixedUpdate_Pre, "player", &player, "lift");
        world.add_system(Phase::FixedUpdate_Post, "drain", &drain);

        world.spawn_with(body(BodyType::Static), collider(ColliderShape::box({12, 0.5f})), at({0, -0.5f}));
        world.spawn_with(body(BodyType::Static), collider(ColliderShape::box({0.5f, 10})), at({-12.5f, 9.5f}));
        world.spawn_with(body(BodyType::Static), collider(ColliderShape::box({0.5f, 10})), at({12.5f, 9.5f}));
        const vec2 ramp[] = {{-11, 0}, {-5, 0}, {-11, 3}};
        world.spawn_with(body(BodyType::Static), collider(ColliderShape::convex(ramp)), at({0, 0}));
        world.spawn_with(body(BodyType::Static), collider(ColliderShape::box({2, 0.1f}), false, true), at({4, 3}));
        world.spawn_with(body(BodyType::Kinematic), collider(ColliderShape::box({1.5f, 0.15f})), Lift{}, at({8, 1}));
        world.spawn_with(body(BodyType::Static), collider(ColliderShape::box({1.5f, 0.3f}), true), at({0, 0.3f}));
        world.spawn_with(body(BodyType::Kinematic), collider(ColliderShape::box({0.3f, 0.6f})), CharacterController2D{},
                         CharacterGround2D{}, Player{}, at({-2, 1}));
        build_joints();
        return ok();
    }

    // A plank bridge hinged between two posts and a door hanging from a beam, all Joint2D components.
    void build_joints() {
        const f32 y = 6.0f;
        const Entity post_l = world.spawn_with(body(BodyType::Static), collider(ColliderShape::box({0.5f, 0.5f})), at({-7.5f, y}));
        const Entity post_r = world.spawn_with(body(BodyType::Static), collider(ColliderShape::box({0.5f, 0.5f})), at({-0.5f, y}));
        Entity prev = post_l;
        for (int i = 0; i < 6; ++i) {
            Joint2D j;
            j.connected = prev;
            j.anchor = {-0.5f, 0.0f};  // the plank's left end: the pivot is placed from this body's pose
            prev = world.spawn_with(body(BodyType::Dynamic), collider(ColliderShape::box({0.48f, 0.08f})),
                                    at({-7.0f + 0.5f + static_cast<f32>(i), y}), j);
        }
        Joint2D end;  // on the right post: its left edge is the last plank's right end
        end.connected = prev;
        end.anchor = {-0.5f, 0.0f};
        world.add(post_r, end);

        const Entity beam = world.spawn_with(body(BodyType::Static), collider(ColliderShape::box({0.3f, 0.2f})), at({6.0f, 9.0f}));
        Joint2D hinge;
        hinge.connected = beam;
        hinge.anchor = {0.0f, 1.0f};  // the top of the door
        hinge.limits = 1;
        hinge.lower = -60.0f;
        hinge.upper = 60.0f;
        world.spawn_with(body(BodyType::Dynamic), collider(ColliderShape::box({0.1f, 1.0f})), at({6.4f, 8.0f}, 0.4f), hinge);
    }

    void on_tick(const app::TickContext& t) override {
        const f64 t0 = now_ms();
        world.tick(t.dt, t.tick_index);
        tick_ms_total += now_ms() - t0;
        ++ticks;
    }
};

}  // namespace

int main(int argc, char** argv) {
    log::init({.app_name = "physics2d_headless", .level = log::Level::Info});
    console::parse_command_line(argc, argv);

    PhysicsApp sample;
    app::AppDesc desc;
    desc.name = "physics2d_headless";
    desc.headless = true;
    desc.max_ticks = static_cast<u64>(cv_ticks.get_int());
    const int rc = app::run(sample, desc);

    const World2D& pw = sample.world.resource<World2D>();
    const World2D::Stats st = pw.stats();
    const u64 hash = hash_combine(sample.world.state_hash(), pw.hash_state(kFnv64Offset));
    const f64 ticks = sample.ticks ? static_cast<f64>(sample.ticks) : 1.0;
    std::printf("physics2d_headless: %llu ticks | %u bodies, %u joints, %u contacts | tick %.3f ms | hash %016llx\n",
                static_cast<unsigned long long>(sample.ticks), st.bodies, st.joints, st.contacts, sample.tick_ms_total / ticks,
                static_cast<unsigned long long>(hash));
    log::shutdown();
    return rc;
}
