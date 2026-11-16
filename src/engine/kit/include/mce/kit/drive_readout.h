#pragma once
// Speed and gear readout for the HUD (M-DRIVEHUD). kit::install_stock installs it.
//
//   DriveReadout3D  on the entity that holds a Rider3D (the player): while the rider is seated in a car with a
//                   Vehicle3D, `kmh` is the car's speed in km/h (always >= 0), `gear` its gear (-1 reverse, 0 neutral,
//                   1.. forward) and `rpm` its engine speed. On foot all three are -1000, so a HUD widget bound to
//                   one with a `range` starting above that is hidden (like the wanted stars): the readout shows only
//                   in a car. They are floats so every bind format works.
//
// System: "kit_drive_readout" (FixedUpdate_Pre, after "kit3d_vehicles").

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::kit {

inline constexpr f32 kDriveReadoutOff = -1000.0f;

struct DriveReadout3D {
    f32 kmh = kDriveReadoutOff;   // engine-written
    f32 gear = kDriveReadoutOff;  // engine-written
    f32 rpm = kDriveReadoutOff;   // engine-written
};
MCE_COMPONENT(DriveReadout3D, (kmh, ReadOnly, Transient, Help{"km/h of the car the Rider3D sits in; -1000 on foot"}),
              (gear, ReadOnly, Transient, Help{"-1 reverse, 0 neutral, 1.. forward; -1000 on foot"}),
              (rpm, ReadOnly, Transient, Help{"engine rpm; -1000 on foot"}))

// Registers the component and adds the system. Idempotent.
void install_drive_readout(World& world);

}  // namespace mce::kit
