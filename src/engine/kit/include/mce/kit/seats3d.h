#pragma once
// Getting in and out of vehicles (M-OPENWORLD C1): a walking character boards a car, drives it with
// the player's input, and steps out again. kit::install_stock installs it.
//
//   VehicleSeat3D  on a Vehicle3D's entity: where the driver sits, where they step out, how close
//                  a rider must be to board
//   Rider3D        on a character (CharacterController3D): boards the nearest free seat in reach and
//                  leaves it again, by the Interact action or by setting `enter`
//
// Boarding: with `use_input` the Interact action (pressed), or `enter` set to 1 (the engine clears it),
// boards the nearest VehicleSeat3D without a driver within `enter_radius` of the seat or the exit
// point. The rider's CharacterController3D is taken off the entity (its settings are kept and come
// back on exit), so the capsule no longer collides, and the rider is carried at the seat pose every
// tick. The car gets `Vehicle3D.use_input = 1` (the player's Move / Jump / TriggerL drive it) and an AI
// VehicleDriver3D on it is switched off while someone sits there. `camera` (a CameraFollow3D entity,
// optional) follows the car while seated and the rider again afterwards.
//
// Leaving: the same Interact / `enter` while seated. The car must be slower than `max_exit_speed`,
// else nothing happens. The rider steps out at `exit` (body space), the mirrored side, or above the
// roof, whichever is the first with room (a sphere overlap against static and dynamic bodies, the
// car excluded). The car gets its handbrake on, its input mode and AI driver back. If the car is
// destroyed while seated, or the rider is, the seat is freed (the rider is put back where it sits).
//
// A rider is not saved mid-ride: its controller settings are held by the engine, and a reloaded rider
// that was seated gets a default CharacterController3D when it steps out.
//
// Events: VehicleEntered3D, VehicleExited3D (producer FixedUpdate_Pre).
//
// Systems: "kit3d_seats" (FixedUpdate_Pre, after "kit3d_vehicles": boarding, leaving, cleanup) and
// "kit3d_seat_pose" (FixedUpdate_Physics, after "kit3d_vehicle_state": the rider at the seat).

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

struct VehicleSeat3D {
    math::vec3 seat{0.0f, 0.3f, 0.0f};      // body space: where the driver sits
    math::vec3 exit{-1.6f, 0.0f, 0.0f};     // body space: where the driver steps out (the mirrored x is tried next)
    f32 enter_radius = 2.5f;                // m from the seat or the exit point within which a rider can board
    f32 max_exit_speed = 3.0f;              // m/s: faster than this and the rider stays in
    u8 locked = 0;                          // 1: nobody can board
    u8 saved = 0;                           // engine-written: what boarding switched off (bit 0 use_input, bit 1 AI driver)
    u16 pad_ = 0;
    Entity driver;                          // engine-written: who sits there (null: free)
};
MCE_COMPONENT(VehicleSeat3D, (seat, Help{"body space: where the driver sits"}),
              (exit, Help{"body space: where the driver steps out; the mirrored x is tried next, then above"}),
              (enter_radius, Range{0.1f, 50}, Help{"metres from the seat or the exit point"}),
              (max_exit_speed, Range{0, 200}, Help{"m/s: faster and the rider stays in"}), (locked, Help{"1: nobody can board"}),
              (saved, ReadOnly, Transient), (driver, ReadOnly, Transient))

struct Rider3D {
    u8 use_input = 0;       // 1: the Interact action boards and leaves
    u8 enter = 0;           // set to 1 to board the nearest seat or leave the one it sits in (the engine clears it)
    u8 seated = 0;          // engine-written
    u8 pad_ = 0;
    Entity vehicle;         // engine-written: the car it sits in
    Entity camera;          // optional: a CameraFollow3D entity that follows the car while seated
};
MCE_COMPONENT(Rider3D, (use_input, Help{"1: the Interact action boards and leaves"}),
              (enter, Help{"set to 1 to board or leave (the engine clears it)"}), (seated, ReadOnly, Transient),
              (vehicle, ReadOnly, Transient), (camera, Help{"a CameraFollow3D entity: follows the car while seated"}))

// Events (producer FixedUpdate_Pre).
struct VehicleEntered3D {
    Entity rider;
    Entity vehicle;
};
struct VehicleExited3D {
    Entity rider;
    Entity vehicle;
};
MCE_EVENT(VehicleEntered3D, rider, vehicle)
MCE_EVENT(VehicleExited3D, rider, vehicle)

// Registers VehicleSeat3D, Rider3D and the events and adds the systems (after install_vehicles3d, which it
// calls). Idempotent.
void install_seats3d(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
