#pragma once
// 3D joints as components (M-GAMEPLAY3D step 2): a door on a hinge, a drawbridge on a motor, a
// piston, a rope, a ball joint, or any mix of locked, free and limited axes, between two bodies or
// a body and the world, on physics::World3D's joints (Jolt constraints). kit::install_stock
// installs them.
//
//   Joint3D  on a body's entity (RigidBody3D, or a static BoxCollider3D / MeshCollider3D): joins it
//            to `connected` (another body's entity) or, when that is null, to the world
//
// The joint is built the first tick both bodies exist, from where they are then: that pose is the
// rest pose (hinge angle 0, slider position 0). `anchor` and `axis` are in the entity's space
// (scaled). Changing any setting other than `enabled` and the motor rebuilds the joint from the
// bodies' current poses; so does a static collider moving (its body is rebuilt). Removing the
// component, or either body, removes the joint.
//
// A joint whose force or torque goes over `break_force` / `break_torque` breaks: `broken` is set,
// a JointBreak3D event is raised and the joint stops acting until a setting changes.
//
// Systems: "kit3d_joints" (FixedUpdate_Pre, after "kit3d_bodies": build, rebuild, enable, motor) and
// "kit3d_joint_state" (FixedUpdate_Physics, after "kit3d_writeback": `position`, `force`, `torque`,
// breaks and their events).

#include "mce/ecs/entity.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

enum class JointKind3D : u8 { Fixed, Hinge, Slider, Distance, Cone, SixDOF };
MCE_ENUM(JointKind3D, Fixed, Hinge, Slider, Distance, Cone, SixDOF)

enum class JointMotor3D : u8 { Off, Velocity, Position };
MCE_ENUM(JointMotor3D, Off, Velocity, Position)

struct Joint3D {
    Entity connected;                    // the other body's entity; null = the world
    JointKind3D kind = JointKind3D::Hinge;
    JointMotor3D motor = JointMotor3D::Off;  // Hinge, Slider
    u8 limits = 0;                       // 1: Hinge / Slider stay within limit_min .. limit_max
    u8 collide_connected = 0;            // 1: the two bodies still collide with each other
    u8 enabled = 1;                      // 0: the joint stops acting (kept, and back on with 1)
    u8 broken = 0;                       // engine-written: broke under break_force / break_torque
    u16 pad_ = 0;
    math::vec3 anchor;                   // the pivot, entity space (Distance: the near end)
    math::vec3 axis{0.0f, 1.0f, 0.0f};   // entity space: hinge / slider / cone axis, six-DOF frame X
    math::vec3 connected_anchor;         // Distance: the far end, in `connected`'s space (world space without one)
    f32 limit_min = -45.0f;              // Hinge: degrees (-180..0); Slider: metres (<= 0)
    f32 limit_max = 45.0f;               // Hinge: degrees (0..180); Slider: metres (>= 0)
    f32 min_distance = -1.0f;            // Distance: metres; < 0 = the distance when the joint is built
    f32 max_distance = -1.0f;            // Distance: metres; < 0 = the distance when the joint is built
    f32 cone_angle = 30.0f;              // Cone: how far `axis` may swing, degrees
    // SixDOF, per axis of the frame (X = axis): min > max free, min == max locked, else limited.
    math::vec3 linear_min;               // metres
    math::vec3 linear_max;
    math::vec3 angular_min;              // degrees
    math::vec3 angular_max;
    f32 spring_frequency = 0.0f;         // > 0: Hinge / Slider limits and Distance are springs of this Hz
    f32 spring_damping = 0.0f;           // 0..1
    f32 friction = 0.0f;                 // Hinge: N*m, Slider: N, holding against motion
    f32 motor_target = 0.0f;             // Velocity: deg/s or m/s; Position: degrees or metres
    f32 motor_max_force = 0.0f;          // N*m (Hinge) or N (Slider); 0 = unlimited
    f32 motor_frequency = 2.0f;          // Position: how stiffly the motor holds the target (Hz)
    f32 break_force = 0.0f;              // N; 0 = never breaks
    f32 break_torque = 0.0f;             // N*m; 0 = never breaks
    f32 position = 0.0f;                 // engine-written: Hinge degrees, Slider / Distance metres
    f32 force = 0.0f;                    // engine-written: the force it held with (N)
    f32 torque = 0.0f;                   // engine-written: the torque it held with (N*m)
};
MCE_COMPONENT(Joint3D, (connected, Help{"the other body's entity; none = the world"}), kind,
              (motor, Help{"Hinge, Slider: Velocity or Position"}), (limits, Help{"1: Hinge / Slider stay within limit_min .. limit_max"}),
              (collide_connected, Help{"1: the two bodies still collide"}), (enabled, Help{"0: switched off"}), (broken, ReadOnly, Transient),
              (anchor, Help{"the pivot, entity space"}), (axis, Help{"entity space; six-DOF frame X"}),
              (connected_anchor, Help{"Distance: the far end, in the connected entity's space (world without one)"}),
              (limit_min, Help{"Hinge: degrees (-180..0); Slider: metres"}), (limit_max, Help{"Hinge: degrees (0..180); Slider: metres"}),
              (min_distance, Help{"Distance: metres; < 0 = as built"}), (max_distance, Help{"Distance: metres; < 0 = as built"}),
              (cone_angle, Range{0, 180}, Help{"Cone: degrees"}), (linear_min, Help{"SixDOF metres: min > max free, min == max locked"}),
              linear_max, (angular_min, Help{"SixDOF degrees: min > max free, min == max locked"}), angular_max,
              (spring_frequency, Range{0, 100}, Help{"> 0: soft limits, Hz"}), (spring_damping, Range{0, 1}),
              (friction, Range{0, 100000}), (motor_target, Help{"Velocity: deg/s or m/s; Position: degrees or metres"}),
              (motor_max_force, Range{0, 1000000}, Help{"0 = unlimited"}), (motor_frequency, Range{0.01f, 100}),
              (break_force, Range{0, 10000000}, Help{"N; 0 = never"}), (break_torque, Range{0, 10000000}, Help{"N*m; 0 = never"}),
              (position, ReadOnly, Transient), (force, ReadOnly, Transient), (torque, ReadOnly, Transient))

// Event (producer FixedUpdate_Physics): a joint broke.
struct JointBreak3D {
    Entity joint;      // the Joint3D's entity
    Entity connected;  // its `connected` (null: the world)
};
MCE_EVENT(JointBreak3D, joint, connected)

// Registers Joint3D and JointBreak3D and adds the systems (after install_bodies3d, which it calls).
// Idempotent.
void install_joints3d(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
