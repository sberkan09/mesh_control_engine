#pragma once
// Wheeled vehicles as a component (M-GAMEPLAY3D step 4): a car on four wheels with suspension,
// tyres, an engine, an automatic gearbox and brakes, on physics::World3D's vehicles (Jolt's wheeled
// vehicle). kit::install_stock installs it.
//
//   Vehicle3D  on an entity with a dynamic RigidBody3D (the car's body)
//
// The body's local -Z is forward and +Y up. The four wheels hang from the suspension points
// (±track, wheel_y, front_axle) and (±track, wheel_y, rear_axle), in metres in the body's space;
// the front pair steers. The car is driven by `throttle` (-1..1, negative reverses), `steer`
// (-1..1, positive turns right), `brake` and `handbrake` (0..1), or with `use_input` by the player's
// Move action (y throttle, x steer), Jump (handbrake) and TriggerL (brake). Pushing the throttle
// against the way the car rolls brakes it first. `wheel_fl` .. `wheel_rr` name optional entities
// (root entities with a MeshRef: the wheel models, axle along local X) that are placed on the
// wheels every tick, turned and spinning.
//
// Changing any setting other than the driver's inputs and the wheel entities rebuilds the vehicle.
//
// Systems: "kit3d_vehicles" (FixedUpdate_Pre, after "kit3d_bodies": build, rebuild, the driver) and
// "kit3d_vehicle_state" (FixedUpdate_Physics, after "kit3d_writeback": speed, rpm, gear, wheels).

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

enum class VehicleDrive3D : u8 { Rear, Front, All };
MCE_ENUM(VehicleDrive3D, Rear, Front, All)

struct Vehicle3D {
    u8 use_input = 0;                       // 1: Move / Jump / TriggerL drive it
    VehicleDrive3D drive = VehicleDrive3D::Rear;
    u16 pad_ = 0;
    f32 front_axle = -1.3f;                 // body space z of the front wheels (forward is -Z)
    f32 rear_axle = 1.3f;
    f32 track = 0.8f;                       // body space x of the right wheels (the left ones at -track)
    f32 wheel_y = -0.2f;                    // body space y of the suspension points
    f32 wheel_radius = 0.35f;
    f32 wheel_width = 0.2f;
    f32 suspension_min = 0.1f;              // m from the suspension point to the wheel's centre, compressed
    f32 suspension_max = 0.35f;             // extended
    f32 suspension_frequency = 1.5f;        // Hz: stiffer is higher
    f32 suspension_damping = 0.5f;          // 0..1
    f32 max_steer = 30.0f;                  // degrees, front wheels
    f32 engine_torque = 500.0f;             // N*m
    f32 max_rpm = 6000.0f;
    f32 brake_torque = 1500.0f;             // N*m per wheel
    f32 handbrake_torque = 4000.0f;         // N*m, rear wheels
    f32 grip = 1.0f;                        // tyre friction scale
    f32 max_tilt = 60.0f;                   // degrees the body may pitch or roll (180: free)
    f32 throttle = 0.0f;                    // -1..1, when use_input is off
    f32 steer = 0.0f;                       // -1..1, positive right
    f32 brake = 0.0f;                       // 0..1
    f32 handbrake = 0.0f;                   // 0..1
    Entity wheel_fl;                        // optional wheel models, placed every tick
    Entity wheel_fr;
    Entity wheel_rl;
    Entity wheel_rr;
    f32 speed = 0.0f;                       // engine-written: m/s forward (negative reversing)
    f32 rpm = 0.0f;                         // engine-written
    i32 gear = 0;                           // engine-written: > 0 forward, < 0 reverse
    u32 wheels_on_ground = 0;               // engine-written
};
MCE_COMPONENT(Vehicle3D, (use_input, Help{"1: Move (throttle, steer), Jump (handbrake), TriggerL (brake)"}), drive,
              (front_axle, Help{"body space z of the front wheels; forward is -Z"}), (rear_axle, Help{"body space z of the rear wheels"}),
              (track, Range{0.05f, 10}, Help{"x of the right wheels; the left at -track"}), (wheel_y, Help{"y of the suspension points"}),
              (wheel_radius, Range{0.05f, 5}), (wheel_width, Range{0.01f, 2}), (suspension_min, Range{0, 5}), (suspension_max, Range{0.01f, 5}),
              (suspension_frequency, Range{0.1f, 20}, Help{"Hz"}), (suspension_damping, Range{0, 1}), (max_steer, Range{0, 80}, Help{"degrees"}),
              (engine_torque, Range{1, 100000}, Help{"N*m"}), (max_rpm, Range{1500, 30000}), (brake_torque, Range{0, 100000}, Help{"N*m per wheel"}),
              (handbrake_torque, Range{0, 100000}, Help{"N*m, rear wheels"}), (grip, Range{0, 5}), (max_tilt, Range{0, 180}, Help{"degrees; 180 = free"}),
              (throttle, Range{-1, 1}), (steer, Range{-1, 1}, Help{"positive turns right"}), (brake, Range{0, 1}), (handbrake, Range{0, 1}),
              (wheel_fl, Help{"an entity placed on the front-left wheel"}), wheel_fr, wheel_rl, wheel_rr, (speed, ReadOnly, Transient),
              (rpm, ReadOnly, Transient), (gear, ReadOnly, Transient), (wheels_on_ground, ReadOnly, Transient))

// Registers Vehicle3D and adds the systems (after install_bodies3d, which it calls). Idempotent.
void install_vehicles3d(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
