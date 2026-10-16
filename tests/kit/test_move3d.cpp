// M-MOVE3D, with the numbers each feature promises. Step 1, character feel: CharacterController3D's
// acceleration, coyote time, jump buffer, jump cut, air jumps, sprint, crouch and the Landed3D event.
// Step 2, the camera: CameraFollow3D's spring arm, look-ahead, first person and lock-on.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/effects.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/particles.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

struct Scene {
    World w;
    u64 t = 0;
    u32 landings = 0;
    f32 last_impact = 0.0f;
    explicit Scene(f32 floor_half_x = 20.0f) {
        w.register_components<LocalTransform>();
        install_bodies3d(w, nullptr);
        install_particles(w, nullptr);
        install_effects(w, nullptr);
        box({0, -0.5f, 0}, {floor_half_x, 0.5f, 20.0f});  // top at y = 0
    }
    Entity box(math::vec3 p, math::vec3 half) {
        BoxCollider3D c;
        c.half = half;
        return w.spawn_with(at(p.x, p.y, p.z), c);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) {
            w.tick(kDt, t++);
            for (const Landed3D& ev : EventAccess(w).read<Landed3D>()) {
                ++landings;
                last_impact = ev.impact_speed;
            }
        }
    }
    CharacterController3D& cc(Entity e) { return *w.try_get_mut<CharacterController3D>(e); }
    f32 y(Entity e) { return w.try_get<LocalTransform>(e)->position.y; }
    f32 x(Entity e) { return w.try_get<LocalTransform>(e)->position.x; }
};

// A character standing at rest on the floor (capsule centre 0.9 m up).
Entity stand(Scene& s, const CharacterController3D& c, f32 x = 0.0f) {
    const Entity e = s.w.spawn_with(at(x, 0.9f, 0), c);
    s.run(10);
    REQUIRE(s.cc(e).grounded);
    return e;
}

// Walks a character off a ledge at x = 2 and asks for a jump `wait` ticks after it left the ground.
// True when the jump happened.
bool coyote_jump(f32 coyote_time, u32 wait) {
    Scene s(2.0f);
    s.box({0, -20.5f, 0}, {40, 0.5f, 40});  // far below
    CharacterController3D c;
    c.coyote_time = coyote_time;
    c.move = {3, 0, 0};
    const Entity e = stand(s, c, 1.0f);
    while (s.cc(e).grounded) s.run(1);
    CHECK(s.cc(e).airborne_ticks == 1);
    s.run(wait - 1);
    CHECK(s.cc(e).airborne_ticks == wait);
    s.cc(e).jump = true;
    s.run(1);
    return s.cc(e).velocity.y > 4.0f;
}

// Drops a character from 3 m and asks for a jump `early` ticks before the tick it lands on. Returns
// the highest point after landing (0.9 when it did not jump).
f32 buffered_jump(f32 jump_buffer, u32 early) {
    u32 land = 0;
    {
        Scene s;
        const Entity e = s.w.spawn_with(at(0, 3, 0), CharacterController3D{});
        while (!s.cc(e).grounded) {
            s.run(1);
            ++land;
        }
    }
    Scene s;
    CharacterController3D c;
    c.jump_buffer = jump_buffer;
    const Entity e = s.w.spawn_with(at(0, 3, 0), c);
    s.run(land - early - 1);
    CHECK_FALSE(s.cc(e).grounded);
    s.cc(e).jump = true;
    s.run(early + 1);
    CHECK(s.cc(e).grounded);  // the landing tick
    f32 top = s.y(e);
    for (u32 i = 0; i < 40; ++i) {
        s.run(1);
        top = math::max(top, s.y(e));
    }
    return top;
}

// The highest point of a jump at 5 m/s, Jump let go after `hold` ticks (0 = never).
f32 jump_peak(f32 jump_cut, u32 hold) {
    Scene s;
    CharacterController3D c;
    c.jump_cut = jump_cut;
    const Entity e = stand(s, c);
    const f32 floor_y = s.y(e);
    s.cc(e).jump = true;
    f32 top = floor_y;
    for (u32 i = 0; i < 90; ++i) {
        if (hold > 0 && i == hold) s.cc(e).release_jump = true;
        s.run(1);
        top = math::max(top, s.y(e));
    }
    return top - floor_y;
}

}  // namespace

TEST_CASE("move3d: acceleration and deceleration ease the ground speed at their rates") {
    Scene s;
    CharacterController3D c;
    c.acceleration = 30.0f;  // 0.5 m/s per tick
    c.deceleration = 60.0f;  // 1 m/s per tick
    const Entity e = stand(s, c);
    s.cc(e).move = {6, 0, 0};
    s.run(6);
    CHECK(s.cc(e).velocity.x == doctest::Approx(3.0f).epsilon(0.01));
    s.run(6);
    CHECK(s.cc(e).velocity.x == doctest::Approx(6.0f).epsilon(0.01));
    s.run(10);
    CHECK(s.cc(e).velocity.x == doctest::Approx(6.0f).epsilon(0.01));  // capped at the wanted speed
    s.cc(e).move = {};
    s.run(3);
    CHECK(s.cc(e).velocity.x == doctest::Approx(3.0f).epsilon(0.01));
    s.run(3);
    CHECK(std::fabs(s.cc(e).velocity.x) < 1e-3f);

    // The default (0) is the original controller: the full speed on the first tick.
    Scene t;
    const Entity d = stand(t, CharacterController3D{});
    t.cc(d).move = {6, 0, 0};
    t.run(1);
    CHECK(t.cc(d).velocity.x == doctest::Approx(6.0f).epsilon(0.01));
}

TEST_CASE("move3d: air_acceleration steers in the air at its own rate") {
    Scene s;
    CharacterController3D c;
    c.air_acceleration = 6.0f;  // 0.1 m/s per tick
    const Entity e = stand(s, c);
    s.cc(e).jump = true;
    s.run(2);
    REQUIRE_FALSE(s.cc(e).grounded);
    s.cc(e).move = {6, 0, 0};
    s.run(10);
    CHECK(s.cc(e).velocity.x == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("move3d: coyote time lets a jump through for its window after a ledge, and no longer") {
    CHECK(coyote_jump(0.1f, 6));       // 6 ticks at 60 Hz
    CHECK_FALSE(coyote_jump(0.1f, 7));
    CHECK_FALSE(coyote_jump(0.0f, 1));  // the default: none
}

TEST_CASE("move3d: coyote time never adds a jump to a jump") {
    Scene s;
    CharacterController3D c;
    c.coyote_time = 1.0f;  // longer than the whole jump
    const Entity e = stand(s, c);
    s.cc(e).jump = true;
    s.run(35);  // past the apex
    REQUIRE(s.cc(e).velocity.y < 0.0f);
    REQUIRE_FALSE(s.cc(e).grounded);
    s.cc(e).jump = true;
    s.run(1);
    CHECK(s.cc(e).velocity.y < 0.0f);
}

TEST_CASE("move3d: a jump buffered in the air fires on landing within its window") {
    // A 0.1 s buffer holds a request through 6 more airborne ticks.
    CHECK(buffered_jump(0.1f, 5) > 1.8f);
    CHECK(buffered_jump(0.1f, 6) < 1.0f);
    CHECK(buffered_jump(0.0f, 1) < 1.0f);  // the default: a jump only counts on the tick it is asked for
}

TEST_CASE("move3d: letting Jump go early cuts the rise") {
    const f32 full = jump_peak(1.0f, 0);
    CHECK(full == doctest::Approx(5.0f * 5.0f / (2.0f * 9.81f)).epsilon(0.05));  // v^2 / 2g
    CHECK(jump_peak(1.0f, 6) == doctest::Approx(full).epsilon(0.001));         // jump_cut 1: full height always
    // Cut at 0.5 after 6 ticks: 5 - 6 * 9.81 / 60 = 4.02 m/s halves to 2.01 m/s.
    const f32 rise = 5.0f * 0.1f - 0.5f * 9.81f * 0.01f, after = 0.5f * (5.0f - 9.81f * 0.1f);
    CHECK(jump_peak(0.5f, 6) == doctest::Approx(rise + after * after / (2.0f * 9.81f)).epsilon(0.08));
}

TEST_CASE("move3d: air jumps, once each until the ground") {
    Scene s;
    CharacterController3D c;
    c.air_jumps = 1;
    const Entity e = stand(s, c);
    s.cc(e).jump = true;
    s.run(20);
    REQUIRE_FALSE(s.cc(e).grounded);
    s.cc(e).jump = true;
    s.run(1);
    CHECK(s.cc(e).velocity.y > 4.5f);  // the air jump
    CHECK(s.cc(e).air_jumps_used == 1);
    s.run(20);
    s.cc(e).jump = true;
    s.run(1);
    CHECK(s.cc(e).velocity.y < 2.0f);  // none left: still slowing from the air jump (5 - 21 ticks of gravity)
    CHECK(s.cc(e).air_jumps_used == 1);
    s.run(120);
    CHECK(s.cc(e).grounded);
    CHECK(s.cc(e).air_jumps_used == 0);
}

TEST_CASE("move3d: sprint and crouch scale the speed; standing up waits for room") {
    Scene s;
    s.box({4, 1.8f, 0}, {2, 0.5f, 2});  // a low roof: underside at 1.3 m, from x = 2 to 6
    CharacterController3D c;
    c.speed = 4.0f;
    c.crouch_half_height = 0.2f;  // 1.0 m tall crouched
    c.use_input = false;
    c.move = {4, 0, 0};
    const Entity e = stand(s, c, -4.0f);
    s.cc(e).sprint = true;
    s.run(1);
    CHECK(s.cc(e).velocity.x == doctest::Approx(6.0f).epsilon(0.01));  // sprint_multiplier 1.5
    s.cc(e).sprint = false;
    s.cc(e).crouch = true;
    s.run(1);
    CHECK(s.cc(e).crouched);
    CHECK(s.cc(e).velocity.x == doctest::Approx(2.0f).epsilon(0.01));  // crouch_multiplier 0.5
    CHECK(s.y(e) == doctest::Approx(0.9f).epsilon(0.01));              // the feet stay put

    while (s.x(e) < 3.0f) s.run(1);  // under the roof
    s.cc(e).crouch = false;
    s.run(5);
    CHECK(s.cc(e).crouched);  // no room to stand
    while (s.x(e) < 3.5f) s.run(1);
    CHECK(s.cc(e).crouched);
    while (s.x(e) < 6.5f) s.run(1);
    CHECK_FALSE(s.cc(e).crouched);  // out the other side
    CHECK(s.cc(e).velocity.x == doctest::Approx(4.0f).epsilon(0.01));
}

TEST_CASE("move3d: Landed3D reports the impact speed and fires effects on landing") {
    Scene s;
    ParticleBurst burst;
    burst.on = FireOn::Landed;
    const Entity e = s.w.spawn_with(at(0, 3, 0), CharacterController3D{}, burst);
    s.run(60);
    CHECK(s.landings == 1);
    CHECK(s.last_impact == doctest::Approx(std::sqrt(2.0f * 9.81f * 2.1f)).epsilon(0.05));  // a 2.1 m fall
    CHECK(s.w.try_get<ParticleBurst>(e)->fired == 1);
    s.run(30);
    CHECK(s.landings == 1);  // standing still lands once
}

TEST_CASE("move3d: a run with every feature replays to the same hash") {
    auto play = [] {
        Scene s;
        s.box({4, 1.8f, 0}, {2, 0.5f, 2});
        CharacterController3D c;
        c.acceleration = 20.0f;
        c.deceleration = 30.0f;
        c.coyote_time = 0.1f;
        c.jump_buffer = 0.1f;
        c.jump_cut = 0.5f;
        c.air_jumps = 1;
        c.crouch_half_height = 0.2f;
        const Entity e = s.w.spawn_with(at(-5, 2, 0), c);
        for (u32 i = 0; i < 240; ++i) {
            CharacterController3D& k = s.cc(e);
            k.move = {i < 200 ? 4.0f : 0.0f, 0, 0};
            k.jump = i % 50 == 10 || i % 50 == 25;
            k.release_jump = i % 50 == 14;
            k.crouch = i >= 100 && i < 160;
            k.sprint = i < 60;
            s.run(1);
        }
        return s.w.state_hash();
    };
    CHECK(play() == play());
}

// ---- step 2: the camera ----------------------------------------------------------------------------

namespace {

// A camera on a target at the origin, boom along +Z (yaw 0, pitch 0), pivot at the target.
Entity follow_cam(Scene& s, Entity target, const CameraFollow3D& base = {}) {
    CameraFollow3D c = base;
    c.target = target;
    c.offset = {};
    c.pitch = 0.0f;
    c.min_pitch = -1.0f;
    c.smoothing = 0.0f;
    return s.w.spawn_with(LocalTransform{}, c);
}

math::vec3 pos(Scene& s, Entity e) { return s.w.try_get<LocalTransform>(e)->position; }

}  // namespace

TEST_CASE("move3d: the spring arm stays in front of a wall and eases back out when it goes") {
    Scene s;
    const Entity target = s.w.spawn_with(at(0, 1, 0));
    const Entity wall = s.box({0, 1, 3.25f}, {5, 5, 0.25f});  // its near face at z = 3
    CameraFollow3D c;
    c.collide = true;
    c.collision_radius = 0.2f;
    c.return_speed = 4.0f;
    const Entity cam = follow_cam(s, target, c);
    const Entity free = follow_cam(s, target);  // collide off: the old boom
    s.run(2);
    CHECK(pos(s, cam).z == doctest::Approx(2.8f).epsilon(0.01));  // the face less the sphere's radius
    CHECK(pos(s, free).z == doctest::Approx(6.0f).epsilon(0.001));
    CHECK(s.w.try_get<CameraFollow3D>(cam)->arm == doctest::Approx(2.8f).epsilon(0.01));

    s.w.destroy(wall);
    s.run(16);  // the first tick still sees the old wall's body go; then 15 ticks at 4 m/s: + 1 m
    CHECK(pos(s, cam).z == doctest::Approx(3.8f).epsilon(0.02));
    s.run(40);
    CHECK(pos(s, cam).z == doctest::Approx(6.0f).epsilon(0.001));  // capped at distance
}

TEST_CASE("move3d: look-ahead leads the pivot by the target's ground velocity") {
    Scene s;
    const Entity target = s.w.spawn_with(at(0, 1, 0));
    CameraFollow3D c;
    c.look_ahead = 0.5f;
    const Entity cam = follow_cam(s, target, c);
    for (u32 i = 0; i < 10; ++i) {
        s.w.try_get_mut<LocalTransform>(target)->position = {3.0f * kDt * static_cast<f32>(i), 1.0f + 0.1f * static_cast<f32>(i), 0};
        s.run(1);
    }
    const CameraFollow3D& f = *s.w.try_get<CameraFollow3D>(cam);
    const math::vec3 p = s.w.try_get<LocalTransform>(target)->position;
    CHECK(f.pivot.x == doctest::Approx(p.x + 1.5f).epsilon(0.001));  // 3 m/s for 0.5 s
    CHECK(f.pivot.y == doctest::Approx(p.y).epsilon(0.001));         // vertical motion is not led
}

TEST_CASE("move3d: first person puts the camera at the pivot looking along yaw and pitch") {
    Scene s;
    const Entity target = s.w.spawn_with(at(2, 0, 3));
    CameraFollow3D c;
    c.first_person = true;
    const Entity cam = follow_cam(s, target, c);
    s.w.try_get_mut<CameraFollow3D>(cam)->offset = {0, 1.6f, 0};
    s.w.try_get_mut<CameraFollow3D>(cam)->yaw = math::kPi / 2.0f;
    s.run(1);
    CHECK(math::near(pos(s, cam), {2, 1.6f, 3}, 1e-4f));
    const math::vec3 look = math::rotate(s.w.try_get<LocalTransform>(cam)->rotation, math::vec3{0, 0, -1});
    CHECK(math::near(look, {-1, 0, 0}, 1e-4f));  // yaw a quarter turn left of -Z
}

TEST_CASE("move3d: lock-on turns the camera to frame the target in front of the locked entity") {
    Scene s;
    const Entity target = s.w.spawn_with(at(0, 1, 0));
    const Entity foe = s.w.spawn_with(at(10, 1, 0));
    CameraFollow3D c;
    c.lock_on = foe;
    c.lock_on_rate = 3.0f;  // 0.05 rad per tick
    const Entity cam = follow_cam(s, target, c);
    s.run(10);
    CHECK(s.w.try_get<CameraFollow3D>(cam)->yaw == doctest::Approx(-0.5f).epsilon(0.001));
    s.run(30);
    CHECK(s.w.try_get<CameraFollow3D>(cam)->yaw == doctest::Approx(-math::kPi / 2.0f).epsilon(0.001));
    const math::vec3 look = math::rotate(s.w.try_get<LocalTransform>(cam)->rotation, math::vec3{0, 0, -1});
    const math::vec3 to_foe = math::normalize(s.w.try_get<LocalTransform>(foe)->position - pos(s, cam));
    CHECK(math::dot(look, to_foe) > 0.999f);  // the camera, the target and the foe on one line
    CHECK(pos(s, cam).x == doctest::Approx(-6.0f).epsilon(0.001));

    // Free again: the orbit stays where lock-on left it.
    s.w.try_get_mut<CameraFollow3D>(cam)->lock_on = {};
    s.run(5);
    CHECK(s.w.try_get<CameraFollow3D>(cam)->yaw == doctest::Approx(-math::kPi / 2.0f).epsilon(0.001));
}

TEST_CASE("move3d: a camera run with every feature replays to the same hash") {
    auto play = [] {
        Scene s;
        s.box({0, 1, 3.25f}, {5, 5, 0.25f});
        CharacterController3D walker;
        walker.acceleration = 20.0f;
        const Entity hero = s.w.spawn_with(at(-4, 0.9f, 0), walker);
        const Entity foe = s.w.spawn_with(at(6, 1, -4));
        CameraFollow3D c;
        c.collide = true;
        c.look_ahead = 0.3f;
        c.lock_on_rate = 2.0f;
        const Entity cam = follow_cam(s, hero, c);
        for (u32 i = 0; i < 180; ++i) {
            s.cc(hero).move = {3, 0, i < 90 ? 0.0f : 2.0f};
            s.w.try_get_mut<CameraFollow3D>(cam)->lock_on = i >= 60 && i < 140 ? foe : Entity{};
            s.run(1);
        }
        return s.w.state_hash();
    };
    CHECK(play() == play());
}
