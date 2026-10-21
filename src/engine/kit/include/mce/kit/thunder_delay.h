#pragma once
// Thunder that arrives late (M-RAINFX 7e). One optional ThunderDelay per scene (the first in ECS order drives).
// install_weather registers the component; install_precipitation adds the system "kit_thunder_delay"
// (FixedUpdate_Pre, after "kit_lightning"), because the LightningStruck event comes from there.
//
//   ThunderDelay  `speed_mps` (343) the speed of sound, `volume` (1) the linear gain at zero distance, `clip` the
//                 thunder Sfx. Engine-written and saved: `pending` (strikes in flight), `due[i]` (ticks left until
//                 strike i sounds) and `gain_db[i]` (its distance-attenuated gain).
//
// Each LightningStruck {thunder_delay_ticks} becomes one delayed one-shot. The event carries no position, only the
// delay the strike was given (30..300 ticks), so the strike distance is that delay at the nominal speed of sound:
//   d = thunder_delay_ticks * dt * 343 m.
// The sound starts n = ceil(d / speed_mps / dt) ticks after the strike tick, the tick whose FixedUpdate_Pre pushed
// the event (a quotient within 1e-6 of an integer counts as that integer, so the default speed gives exactly
// `thunder_delay_ticks`). Events are read one tick after they are pushed, so n of 0 or 1 sounds on the tick after
// the strike, the earliest this system can see it.
// A tick that reaches 0 pushes one audio::PlaySfx (clip, centred, SFX bus); audio_bridge plays it the same tick.
// Attenuation: linear = volume / (1 + d / 100 m), gain_db = 20 log10(linear), floored at -60 dB (a volume of 0
// is -60 dB). Nearer strikes are louder; the curve is a fixed function of d, volume and the clamps below.
//   * No ThunderDelay, no clip (null guid) or no Camera3DComponent in the scene => the strike makes no sound and
//     nothing is queued. The camera is the listener; it is only required to exist.
//   * speed_mps is clamped to [1, 1e6] (NaN: 343); volume to [0, 4] (NaN: 1). The fields are not rewritten.
//   * Capacity 8 strikes in flight. A strike that arrives with 8 pending is dropped (the oldest, which sounds
//     first, are kept). A queue entry counts down once per tick from the tick after the strike.
// All state is integer or an f32 computed from integers and saved, so a save mid-flight resumes on the same state
// hash. Slots past `pending` are zero (state_hash hashes the raw bytes). Scenes without a ThunderDelay are unchanged.

#include "mce/asset/handle.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::kit {

inline constexpr u32 kThunderQueue = 8;  // strikes in flight

struct ThunderDelay {
    f32 speed_mps = 343.0f;
    f32 volume = 1.0f;
    asset::AssetRef<asset::Sfx> clip;
    u32 pending = 0;                  // engine-written: strikes in flight
    u32 pad_ = 0;                     // explicit pad (state_hash hashes whole columns); reserved
    u32 due[kThunderQueue] = {};      // engine-written: ticks until each sounds, oldest first
    f32 gain_db[kThunderQueue] = {};  // engine-written: each one's gain
};
MCE_COMPONENT(ThunderDelay, (speed_mps, Save, Range{1, 1000000}, Help{"speed of sound in metres per second"}),
              (volume, Save, Range{0, 4}, Help{"linear gain of thunder at zero distance"}),
              (clip, Help{"the thunder sound"}), (pending, Save, ReadOnly), (due, Save, ReadOnly),
              (gain_db, Save, ReadOnly))

// Adds the system (after "kit_lightning": call from install_precipitation). Idempotent per world.
void install_thunder_delay(World& world);

}  // namespace mce::kit
