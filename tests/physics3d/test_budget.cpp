// The 2,000-body budget in 3D (M-GAMEPLAY3D gate; FR-PHY-050's 2D budget in tests/physics2d): 2,000
// free-falling boxes step within 3 ms in optimized builds, and a 2,000-body pile with joints between
// some of its pieces stays deterministic.

#include <chrono>
#include <cmath>

#include "helpers.h"

using namespace phys3_test;

TEST_CASE("budget3d: 2,000 dynamic bodies step within budget") {
    // Spread on a grid with gaps and free-falling: every step moves 2,000 bodies through the broad
    // phase with no contacts to solve.
    World3D w;
    u32 id = 1;
    for (u32 y = 0; y < 10; ++y)
        for (u32 z = 0; z < 10; ++z)
            for (u32 x = 0; x < 20; ++x)
                REQUIRE(add_box(w, ent(id++), BodyType3D::Dynamic,
                                {static_cast<f32>(x) * 1.5f, 20.0f + static_cast<f32>(y) * 1.5f, static_cast<f32>(z) * 1.5f},
                                {0.4f, 0.4f, 0.4f}));
    for (u32 i = 0; i < 5; ++i) w.step(kDt);  // warm-up
    const auto t0 = std::chrono::steady_clock::now();
    const u32 steps = 30;
    for (u32 i = 0; i < steps; ++i) w.step(kDt);
    const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count() / steps;
    CHECK(w.stats().bodies == 2000);
    CHECK(w.stats().active_bodies == 2000);
    MESSAGE("2,000-body 3D step (broadphase + integrate): " << ms << " ms");
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
    CHECK(ms < 3.0);  // optimized builds only: Debug/ASan numbers are not the budget
#endif
}

TEST_CASE("budget3d: a 2,000-body pile with hinged pairs stays deterministic") {
    auto run = [] {
        World3D w;
        add_floor(w, ent(100000));
        u32 id = 1;
        for (u32 y = 0; y < 10; ++y)
            for (u32 z = 0; z < 10; ++z)
                for (u32 x = 0; x < 20; ++x) {
                    const vec3 p{-15.0f + static_cast<f32>(x) * 1.5f, 0.5f + static_cast<f32>(y) * 1.05f, -7.5f + static_cast<f32>(z) * 1.5f};
                    REQUIRE(add_box(w, ent(id++), BodyType3D::Dynamic, p, {0.45f, 0.45f, 0.45f}));
                }
        // Every tenth box hinged to its neighbour along x.
        for (u32 i = 1; i + 1 < id; i += 10) {
            JointDesc3D j;
            j.type = JointType3D::Hinge;
            j.a = ent(i);
            j.b = ent(i + 1);
            j.anchor = position_of(w, ent(i)) + vec3{0.75f, 0, 0};
            j.axis = {0, 0, 1};
            REQUIRE(w.create_joint(j) != 0);
        }
        f64 worst = 0.0;
        for (u32 i = 0; i < 60; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            w.step(kDt);
            worst = std::fmax(worst, std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        MESSAGE("2,000-body 3D pile: worst full step " << worst << " ms, active " << w.stats().active_bodies << ", joints "
                                                       << w.stats().joints);
        CHECK(w.stats().joints == 200);
        return w.hash_state(0);
    };
    CHECK(run() == run());
}
