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
// Changing any setting other than the driver's inputs, the wheel entities, downforce, skid_slip and
// the self-righting rebuilds the vehicle.
//
// M-MOVE3D 3: anti-roll bars, downforce, gear count and shift points as data; per-wheel slip with
// Skid3D events (FireOn::Skid on the vehicle's effects) past skid_slip; self_right puts a flipped car
// that has nearly stopped back on its wheels after right_after seconds, a metre up, facing the way it did.
//
// M-MOVE3D 4: VehicleDriver3D drives the car around a chain of Waypoint3D entities and counts laps
// (LapCompleted3D, FireOn::Lap).
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
    // Feel (M-MOVE3D 3), each off by default.
    f32 anti_roll = 0.0f;                   // N/m: a bar across each axle against body roll; 0 = none
    f32 downforce = 0.0f;                   // N per (m/s)^2 pressing the car onto its wheels
    u8 gears = 0;                           // forward gears, ratios first_gear .. top_gear; 0 = the stock five
    u8 self_right = 0;                      // 1: put the car back on its wheels after a flip
    u16 pad2_ = 0;
    f32 first_gear = 2.66f;
    f32 top_gear = 0.74f;
    f32 shift_up_rpm = 4000.0f;             // the automatic gearbox shifts up above this rpm
    f32 shift_down_rpm = 2000.0f;           // and down below this
    f32 skid_slip = 0.0f;                   // a grounded wheel above 3 m/s skids past this slip (rad sideways, or
                                            // a braking wheel's lock ratio) until under 70% of it; 0 = off
    f32 right_after = 1.0f;                 // s tilted past ~53 degrees (or upside down), nearly still, before self_right
    u32 skidding = 0;                       // engine-written: a bit per wheel, fl fr rl rr
    f32 lateral_slip = 0.0f;                // engine-written: the largest sideways slip of a grounded wheel, rad
    u16 flipped_ticks = 0;                  // engine-written
    u16 rightings = 0;                      // engine-written: times self_right set it upright
};

// A grounded wheel started to skid (M-MOVE3D 3): FixedUpdate_Physics, from "kit3d_vehicle_state".
struct Skid3D {
    Entity vehicle;
    u32 wheel = 0;          // 0 front left, 1 front right, 2 rear left, 3 rear right
    f32 slip = 0.0f;        // the slip that passed skid_slip
    math::vec3 position;    // the wheel's centre
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
              (rpm, ReadOnly, Transient), (gear, ReadOnly, Transient), (wheels_on_ground, ReadOnly, Transient),
              (anti_roll, Range{0, 1000000}, Help{"N/m across each axle against body roll; 0 = none"}),
              (downforce, Range{0, 1000}, Help{"N per (m/s)^2 pressing the car down"}),
              (gears, Range{0, 10}, Help{"forward gears from first_gear to top_gear; 0 = the stock five"}),
              (self_right, Help{"1: set the car back on its wheels after right_after seconds flipped"}),
              (first_gear, Range{0.1f, 10}), (top_gear, Range{0.1f, 10}), (shift_up_rpm, Range{500, 30000}), (shift_down_rpm, Range{100, 30000}),
              (skid_slip, Range{0, 2}, Help{"a grounded wheel skids past this slip; Skid3D events, FireOn Skid; 0 = off"}),
              (right_after, Range{0, 10}, Help{"seconds flipped and nearly still before self_right acts"}),
              (skidding, ReadOnly, Transient), (lateral_slip, ReadOnly, Transient), (flipped_ticks, ReadOnly, Transient),
              (rightings, ReadOnly, Transient))

// One point of a path a VehicleDriver3D follows (M-MOVE3D 4). Waypoints chain by `next`; the last
// pointing back at the first makes a lap. The entity's position is the point.
struct Waypoint3D {
    Entity next;            // the following waypoint; null ends the path
    f32 speed = 0.0f;       // m/s cap at this point; 0 = from the corner's sharpness
    f32 radius = 4.0f;      // reached within this many metres, or once passed
};
MCE_COMPONENT(Waypoint3D, (next, Help{"the following waypoint; the last pointing back at the first makes a lap"}),
              (speed, Range{0, 200}, Help{"m/s cap here; 0 = from the corner"}), (radius, Range{0.1f, 100}, Help{"metres: reached within this"}))

// An AI driver for the Vehicle3D on the same entity (M-MOVE3D 4). Each tick before the car takes its
// inputs, it writes throttle, steer, brake and handbrake to follow the path from `path`. It steers by
// pure pursuit at a point look_ahead metres down the path, no sharper than 1.5x corner_accel allows at
// its speed. It plans its speed so it can brake at brake_decel to each of the next two corners in
// time (a corner allows sqrt(corner_accel * radius), the radius from the turn's angle and the legs
// beside it, capped by the waypoint's speed) and no faster than the arc to its aim point allows.
// Reaching `path` again completes a lap: LapCompleted3D, FireOn::Lap; after `laps` laps (0 = never)
// it brakes the car to a stop. A tall car needs anti_roll to take the corners flat.
struct VehicleDriver3D {
    Entity path;                // the first waypoint: the start and finish line
    f32 max_speed = 20.0f;      // m/s
    f32 corner_accel = 6.0f;    // m/s^2 sideways it allows in a corner
    f32 brake_decel = 4.0f;     // m/s^2 it plans to brake at
    f32 steer_gain = 1.0f;      // scales the pure-pursuit wheel angle
    f32 look_ahead = 6.0f;      // m down the path it steers at (at least 0.6 s of travel)
    u16 laps = 0;               // laps before it stops; 0 = never
    u8 drive = 1;               // 0: lets go of the car (inputs left as they are)
    u8 pad_ = 0;
    Entity target;              // engine-written: the waypoint it heads for
    Entity previous;            // engine-written: the one it reached last
    f32 target_speed = 0.0f;    // engine-written: m/s it wants now
    f32 lap_time = 0.0f;        // engine-written: s into the current lap
    f32 last_lap = 0.0f;        // engine-written: s
    f32 best_lap = 0.0f;        // engine-written: s, 0 before the first lap
    u16 laps_done = 0;          // engine-written
    u8 finished = 0;            // engine-written: 1 once laps are done
    u8 started = 0;             // engine-written: 1 once it crossed the start
};
MCE_COMPONENT(VehicleDriver3D, (path, Help{"the first waypoint: the start and finish line"}), (max_speed, Range{0, 200}, Help{"m/s"}),
              (corner_accel, Range{0.1f, 100}, Help{"m/s^2 sideways allowed in a corner"}),
              (brake_decel, Range{0.1f, 100}, Help{"m/s^2 it plans to brake at"}), (steer_gain, Range{0, 20}),
              (look_ahead, Range{0.5f, 100}, Help{"metres down the path it steers at (at least 0.6 s of travel)"}),
              (laps, Help{"laps before it stops; 0 = never"}), (drive, Help{"0: lets go of the car"}), (target, ReadOnly, Transient),
              (previous, ReadOnly, Transient), (target_speed, ReadOnly, Transient), (lap_time, ReadOnly, Transient),
              (last_lap, ReadOnly, Transient), (best_lap, ReadOnly, Transient), (laps_done, ReadOnly, Transient),
              (finished, ReadOnly, Transient), (started, ReadOnly, Transient))

// A VehicleDriver3D crossed its start again (M-MOVE3D 4): FixedUpdate_Pre, from "kit3d_vehicles".
struct LapCompleted3D {
    Entity vehicle;
    u32 lap = 0;            // 1 for the first
    f32 time = 0.0f;        // s
};

// Registers Vehicle3D, Waypoint3D and VehicleDriver3D and adds the systems (after install_bodies3d, which it calls). Idempotent.
void install_vehicles3d(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
