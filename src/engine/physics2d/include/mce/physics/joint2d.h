#pragma once
// 2D joints as a component (M-JOINT2D step 1): a rod, a rope, a spring or a hinge (with an angle
// limit and a motor) between two bodies, on World2D's distance and revolute joints.
// install_physics2d registers it.
//
//   Joint2D  on a body's entity (RigidBody2D + Collider2D): joins it to `connected`, another
//            body's entity. World2D needs two bodies, at least one of them dynamic, so a joint to
//            the world is made with a static body.
//
// The joint is built in physics_step, after the bodies are imported and before the step, the first
// tick both bodies exist; the poses then are the rest pose (hinge angle 0). `anchor` is in this
// entity's frame and `connected_anchor` in the connected entity's (Distance only: a hinge pivots
// at `anchor`). Changing any setting other than the motor speed and the limit angles rebuilds the
// joint from the bodies' current poses; those three update in place. `enabled` 0 removes the joint
// and keeps the component. Removing the component, or either body, removes the joint.
//
// Units are the component's own: degrees and degrees per second here, radians in World2D.

#include "mce/ecs/entity.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::physics {

enum class JointKind2D : u8 { Distance, Revolute };
MCE_ENUM(JointKind2D, Distance, Revolute)

struct Joint2D {
    Entity connected;                     // the other body's entity (required)
    JointKind2D kind = JointKind2D::Revolute;
    u8 enabled = 1;                       // 0: the joint is removed (the component stays; 1 builds it again)
    u8 collide_connected = 0;             // 1: the two bodies still collide with each other
    u8 limits = 0;                        // Revolute: 1 keeps the angle within lower .. upper
    u8 motor = 0;                         // Revolute: 1 drives the hinge at motor_speed
    u8 pad_[3] = {};
    math::vec2 anchor;                    // this entity's frame: the hinge pivot, or the near end of a Distance joint
    math::vec2 connected_anchor;          // Distance: the far end, in the connected entity's frame
    f32 length = -1.0f;                   // Distance: metres; < 0 = the anchors' distance when built
    f32 min_length = -1.0f;               // Distance: < 0 = length (a rod); min < max makes a rope
    f32 max_length = -1.0f;               // Distance: < 0 = length
    f32 hertz = 0.0f;                     // Distance: > 0 makes it a spring of this frequency
    f32 damping_ratio = 0.0f;             // Distance spring: 0..1
    f32 lower = -45.0f;                   // Revolute: degrees from the rest angle (-180..0)
    f32 upper = 45.0f;                    // Revolute: degrees from the rest angle (0..180)
    f32 motor_speed = 0.0f;               // Revolute: degrees per second
    f32 max_motor_torque = 0.0f;          // Revolute: N*m
    f32 length_now = 0.0f;                // engine-written: Distance, the anchors' distance (m)
    f32 angle_now = 0.0f;                 // engine-written: Revolute, degrees from the rest angle
    friend bool operator==(const Joint2D&, const Joint2D&) = default;
};
MCE_COMPONENT(Joint2D, (connected, Help{"the other body's entity (required)"}), kind,
              (enabled, Help{"0: removes the joint, keeps the component"}), (collide_connected, Help{"1: the two bodies still collide"}),
              (limits, Help{"Revolute: 1 keeps the angle within lower .. upper"}), (motor, Help{"Revolute: 1 drives the hinge at motor_speed"}),
              (anchor, Help{"this entity's frame: hinge pivot, or the near end of a Distance joint"}),
              (connected_anchor, Help{"Distance: the far end, in the connected entity's frame"}),
              (length, Help{"Distance: metres; < 0 = as built"}), (min_length, Help{"Distance: < 0 = length; min < max makes a rope"}),
              (max_length, Help{"Distance: < 0 = length"}), (hertz, Range{0, 100}, Help{"Distance: > 0 makes a spring, Hz"}),
              (damping_ratio, Range{0, 1}), (lower, Range{-180, 0}, Help{"Revolute: degrees from the rest angle"}),
              (upper, Range{0, 180}, Help{"Revolute: degrees from the rest angle"}), (motor_speed, Help{"Revolute: degrees per second"}),
              (max_motor_torque, Range{0, 1000000}, Help{"Revolute: N*m"}), (length_now, ReadOnly, Transient),
              (angle_now, ReadOnly, Transient))

}  // namespace mce::physics
