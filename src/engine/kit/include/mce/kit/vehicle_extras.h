#pragma once
// Horn and headlights for a car the player drives (M-OWHORN). kit::install_stock installs it; cars without the
// components are unchanged.
//
//   VehicleHorn3D    on a car with a VehicleSeat3D: while a Rider3D with `use_input` sits in it, the Horn action (or
//                    `honk` set to 1, which the engine clears) pushes one audio::PlaySfx at the car, 3D with
//                    `attenuation_id`, at most once per `min_gap_s`. `honks` counts them (engine-written).
//   VehicleLights3D  on a car with a SpotLight3D (and usually a TimedLight3D): the Lights action cycles `mode`
//                    Auto -> On -> Off. Auto leaves kit_timed_light in charge; On lights the spot at its authored
//                    intensity (TimedLight3D.spot_scale, else the light's own); Off switches it off.
//
// A car with no driver, or one driven by traffic or the police, reads no input. System: "kit_vehicle_extras"
// (FixedUpdate_Pre, after "kit_timed_light").

#include "mce/asset/handle.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

struct VehicleHorn3D {
    asset::AssetRef<asset::Sfx> clip;
    f32 gain_db = -4.0f;
    f32 min_gap_s = 0.35f;
    f32 since_s = 1.0e6f;  // engine-written: seconds since the last honk
    u16 attenuation_id = 1;
    u8 honk = 0;
    u8 pad_ = 0;
    u32 honks = 0;
};
MCE_COMPONENT(VehicleHorn3D, (clip, Help{"the horn sound; null: silent"}), (gain_db, Range{-60, 12}),
              (min_gap_s, Range{0, 10}, Help{"seconds between two honks"}),
              (attenuation_id, Help{"audio attenuation curve of the 3D one-shot"}),
              (honk, Help{"set to 1 to honk once (the engine clears it); the Horn action sets it for the driver"}),
              (since_s, ReadOnly, Transient), (honks, ReadOnly, Transient))

enum class HeadlightMode : u8 { Auto, On, Off };
MCE_ENUM(HeadlightMode, Auto, On, Off)

struct VehicleLights3D {
    HeadlightMode mode = HeadlightMode::Auto;
    u8 pad_[3] = {};
};
MCE_COMPONENT(VehicleLights3D, (mode, Save, Help{"Auto: the day cycle decides; On / Off: the driver's choice (Lights cycles)"}))

// Registers the components and adds the system. Idempotent.
void install_vehicle_extras(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
