#pragma once
// Crash damage (M-CONTACTEVENTS 4). kit::install_stock installs it.
//
//   ImpactDamage3D  on an entity with Health3D and a RigidBody3D that has `contact_events` on (a car, a crate): every ContactBegin3D
//                   it is in that closes at least `min_speed` m/s hurts it by (speed - min_speed + 1) * damage_per_ms through
//                   apply_damage. The instigator is the other body, or its VehicleSeat3D driver, so a wanted player who rams a
//                   traffic car gets the crime (kit/wanted3d.h). One hit per second at most (Health3D `since_hit`). When the body is a
//                   car with a VehicleSeat3D driver, the driver takes `occupant_share` of the same blow (a crash hurts the person inside).
//
// "kit_impact_damage" runs in FixedUpdate_Pre and reads the previous tick's contact events (apply_damage belongs to that phase).
// A body with ImpactDamage3D is skipped by kit_vehicle_ram, so a car does not take a ram hit and a crash hit for one touch.

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

struct ImpactDamage3D {
    u8 enabled = 1;
    u8 pad_[3] = {};
    f32 min_speed = 5.0f;       // m/s the bodies must close at
    f32 damage_per_ms = 4.0f;   // hp per m/s above min_speed (plus one)
    f32 occupant_share = 0.5f;  // the VehicleSeat3D driver takes this fraction of the same blow (0: none)
};
MCE_COMPONENT(ImpactDamage3D, enabled, (min_speed, Range{0, 200}, Help{"m/s the bodies must close at"}),
              (damage_per_ms, Range{0, 10000}, Help{"hp per m/s above min_speed, plus one"}),
              (occupant_share, Range{0, 1}, Help{"the driver takes this fraction of the same blow; 0 = unhurt"}))

// Undoes kit_wreck: a dead or hurt car is back at full health, drives again (`use_input` 1 when a VehicleSeat3D driver sits in
// it, the handbrake off) and its ParticleEmitter3D (the wreck's smoke) stops emitting. False without a Health3D. From a
// FixedUpdate_Pre system or between ticks.
bool repair_vehicle(World& world, Entity car);

// Registers ImpactDamage3D and adds "kit_impact_damage" (installs the combat kit and the 3D bodies first). Idempotent.
void install_impact_damage(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
