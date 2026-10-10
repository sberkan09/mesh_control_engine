#pragma once
// The ECS surface of audio (audio-input-ui §2.4): the AudioSource starter-kit component, the
// PlaySfx one-shot event, and the audio_bridge system — the ONLY code that turns ECS state into
// audio commands. It runs in FixedUpdate_Post, so audio commands are a deterministic function
// of sim state (and are hashed by the determinism harness, §4.7).
//
// v0.5 (FR-AUD-050): AudioListener + AudioSpatial. A source whose entity also has AudioSpatial
// plays in 3D from its LocalTransform position with that attenuation curve; the bridge follows it
// every tick (the device re-sends gain/pan only when they change). 3D is a separate component so
// 2D sources keep their exact M3 layout (and the determinism hashes that fold component bytes).

#include "mce/audio/audio.h"
#include "mce/audio/clip_bank.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/refl/macros.h"

namespace mce::audio {

struct AudioSource {
    // The cooked Sfx asset; the bridge resolves it through the ClipBank when the source starts.
    asset::AssetRef<asset::Sfx> clip;
    f32 gain_db = 0.f;
    f32 pan = 0.f;
    u8 priority = 128;
    BusId bus = Bus_SFX;
    bool loop = false;
    bool play_on_start = false;
    // A clip registered from code (procedural, tests). Used when `clip` has no GUID.
    ClipHandle clip_handle;
    // ---- engine-written, not serialized ----
    bool started = false;
    u8 pad_[3] = {};  // explicit and zeroed: World::state_hash hashes component bytes (ADR-0007)
    AudioHandle handle;
    ClipHandle playing_clip;  // what `handle` plays; a hot reload swaps it and restarts loops
    f32 applied_gain_db = 0.f;
    f32 applied_pan = 0.f;
};
MCE_COMPONENT(AudioSource,
    clip,
    (gain_db, Range{-60, 12}, Help{"source gain in dB"}),
    (pan, Range{-1, 1}),
    priority,
    bus,
    loop,
    play_on_start)

// Makes the AudioSource on the same entity 3D (FR-AUD-050).
struct AudioSpatial {
    u16 attenuation_id = 1;  // a curve id set on the device (AudioDevice::set_attenuation)
    u16 pad_ = 0;
    // ---- engine-written, not serialized ----
    math::vec3 applied_position;
};
MCE_COMPONENT(AudioSpatial, attenuation_id)

// The ear (usually on the camera entity). Exactly one is used: the first in query order. Its
// LocalTransform gives position and orientation (forward = rotation * -Z, up = rotation * +Y).
struct AudioListener {
    f32 gain_db = 0.f;
};
MCE_COMPONENT(AudioListener, (gain_db, Range{-60, 12}))

// Fire-and-forget one-shot from gameplay (pickups, hits, footsteps): push in FixedUpdate_Pre;
// the bridge plays it in FixedUpdate_Post of the same tick.
struct PlaySfx {
    asset::Guid clip;        // cooked Sfx asset (loaded on first use)
    ClipHandle clip_handle;  // or a clip registered from code, used when `clip` is null
    f32 gain_db = 0.f;
    f32 pan = 0.f;
    u8 priority = 128;
    BusId bus = Bus_SFX;
    u16 attenuation_id = 0;  // 3D one-shot at `position` when non-zero
    math::vec3 position;
};

// World resource giving systems the device and the GUID -> clip bank (null: only clip_handle
// sources and events play).
struct AudioRef {
    AudioDevice* device = nullptr;
    ClipBank* clips = nullptr;
};

// Registers AudioSource, AudioSpatial, AudioListener, PlaySfx, the AudioRef resource, the remove hook (stop with a 50 ms
// fade) and the audio_bridge system in FixedUpdate_Post. Call after any system named in `after`.
// The host calls clips->sync() once per frame before the sim ticks.
void install(World& world, AudioDevice& device, ClipBank* clips = nullptr, const char* after = nullptr);

// The bridge system itself (exposed for tests and custom pipelines).
void audio_bridge(Frame& f);

}  // namespace mce::audio
