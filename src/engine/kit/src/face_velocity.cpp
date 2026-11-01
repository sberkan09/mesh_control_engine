#include "mce/kit/face_velocity.h"

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/seats3d.h"

namespace mce::kit {

static_assert(sizeof(FaceVelocity3D) == 24);

namespace {

struct FaceVelocityInstalled {};

void kit_face_velocity(Frame& f) {
    World& w = f.world();
    f.query<FaceVelocity3D, LocalTransform>().each([&](Entity, FaceVelocity3D& fv, LocalTransform& t) {
        if (fv.source.is_null() || !w.alive(fv.source)) return;
        const CharacterController3D* c = w.try_get<CharacterController3D>(fv.source);
        if (!c) return;
        if (const Rider3D* r = w.try_get<Rider3D>(fv.source); r && r->seated == 1) return;
        const f32 speed = std::sqrt(c->velocity.x * c->velocity.x + c->velocity.z * c->velocity.z);
        if (speed > fv.min_speed) {
            const f32 want = std::atan2(-c->velocity.x, -c->velocity.z);
            f32 delta = want - fv.yaw;
            while (delta > math::kPi) delta -= math::kTau;
            while (delta < -math::kPi) delta += math::kTau;
            const f32 step = math::radians(fv.turn_rate_deg) * f.dt;
            fv.yaw += math::clamp(delta, -step, step);
            if (fv.yaw > math::kPi) fv.yaw -= math::kTau;
            if (fv.yaw < -math::kPi) fv.yaw += math::kTau;
        }
        t.rotation = math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, fv.yaw + fv.yaw_offset);
    });
}

}  // namespace

void install_face_velocity(World& world) {
    if (world.try_resource<FaceVelocityInstalled>()) return;
    world.set_resource(FaceVelocityInstalled{});
    world.register_components<FaceVelocity3D>();
    world.add_system(Phase::FixedUpdate_Pre, "kit_face_velocity", &kit_face_velocity, "kit3d_bodies");
}

}  // namespace mce::kit
