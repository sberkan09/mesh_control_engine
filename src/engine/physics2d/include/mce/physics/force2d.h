#pragma once
// Pushing 2D bodies from components (M-PHYSDRIVE2D step 1), on World2D's velocity, impulse and
// force calls. install_physics2d registers them; physics_step applies them after the bodies are
// imported and before the step. Each needs the entity to carry RigidBody2D and Collider2D.
//
//   LaunchVelocity2D  sets the body's velocity once, the first tick the body exists (a thrown
//                     ball, a bullet). Dynamic and kinematic bodies; ignored on static ones.
//   Impulse2D         a one-shot push in world space at `point` (the body's frame, 0,0 = its
//                     centre). The tick it is applied `impulse` goes back to zero, so writing a
//                     new value pushes again. Dynamic bodies only.
//   ConstantForce2D   a force in newtons applied every tick while the component is there (wind,
//                     a conveyor, a thruster). Dynamic bodies only.
//
// NaN and infinity count as zero. Bodies without these components are untouched.

#include "mce/ecs/entity.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::physics {

struct LaunchVelocity2D {
    math::vec2 linear;        // m/s, world space
    f32 angular = 0.0f;       // degrees per second
    u8 applied = 0;           // set when the velocity was applied; not saved, so a loaded scene launches again
    u8 pad_[3] = {};
    friend bool operator==(const LaunchVelocity2D&, const LaunchVelocity2D&) = default;
};
MCE_COMPONENT(LaunchVelocity2D, (linear, Help{"m/s, world space"}), (angular, Help{"degrees per second"}),
              (applied, ReadOnly, Transient))

struct Impulse2D {
    math::vec2 impulse;  // N*s, world space; reset to zero once applied
    math::vec2 point;    // where it pushes, in the body's frame
    friend bool operator==(const Impulse2D&, const Impulse2D&) = default;
};
MCE_COMPONENT(Impulse2D, (impulse, Help{"N*s, world space; goes back to zero when applied"}),
              (point, Help{"where it pushes, in the body's frame"}))

struct ConstantForce2D {
    math::vec2 force;  // N, world space
    friend bool operator==(const ConstantForce2D&, const ConstantForce2D&) = default;
};
MCE_COMPONENT(ConstantForce2D, (force, Help{"N, world space, every tick"}))

}  // namespace mce::physics
