// Facing the movement (M-FACEMOVE): a model turns about Y towards the ground velocity of its source character, at a
// limited rate, the shortest way round, and keeps its heading when the character stands still.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/face_velocity.h"
#include "mce/kit/kit3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

struct Scene {
    World w;
    u64 t = 0;
    Entity walker, model;
    Scene(f32 turn_deg = 360.0f) {
        w.register_components<LocalTransform>();
        install_bodies3d(w, nullptr);
        install_face_velocity(w);
        BoxCollider3D floor;
        floor.half = {50.0f, 0.5f, 50.0f};
        w.spawn_with(at(0, -0.5f, 0), floor);
        walker = w.spawn_with(at(0, 2, 0), CharacterController3D{});
        FaceVelocity3D fv;
        fv.source = walker;
        fv.turn_rate_deg = turn_deg;
        model = w.spawn_with(at(0, 0, 0), fv);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    void walk(f32 x, f32 z) { w.try_get_mut<CharacterController3D>(walker)->move = {x, 0.0f, z}; }
    f32 yaw() { return w.try_get<FaceVelocity3D>(model)->yaw; }
};

}  // namespace

TEST_CASE("face_velocity: the model turns to the walking direction and keeps it when the walker stops") {
    Scene s;
    s.walk(3.0f, 0.0f);  // +X
    s.run(60);
    CHECK(s.yaw() == doctest::Approx(-math::kPi * 0.5f).epsilon(1e-3));
    // The rotation sends the forward axis (-Z) onto +X.
    const math::vec3 fwd = math::rotate(s.w.try_get<LocalTransform>(s.model)->rotation, math::vec3{0, 0, -1});
    CHECK(fwd.x == doctest::Approx(1.0f).epsilon(1e-3));
    CHECK(std::abs(fwd.z) < 1e-3f);
    s.walk(0.0f, 0.0f);
    s.run(30);
    CHECK(s.yaw() == doctest::Approx(-math::kPi * 0.5f).epsilon(1e-3));
}

TEST_CASE("face_velocity: the turn is rate limited") {
    Scene s(90.0f);  // 90 degrees a second
    s.walk(3.0f, 0.0f);
    s.run(30);  // half a second: about 45 degrees of the 90 wanted
    CHECK(std::abs(s.yaw()) < math::kPi * 0.5f - 0.1f);
    CHECK(std::abs(s.yaw()) > 0.3f);
}

TEST_CASE("face_velocity: yaw_offset is added to the written rotation only") {
    Scene s;
    s.w.try_get_mut<FaceVelocity3D>(s.model)->yaw_offset = math::kPi;
    s.walk(3.0f, 0.0f);
    s.run(60);
    CHECK(s.yaw() == doctest::Approx(-math::kPi * 0.5f).epsilon(1e-3));
    const math::vec3 fwd = math::rotate(s.w.try_get<LocalTransform>(s.model)->rotation, math::vec3{0, 0, -1});
    CHECK(fwd.x == doctest::Approx(-1.0f).epsilon(1e-3));  // a +Z mesh faces +X
}
