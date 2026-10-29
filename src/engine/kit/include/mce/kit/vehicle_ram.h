#pragma once
// Cars that hurt what they hit (M-VEHICLEHIT, GTA-slice gap: nothing took damage from a car). kit::install_stock installs it.
//
//   VehicleRam3D   on an entity with a Vehicle3D: a zone in front of it (or behind it while reversing) that hurts every living
//                  Health3D inside it while the car goes at least `min_speed` m/s.
//
// The zone is `zone_start` to `zone_end` metres from the car's centre along its travel direction and `half_width` to each side
// (flat: height is ignored), so the people inside the car, who sit nearer than `zone_start`, are never hit. A victim takes
// (speed - min_speed + 1) * damage_per_ms through apply_damage, once per second at most from the same car (Health3D `since_hit` and
// `last_attacker`). The instigator is the car's driver (VehicleSeat3D::driver) when there is one, else the car, so a wanted player
// who runs a civilian over gets the crime (kit/wanted3d.h) and traffic cars hurt without one. The blow carries the car's travel
// direction and an impulse of speed * `impulse_per_ms` for a ragdoll.
//
// "kit_vehicle_ram" runs in FixedUpdate_Pre after the vehicles; everything is in the components, so it replays and hashes.

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

struct VehicleRam3D {
    u8 enabled = 1;
    u8 pad_[3] = {};
    f32 min_speed = 4.0f;       // m/s: slower than this hurts nobody
    f32 damage_per_ms = 8.0f;   // hp per m/s above min_speed (plus one)
    f32 zone_start = 1.8f;      // m from the centre where the hurting zone begins
    f32 zone_end = 3.0f;        // m from the centre where it ends
    f32 half_width = 1.1f;      // m to each side
    f32 impulse_per_ms = 4.0f;  // N*s per m/s given to a ragdoll
};
MCE_COMPONENT(VehicleRam3D, enabled, (min_speed, Range{0, 200}, Help{"m/s: slower than this hurts nobody"}),
              (damage_per_ms, Range{0, 10000}, Help{"hp per m/s above min_speed, plus one"}), (zone_start, Range{0, 20}),
              (zone_end, Range{0, 20}, Help{"m from the centre along the travel direction"}), (half_width, Range{0.1f, 10}),
              (impulse_per_ms, Range{0, 1000}))

// Registers VehicleRam3D and adds "kit_vehicle_ram" (installs the combat kit and the vehicles first). Idempotent.
void install_vehicle_ram(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
