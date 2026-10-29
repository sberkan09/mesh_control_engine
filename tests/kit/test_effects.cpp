// Events and effects as components (M-DATA-REACH 4): the triggers, camera shake and follow,
// PhysicsEvents and the NavGrid the stock systems build.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/convar.h"
#include "mce/audio/components.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/effects.h"
#include "mce/kit/particles.h"
#include "mce/kit/pathfinding.h"
#include "mce/kit/sprite_extract.h"
#include "mce/kit/vehicles3d.h"
#include "mce/input/actions.h"
#include "mce/physics/module.h"
#include "mce/physics/world2d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z = 0.0f) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

u64 sfx_pushed(World& w) { return w.event_queue<audio::PlaySfx>()->total_pushed; }

}  // namespace

TEST_CASE("effects: triggers fire on a sensor entry, on Start and on the fire flag; PhysicsEvents records") {
    World w;
    w.register_components<LocalTransform>();
    physics::install_physics2d(w);
    w.register_event<audio::PlaySfx>(Phase::FixedUpdate_Pre);  // what audio::install registers
    install_particles(w, nullptr);
    install_effects(w, nullptr);

    physics::RigidBody2D fixed;  // static
    physics::Collider2D sensor;
    sensor.shape = physics::ColliderShape::box({2.0f, 0.5f});
    sensor.is_sensor = true;
    SfxTrigger sfx;
    sfx.clip.guid = asset::Guid{1, 2};
    sfx.on = FireOn::TriggerEnter;
    sfx.gain_jitter_db = 3.0f;
    ParticleBurst burst;
    burst.on = FireOn::TriggerEnter;
    const Entity zone = w.spawn_with(at(0, 0), fixed, sensor, sfx, burst, PhysicsEvents{});

    physics::RigidBody2D dyn;
    dyn.type = physics::BodyType::Dynamic;
    physics::Collider2D ball;
    ball.shape = physics::ColliderShape::circle(0.25f);
    const Entity falling = w.spawn_with(at(0, 3), dyn, ball, PhysicsEvents{});

    SfxTrigger start;
    start.clip.guid = asset::Guid{3, 4};
    start.on = FireOn::Start;
    SfxTrigger manual;
    manual.clip.guid = asset::Guid{5, 6};
    const Entity starter = w.spawn_with(start);
    const Entity button = w.spawn_with(manual);

    u64 t = 0;
    w.tick(kDt, t++);
    CHECK(w.try_get<SfxTrigger>(starter)->fired == 1);
    CHECK(w.try_get<SfxTrigger>(button)->fired == 0);
    for (; t < 120 && w.try_get<SfxTrigger>(zone)->fired == 0; ++t) w.tick(kDt, t);
    REQUIRE(w.try_get<SfxTrigger>(zone)->fired == 1);
    CHECK(w.try_get<ParticleBurst>(zone)->fired == 1);
    const PhysicsEvents& pe = *w.try_get<PhysicsEvents>(zone);
    CHECK(pe.trigger_enters == 1);
    CHECK(pe.overlapping == 1);
    CHECK(pe.last_trigger == falling);
    CHECK(w.try_get<PhysicsEvents>(falling)->last_trigger == zone);
    CHECK(w.try_get<SfxTrigger>(starter)->fired == 1);  // Start fires once

    w.try_get_mut<SfxTrigger>(button)->fire = true;
    const u64 before = sfx_pushed(w);
    w.tick(kDt, t++);
    CHECK(w.try_get<SfxTrigger>(button)->fired == 1);
    CHECK_FALSE(w.try_get<SfxTrigger>(button)->fire);  // cleared by the engine
    CHECK(sfx_pushed(w) == before + 1);
    CHECK(sfx_pushed(w) == 3);  // Start, the sensor entry and the button

    // The ball falls through the sensor and lands nowhere: the exit is recorded.
    for (u32 i = 0; i < 120; ++i) w.tick(kDt, t++);
    CHECK(w.try_get<PhysicsEvents>(zone)->trigger_exits == 1);
    CHECK(w.try_get<PhysicsEvents>(zone)->overlapping == 0);
    CHECK(w.try_get<SfxTrigger>(zone)->fired == 1);
}

TEST_CASE("effects: ShakeTrigger shakes the primary camera view, and it decays") {
    World w;
    w.register_components<LocalTransform>();
    install_effects(w, nullptr);
    w.register_components<Camera2DComponent>();
    const Entity cam = w.spawn_with(at(2, 3), Camera2DComponent{}, CameraShake2D{});
    ShakeTrigger s;
    s.trauma = 1.0f;
    const Entity hit = w.spawn_with(s);

    u64 t = 0;
    w.tick(kDt, t++);
    render::Camera2D view;
    f32 ppm = 0.0f;
    REQUIRE(primary_camera(w, view, ppm));
    CHECK(view.position.x == doctest::Approx(2.0f));

    w.try_get_mut<ShakeTrigger>(hit)->fire = true;
    w.tick(kDt, t++);
    w.tick(kDt, t++);
    const CameraShake2D& shake = *w.try_get<CameraShake2D>(cam);
    CHECK(shake.trauma > 0.9f);
    CHECK(math::length(shake.offset) > 0.0f);
    REQUIRE(primary_camera(w, view, ppm));
    CHECK(view.position.x == doctest::Approx(2.0f + shake.offset.x));
    CHECK(view.position.y == doctest::Approx(3.0f + shake.offset.y));

    for (u32 i = 0; i < 90; ++i) w.tick(kDt, t++);  // 1.5 s at 1.5/s decay
    CHECK(w.try_get<CameraShake2D>(cam)->trauma == 0.0f);
    REQUIRE(primary_camera(w, view, ppm));
    CHECK(view.position.x == doctest::Approx(2.0f));
}

TEST_CASE("effects: CameraFollow2D snaps, keeps a dead zone, smooths and stays in bounds") {
    World w;
    w.register_components<LocalTransform>();
    install_effects(w, nullptr);
    const Entity target = w.spawn_with(at(0, 0));
    CameraFollow2D f;
    f.target = target;
    f.offset = {0.0f, 1.0f};
    f.dead_zone = {1.0f, 0.0f};
    f.smoothing = 0.0f;
    const Entity cam = w.spawn_with(at(10, 10), f);

    u64 t = 0;
    w.tick(kDt, t++);
    CHECK(w.try_get<LocalTransform>(cam)->position.x == doctest::Approx(0.0f));  // starts on the target
    CHECK(w.try_get<LocalTransform>(cam)->position.y == doctest::Approx(1.0f));

    w.try_get_mut<LocalTransform>(target)->position.x = 0.5f;  // inside the dead zone
    w.tick(kDt, t++);
    CHECK(w.try_get<LocalTransform>(cam)->position.x == doctest::Approx(0.0f));
    w.try_get_mut<LocalTransform>(target)->position.x = 3.0f;  // 2 m past the zone's edge
    w.tick(kDt, t++);
    CHECK(w.try_get<LocalTransform>(cam)->position.x == doctest::Approx(2.0f));

    CameraFollow2D& c = *w.try_get_mut<CameraFollow2D>(cam);
    c.dead_zone = {};
    c.smoothing = 5.0f;
    c.use_bounds = true;
    c.bounds_min = {-5.0f, -5.0f};
    c.bounds_max = {5.0f, 5.0f};
    w.try_get_mut<LocalTransform>(target)->position.x = 4.0f;
    w.tick(kDt, t++);
    const f32 x = w.try_get<LocalTransform>(cam)->position.x;
    CHECK(x > 2.0f);
    CHECK(x < 4.0f);  // eased, not snapped
    w.try_get_mut<LocalTransform>(target)->position.x = 50.0f;
    for (u32 i = 0; i < 120; ++i) w.tick(kDt, t++);
    CHECK(w.try_get<LocalTransform>(cam)->position.x == doctest::Approx(5.0f));
}

TEST_CASE("effects: CameraFollow3D orbits at its distance and looks at the pivot") {
    World w;
    w.register_components<LocalTransform>();
    install_effects(w, nullptr);
    const Entity target = w.spawn_with(at(1, 0, -2));
    CameraFollow3D f;
    f.target = target;
    f.smoothing = 0.0f;
    f.yaw = 0.7f;
    const Entity cam = w.spawn_with(at(0, 0, 0), f);
    w.tick(kDt, 0);
    const LocalTransform& t = *w.try_get<LocalTransform>(cam);
    const math::vec3 pivot{1.0f, 1.5f, -2.0f};
    CHECK(math::length(t.position - pivot) == doctest::Approx(6.0f));
    CHECK(t.position.y > pivot.y);  // above: pitch 0.35 rad
    const math::vec3 forward = math::rotate(t.rotation, {0.0f, 0.0f, -1.0f});
    const math::vec3 to_pivot = math::normalize(pivot - t.position);
    CHECK(math::dot(forward, to_pivot) == doctest::Approx(1.0f));
}

namespace {

struct ChaseScene {
    World w;
    u64 t = 0;
    Entity target, cam;
    explicit ChaseScene(u8 chase, bool vehicle, f32 vehicle_speed = 8.0f) {
        w.register_components<LocalTransform, Vehicle3D>();
        install_effects(w, nullptr);
        Vehicle3D v;
        v.speed = vehicle_speed;
        target = vehicle ? w.spawn_with(at(0, 0, 0), v) : w.spawn_with(at(0, 0, 0));
        CameraFollow3D f;
        f.target = target;
        f.smoothing = 0.0f;
        f.chase = chase;
        f.orbit_speed = 2.0f;
        cam = w.spawn_with(at(0, 0, 0), f);
        w.tick(kDt, t++);
    }
    // The target moves `speed` m/s along `dir` (unit, flat) for `seconds`.
    void drive(math::vec3 dir, f32 speed, f32 seconds) {
        for (u32 i = 0; i < static_cast<u32>(seconds * 60.0f); ++i) {
            w.try_get_mut<LocalTransform>(target)->position = w.try_get<LocalTransform>(target)->position + dir * (speed * kDt);
            w.tick(kDt, t++);
        }
    }
    f32 yaw() { return w.try_get<CameraFollow3D>(cam)->yaw; }
};

}  // namespace

TEST_CASE("effects: CameraFollow3D chase turns the camera behind a moving target (M-OPENWORLD-POLICE 4)") {
    {  // chase 2: any target; heading +X puts the camera on -X
        ChaseScene s(2, false);
        s.drive({1.0f, 0.0f, 0.0f}, 6.0f, 3.0f);
        CHECK(std::sin(s.yaw()) < -0.99f);  // the camera's back vector is (sin yaw, cos yaw): -X
        CHECK(s.w.try_get<LocalTransform>(s.cam)->position.x < s.w.try_get<LocalTransform>(s.target)->position.x - 3.0f);
        s.drive({0.0f, 0.0f, -1.0f}, 6.0f, 3.0f);  // then it heads -Z: camera on +Z (yaw 0)
        CHECK(std::cos(s.yaw()) > 0.99f);
    }
    {  // off by default
        ChaseScene s(0, false);
        s.drive({1.0f, 0.0f, 0.0f}, 6.0f, 3.0f);
        CHECK(s.yaw() == 0.0f);
    }
    {  // chase 1 follows a Vehicle3D driving forward, not a walker and not a reversing car
        ChaseScene walker(1, false);
        walker.drive({1.0f, 0.0f, 0.0f}, 6.0f, 3.0f);
        CHECK(walker.yaw() == 0.0f);
        ChaseScene reversing(1, true, -6.0f);
        reversing.drive({1.0f, 0.0f, 0.0f}, 6.0f, 3.0f);
        CHECK(reversing.yaw() == 0.0f);
        ChaseScene car(1, true, 8.0f);
        car.drive({1.0f, 0.0f, 0.0f}, 6.0f, 3.0f);
        CHECK(std::sin(car.yaw()) < -0.99f);
    }
    {  // below chase_min_speed (3 m/s) nothing turns
        ChaseScene s(2, false);
        s.drive({1.0f, 0.0f, 0.0f}, 1.5f, 3.0f);
        CHECK(s.yaw() == 0.0f);
    }
}

TEST_CASE("effects: chase ignores a target swap and speed jitter at the threshold (M-OPENWORLD-POLICE 4)") {
    {  // swapping the target to an entity far away is not a speed: the yaw does not move on the swap
        ChaseScene s(2, false);
        const Entity far_away = s.w.spawn_with(at(500, 0, 500));
        s.drive({1.0f, 0.0f, 0.0f}, 6.0f, 0.0f);
        s.w.try_get_mut<CameraFollow3D>(s.cam)->target = far_away;
        const f32 before = s.yaw();
        s.w.tick(kDt, s.t++);
        CHECK(s.yaw() == before);
    }
    {  // between 70% and 100% of chase_min_speed it keeps chasing once started, but never starts
        ChaseScene s(2, false);
        s.drive({1.0f, 0.0f, 0.0f}, 2.6f, 3.0f);  // 2.6 < 3: does not start
        CHECK(s.yaw() == 0.0f);
        s.drive({1.0f, 0.0f, 0.0f}, 3.2f, 0.5f);  // starts
        const f32 started = s.yaw();
        CHECK(started != 0.0f);
        s.drive({1.0f, 0.0f, 0.0f}, 2.6f, 0.5f);  // 2.6 >= 2.1: keeps going
        CHECK(std::fabs(s.yaw()) > std::fabs(started));
        s.drive({1.0f, 0.0f, 0.0f}, 1.5f, 0.5f);  // below 70%: stops
        const f32 stopped = s.yaw();
        s.drive({1.0f, 0.0f, 0.0f}, 1.5f, 0.5f);
        CHECK(s.yaw() == stopped);
    }
    {  // mode 2 is any moving target, a reversing car included (mode 1 skips it)
        ChaseScene s(2, true, -6.0f);
        s.drive({1.0f, 0.0f, 0.0f}, 6.0f, 3.0f);
        CHECK(std::sin(s.yaw()) < -0.99f);
    }
}

TEST_CASE("effects: the Look input holds a chasing camera for chase_hold seconds") {
    ChaseScene s(2, false);
    input::InputState in;
    in.look = {0.5f, 0.0f};
    s.w.set_resource(in);
    s.drive({1.0f, 0.0f, 0.0f}, 6.0f, 0.2f);  // the player turns the camera
    const f32 held = s.yaw();
    s.w.resource<input::InputState>().look = {0.0f, 0.0f};
    s.drive({1.0f, 0.0f, 0.0f}, 6.0f, 1.0f);  // inside chase_hold (1.5 s): the yaw stays where the player left it
    CHECK(s.yaw() == held);
    s.drive({1.0f, 0.0f, 0.0f}, 6.0f, 3.0f);  // then it eases behind the heading
    CHECK(std::sin(s.yaw()) < -0.99f);
}

TEST_CASE("effects: PathAgents walk on a NavGrid built from the scene's colliders") {
    World w;
    w.register_components<LocalTransform>();
    physics::PhysicsConfig cfg;
    cfg.gravity = {0.0f, 0.0f};
    physics::install_physics2d(w, cfg);
    install_effects(w, nullptr);
    physics::RigidBody2D fixed;
    physics::Collider2D wall;
    wall.shape = physics::ColliderShape::box({0.5f, 3.0f});
    w.spawn_with(at(0, 0), fixed, wall);
    PathAgent a;
    a.target = {4.0f, 0.0f};
    a.speed = 6.0f;
    const Entity e = w.spawn_with(at(-4, 0), a);

    u64 t = 0;
    w.tick(kDt, t++);
    REQUIRE(w.try_resource<NavGrid>() != nullptr);  // no NavGridSource: the automatic grid
    CHECK_FALSE(w.resource<NavGrid>().walkable(w.resource<NavGrid>().cell_at({0.0f, 0.0f})));
    for (; t < 600 && w.try_get<PathAgent>(e)->status != PathStatus::Arrived; ++t) w.tick(kDt, t);
    CHECK(w.try_get<PathAgent>(e)->status == PathStatus::Arrived);
    CHECK(w.try_get<LocalTransform>(e)->position.x == doctest::Approx(4.0f).epsilon(0.02));

    // A NavGridSource replaces it when asked to rebuild.
    NavGridSource src;
    src.origin = {-8.0f, -8.0f};
    src.width = 32;
    src.height = 32;
    src.cell_size = 0.5f;
    src.inflate = 0.0f;
    src.rebuild = true;
    const Entity s = w.spawn_with(src);
    w.tick(kDt, t++);
    CHECK(w.resource<NavGrid>().width() == 32);
    CHECK(w.try_get<NavGridSource>(s)->blocked == 2 * 12);  // the 1 x 6 m wall in half-metre cells
    CHECK_FALSE(w.try_get<NavGridSource>(s)->rebuild);
}

TEST_CASE("M-ACCESS: reduced motion scales the view's shake, never the camera's state") {
    ConVar* motion = ConVar::find(StringId("access.motion_scale"));
    REQUIRE(motion != nullptr);
    const auto run = [&](f32 scale, render::Camera2D& view) {
        motion->set_float(scale);
        World w;
        w.register_components<LocalTransform>();
        install_effects(w, nullptr);
        w.register_components<Camera2DComponent>();
        w.spawn_with(at(2, 3), Camera2DComponent{}, CameraShake2D{});
        ShakeTrigger s;
        s.trauma = 1.0f;
        s.fire = true;
        w.spawn_with(s);
        for (u64 t = 0; t < 3; ++t) w.tick(kDt, t);
        f32 ppm = 0.0f;
        REQUIRE(primary_camera(w, view, ppm));
        return w.state_hash();
    };
    render::Camera2D full, half, still;
    const u64 h_full = run(1.0f, full), h_half = run(0.5f, half), h_still = run(0.0f, still);
    motion->reset();
    CHECK(h_full == h_half);  // the setting never reaches the simulation
    CHECK(h_full == h_still);
    CHECK(full.position.x != doctest::Approx(2.0f));
    CHECK(half.position.x - 2.0f == doctest::Approx((full.position.x - 2.0f) * 0.5f));
    CHECK(still.position.x == doctest::Approx(2.0f));
    CHECK(still.position.y == doctest::Approx(3.0f));
    CHECK(still.rotation == doctest::Approx(0.0f));
}
