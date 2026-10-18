// M-MOVE3D gate (ROADMAP, Linear SBA-139): 64 characters and 8 vehicles step within a stated budget, and the crowd
// replays to the same hash. 64 CharacterController3D walkers (every other one crouching or jumping on a schedule)
// share a 300 m field with 8 AI-driven cars lapping a 120 m loop, and a spring-arm camera follows one of them.
// Budget: at most 4 ms per 60 Hz tick on average, in optimized builds only (Debug and sanitizer numbers are not the
// budget); the whole crowd is always checked for sanity and for a repeatable hash.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/effects.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/particles.h"
#include "mce/kit/vehicles3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr u32 kWalkers = 64;
constexpr u32 kCars = 8;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

struct Field {
    World w;
    u64 t = 0;
    Entity walkers[kWalkers];
    Entity cars[kCars];
    Field() {
        w.register_components<LocalTransform>();
        install_bodies3d(w, nullptr);
        install_particles(w, nullptr);
        install_effects(w, nullptr);
        install_vehicles3d(w, nullptr);
        BoxCollider3D ground;
        ground.half = {150.0f, 0.5f, 150.0f};
        ground.friction = 1.0f;
        w.spawn_with(at(0, -0.5f, 0), ground);
        // The walkers: an 8 x 8 grid 3 m apart in the field's south-west quarter, clear of the loop.
        for (u32 row = 0; row < 8; ++row)
            for (u32 col = 0; col < 8; ++col) {
                CharacterController3D c;
                c.acceleration = 30.0f;
                c.deceleration = 40.0f;
                c.coyote_time = 0.1f;
                c.jump_buffer = 0.1f;
                c.crouch_half_height = 0.3f;
                c.layer = 1;
                walkers[row * 8 + col] = w.spawn_with(at(-130.0f + 3.0f * static_cast<f32>(col), 0.9f, 60.0f + 3.0f * static_cast<f32>(row)), c);
            }
        // The cars: a 100 x 100 m loop of waypoints around the origin, two cars per side-start, staggered along it.
        const math::vec3 loop[4] = {{50, 0.3f, 50}, {-50, 0.3f, 50}, {-50, 0.3f, -50}, {50, 0.3f, -50}};
        Entity points[4];
        for (u32 i = 0; i < 4; ++i) points[i] = w.spawn_with(at(loop[i].x, loop[i].y, loop[i].z));
        for (u32 i = 0; i < 4; ++i) {
            Waypoint3D wp;
            wp.next = points[(i + 1) % 4];
            w.add(points[i], wp);
        }
        for (u32 i = 0; i < kCars; ++i) {
            RigidBody3D b;
            b.half = {0.9f, 0.4f, 2.0f};
            b.density = 1200.0f / (8.0f * 0.9f * 0.4f * 2.0f);
            Vehicle3D v;
            v.anti_roll = 8000.0f;
            v.front_axle = -1.4f;
            v.rear_axle = 1.4f;
            v.track = 0.9f;
            v.wheel_y = -0.3f;
            v.drive = VehicleDrive3D::All;
            v.skid_slip = 0.2f;
            VehicleDriver3D d;
            d.path = points[1];  // heading west along the north side, from different x and lane so they spread out
            d.max_speed = 14.0f + static_cast<f32>(i);
            LocalTransform lt = at(40.0f - 10.0f * static_cast<f32>(i), 1.0f, 50.0f - 3.0f * static_cast<f32>(i % 2));
            lt.rotation = math::quat_from_axis_angle({0, 1, 0}, 1.5707964f);
            cars[i] = w.spawn_with(lt, b, v, d);
        }
        CameraFollow3D follow;
        follow.target = walkers[0];
        follow.collide = true;
        follow.look_ahead = 0.3f;
        w.spawn_with(at(0, 0, 0), follow);
    }
    // A repeatable stroll: each walker heads along a direction that turns every second, a third of them jump every
    // 1.5 s, every fourth crouches for half a second out of two.
    void run(u32 n) {
        for (u32 k = 0; k < n; ++k) {
            for (u32 i = 0; i < kWalkers; ++i) {
                CharacterController3D& c = *w.try_get_mut<CharacterController3D>(walkers[i]);
                const f32 a = static_cast<f32>(((t / 60 + i * 7) % 8)) * 0.785398f;
                c.move = {std::cos(a) * 3.0f, 0.0f, std::sin(a) * 3.0f};
                if (i % 3 == 0 && (t + i) % 90 == 0) c.jump = true;
                c.crouch = i % 4 == 0 && (t / 30) % 2 == 0;
            }
            w.tick(kDt, t++);
        }
    }
};

}  // namespace

TEST_CASE("move3d budget: 64 characters and 8 vehicles step within 4 ms (optimized builds)") {
    Field f;
    f.run(60);  // warm-up: bodies built, the cars settled on their wheels
    const u32 ticks = 240;
    const auto t0 = std::chrono::steady_clock::now();
    f.run(ticks);
    const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count() / ticks;
    MESSAGE("64 characters + 8 vehicles: " << ms << " ms per tick");

    // Sanity: everybody is still on the field, the walkers grounded or in a jump, the cars driving their loop.
    u32 grounded = 0;
    for (const Entity e : f.walkers) {
        const LocalTransform& p = *f.w.try_get<LocalTransform>(e);
        CHECK(p.position.y > 0.0f);
        CHECK(p.position.y < 3.0f);
        grounded += f.w.try_get<CharacterController3D>(e)->grounded;
    }
    CHECK(grounded > kWalkers / 2);
    for (const Entity e : f.cars) {
        CHECK(f.w.try_get<LocalTransform>(e)->position.y < 2.0f);
        CHECK(f.w.try_get<Vehicle3D>(e)->wheels_on_ground >= 1);
        CHECK(f.w.try_get<Vehicle3D>(e)->speed > 3.0f);
    }
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
    CHECK(ms < 4.0);  // optimized builds only: Debug/ASan numbers are not the budget
#endif
}

TEST_CASE("move3d budget: the crowd hashes the same on two runs") {
    Field a, b;
    for (u32 i = 0; i < 180; ++i) {
        a.run(1);
        b.run(1);
        REQUIRE(a.w.state_hash() == b.w.state_hash());
    }
}
