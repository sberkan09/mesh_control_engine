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
//   BrakeLight3D     on a child lamp entity of a car (a ChildOf the car, with a PointLight3D): while the car brakes (its
//                    `brake` or `handbrake`, the player's brake trigger or handbrake key, or the throttle against the way it
//                    rolls) the lamp is lit at `braking` in `brake_color`; in reverse (gear < 0) and not braking it shows `reverse` in
//                    `reverse_color` (0: not used); otherwise `idle` (0: off), or `headlight_idle` while the parent car's SpotLight3D headlight
//                    is on (0: not used). `lit` is 1 while braking, 2 while reversing.
//
//   LightFlasher3D   on an entity with a PointLight3D (a police light bar, an ambulance): the light alternates `color_a` and
//                    `color_b` every half `period_s`, each half starting at `intensity` and fading toward 30% of it. With
//                    `follow_siren` 1 it runs only while its parent car's SirenAudio is audible (the wanted level reached the
//                    siren's `min_level`) and is dark otherwise.
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

struct BrakeLight3D {
    f32 braking = 4.0f;
    f32 idle = 0.0f;
    f32 reverse = 0.0f;  // intensity while the car is in reverse and not braking; 0: the lamp stays at `idle`
    u8 lit = 0;          // engine-written: 1 braking, 2 reversing
    u8 pad_[3] = {};
    f32 headlight_idle = 0.0f;  // intensity when not braking or reversing and the parent's headlight is on; 0: not used
    math::Color brake_color{1.0f, 0.04f, 0.02f, 1.0f};
    math::Color reverse_color{1.0f, 0.95f, 0.85f, 1.0f};
};
MCE_COMPONENT(BrakeLight3D, (braking, Range{0, 100}, Help{"PointLight3D intensity while the parent car brakes"}),
              (idle, Range{0, 100}, Help{"intensity otherwise; 0 switches the lamp off"}),
              (reverse, Range{0, 100}, Help{"intensity while the parent car is in reverse and not braking; 0: not used"}),
              (lit, ReadOnly, Transient),
              (headlight_idle, Range{0, 100}, Help{"intensity while the parent car's headlight is on and the lamp is otherwise idle; 0: not used"}),
              brake_color, reverse_color)

struct LightFlasher3D {
    math::Color color_a{1.0f, 0.05f, 0.04f, 1.0f};
    math::Color color_b{0.1f, 0.2f, 1.0f, 1.0f};
    f32 intensity = 12.0f;
    f32 period_s = 0.5f;
    f32 t = 0.0f;  // engine-written: seconds since the start
    u8 follow_siren = 0;
    u8 pad_[3] = {};
};
MCE_COMPONENT(LightFlasher3D, color_a, color_b, (intensity, Range{0, 100}), (period_s, Range{0.05f, 10}, Help{"one A then one B"}),
              (t, ReadOnly, Transient), (follow_siren, Range{0, 1}, Help{"1: only while the parent car's SirenAudio is audible"}))

// Registers the components and adds the system. Idempotent.
void install_vehicle_extras(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
