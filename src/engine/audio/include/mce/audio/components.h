#pragma once
// The ECS surface of audio (audio-input-ui §2.4): the AudioSource starter-kit component, the
// PlaySfx one-shot event, and the audio_bridge system — the ONLY code that turns ECS state into
// audio commands. It runs in FixedUpdate_Post, so audio commands are a deterministic function
// of sim state (and are hashed by the determinism harness, §4.7).
//
// AudioListener and 3D attenuation join with spatialization at v0.5 (FR-AUD-050).

#include "mce/audio/audio.h"
#include "mce/ecs/world.h"
#include "mce/refl/macros.h"

namespace mce::audio {

struct AudioSource {
    // Resolved from the clip's asset GUID at scene load (assets hook); not authored directly.
    ClipHandle clip;
    f32 gain_db = 0.f;
    f32 pan = 0.f;
    u8 priority = 128;
    BusId bus = Bus_SFX;
    bool loop = false;
    bool play_on_start = false;
    // ---- engine-written, not serialized ----
    bool started = false;
    AudioHandle handle;
    f32 applied_gain_db = 0.f;
    f32 applied_pan = 0.f;
};
MCE_COMPONENT(AudioSource,
    (gain_db, Range{-60, 12}, Help{"source gain in dB"}),
    (pan, Range{-1, 1}),
    priority,
    bus,
    loop,
    play_on_start)

// Fire-and-forget one-shot from gameplay (pickups, hits, footsteps): push in FixedUpdate_Pre;
// the bridge plays it in FixedUpdate_Post of the same tick.
struct PlaySfx {
    ClipHandle clip;
    f32 gain_db = 0.f;
    f32 pan = 0.f;
    u8 priority = 128;
    BusId bus = Bus_SFX;
};

// World resource giving systems the device.
struct AudioRef {
    AudioDevice* device = nullptr;
};

// Registers AudioSource, PlaySfx, the AudioRef resource, the remove hook (stop with a 50 ms
// fade) and the audio_bridge system in FixedUpdate_Post. Call after any system named in `after`.
void install(World& world, AudioDevice& device, const char* after = nullptr);

// The bridge system itself (exposed for tests and custom pipelines).
void audio_bridge(Frame& f);

}  // namespace mce::audio
