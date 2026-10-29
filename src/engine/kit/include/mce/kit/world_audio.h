#pragma once
// The sounds of a driving-and-shooting game as components (M-OPENWORLD E3): an engine that follows the car's speed, a
// shot for a weapon, footsteps for a walker and a siren that follows the wanted level. They sit on the entities that
// already carry the gameplay (Vehicle3D, Weapon3D, CharacterController3D) next to an ordinary AudioSource, so a scene
// authors the whole mix as data. kit::install_stock installs them; one system, "kit_world_audio" (FixedUpdate_Post,
// after audio_bridge, so a source's voice exists when its pitch is set).
//
//   EngineAudio    on a Vehicle3D entity with a looping, play_on_start AudioSource (and AudioSpatial for a 3D engine). The
//                  engine runs while a driver sits in its VehicleSeat3D or the car moves faster than 0.5 m/s, otherwise it
//                  is silent (`parked_db`). With |speed| / `top_speed` as t in 0..1: pitch = idle_pitch + t * (top_pitch -
//                  idle_pitch) and gain = idle_db + t * (top_db - idle_db). The system writes AudioSource::gain_db and
//                  AudioSource::pick_rate and sets the live voice's pitch, so do not add AudioDoppler to the same source.
//   WeaponAudio    on a Weapon3D entity: each WeaponFired3D naming it plays `shot` (a sound set picks a variant and pitch)
//                  at the muzzle, in 3D with `attenuation_id` (0: centred, 2D).
//   FootstepAudio  on a CharacterController3D entity: while it is grounded and not seated in a car, every `stride` metres
//                  walked plays `clip` (a sound set) at its feet. Faster walking steps faster.
//   SirenAudio     on a looping AudioSource: silent (-60 dB) until the first Wanted3D reaches `min_level`, then `active_db`;
//                  the gain slides at `slew_db_per_s`.
//
// Sounds are a function of sim state (speed, events, wanted level): no clock and no randomness, so replays play the same
// and a world without audio resources (a headless sim) is left untouched. The component fields the system writes are
// engine-written and part of the state hash by design, like WeatherAudio's.

#include "mce/asset/handle.h"
#include "mce/audio/components.h"
#include "mce/ecs/entity.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::kit {

struct EngineAudio {
    f32 idle_pitch = 0.8f;
    f32 top_pitch = 2.2f;
    f32 top_speed = 28.0f;
    f32 idle_db = -12.0f;
    f32 top_db = 0.0f;
    f32 parked_db = -60.0f;
    f32 applied_pitch = 1.0f;  // engine-written: the rate last sent to the voice
};
MCE_COMPONENT(EngineAudio, (idle_pitch, Range{0.25f, 4}, Help{"playback rate at a standstill"}),
              (top_pitch, Range{0.25f, 4}, Help{"playback rate at top_speed"}),
              (top_speed, Range{1, 200}, Help{"m/s where pitch and gain reach their top"}),
              (idle_db, Range{-60, 12}, Help{"gain at a standstill with the engine running"}),
              (top_db, Range{-60, 12}, Help{"gain at top_speed"}),
              (parked_db, Range{-60, 12}, Help{"gain with nobody aboard and the car standing"}),
              (applied_pitch, ReadOnly, Transient))

struct WeaponAudio {
    asset::AssetRef<asset::Sfx> shot;
    f32 gain_db = 0.0f;
    u16 attenuation_id = 1;
    u16 pad_ = 0;
    u32 fired = 0;  // engine-written: shots played
    u32 pad2_ = 0;
};
MCE_COMPONENT(WeaponAudio, (shot, Help{"an Sfx or a sound set (.mcsound) played for each shot"}), (gain_db, Range{-60, 12}),
              (attenuation_id, Help{"3D curve id at the muzzle; 0 = centred"}), (fired, ReadOnly, Transient))

struct FootstepAudio {
    asset::AssetRef<asset::Sfx> clip;
    f32 stride = 1.8f;
    f32 gain_db = -6.0f;
    u16 attenuation_id = 1;
    u16 pad_ = 0;
    f32 travelled = 0.0f;  // engine-written: metres since the last step
    u32 steps = 0;         // engine-written: steps played
    u32 pad2_ = 0;
};
MCE_COMPONENT(FootstepAudio, (clip, Help{"an Sfx or a sound set (.mcsound) played for each step"}),
              (stride, Range{0.2f, 10}, Help{"metres walked per step"}), (gain_db, Range{-60, 12}),
              (attenuation_id, Help{"3D curve id at the feet; 0 = centred"}), (travelled, ReadOnly, Transient),
              (steps, ReadOnly, Transient))

struct SirenAudio {
    u8 min_level = 1;
    u8 pad_[3] = {};
    f32 active_db = -6.0f;
    f32 slew_db_per_s = 30.0f;
    f32 current_db = -60.0f;  // engine-written: the gain last set
};
MCE_COMPONENT(SirenAudio, (min_level, Range{1, 10}, Help{"wanted level where the siren starts"}), (active_db, Range{-60, 12}),
              (slew_db_per_s, Range{1, 600}, Help{"how fast the gain slides"}), (current_db, ReadOnly, Transient))

// Registers the four components and adds "kit_world_audio". Idempotent.
void install_world_audio(World& world);

}  // namespace mce::kit
