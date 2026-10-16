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

// The mixer's stock buses as an enum (the BusId values), for components a scene authors. Voice is
// the first user bus, for dialogue lines that other buses duck under (M-AUDIO2 4).
enum class AudioBus : u8 { Master = Bus_Master, Music = Bus_Music, SFX = Bus_SFX, UI = Bus_UI, Voice = Bus_User0 };
MCE_ENUM(AudioBus, Master, Music, SFX, UI, Voice)

struct AudioSource {
    // The cooked Sfx asset, or a sound set (.mcsound: a clip, pitch and gain picked on each start);
    // the bridge resolves it through the ClipBank when the source starts.
    asset::AssetRef<asset::Sfx> clip;
    f32 gain_db = 0.f;
    f32 pan = 0.f;
    u8 priority = 128;
    AudioBus bus = AudioBus::SFX;  // Music streams the clip (two music streams at a time)
    bool loop = false;
    bool play_on_start = false;
    // A clip registered from code (procedural, tests). Used when `clip` has no GUID.
    ClipHandle clip_handle;
    // ---- engine-written, not serialized ----
    bool started = false;
    i8 pick_gain = 0;  // a sound set's gain for this play, in 0.5 dB steps (0 for a plain clip)
    // A sound set's pitch for this play, 4.12 fixed point (0 = 1, so a plain clip's bytes stay zero:
    // World::state_hash hashes component bytes, ADR-0007). Doppler scales it (M-AUDIO2 5).
    u16 pick_rate = 0;
    AudioHandle handle;
    ClipHandle playing_clip;  // what `handle` plays; a hot reload swaps it and restarts loops
    f32 applied_gain_db = 0.f;
    f32 applied_pan = 0.f;
    f32 fade_s = 0.f;  // seconds the voice fades in on start and out on stop (music crossfades)
    f32 fade_t = 0.f;  // engine-written: 0..1 through the fade-in
};
MCE_COMPONENT(AudioSource,
    clip,
    (gain_db, Range{-60, 12}, Help{"source gain in dB"}),
    (pan, Range{-1, 1}),
    priority,
    (bus, Help{"Music streams the clip from its asset instead of holding it decoded"}),
    loop,
    play_on_start,
    (fade_s, Range{0, 30}, Help{"fade in on start and out when stopped or removed (crossfade two music sources)"}))

// Makes the AudioSource on the same entity 3D (FR-AUD-050).
struct AudioSpatial {
    u16 attenuation_id = 1;  // a curve id set on the device (AudioDevice::set_attenuation)
    u16 pad_ = 0;
    // ---- engine-written, not serialized ----
    math::vec3 applied_position;
};
MCE_COMPONENT(AudioSpatial, attenuation_id)

// Occlusion for a 3D source (M-AUDIO2 5): while something in `mask` lies between the listener and the
// source, the source drops by gain_db and loses its highs above lowpass_hz, fading over 100 ms. The
// test is a physics ray from the listener (kit::install_audio_occlusion provides it: 3D physics when
// the world has it, else 2D); without one, nothing is occluded.
struct AudioOcclusion {
    u16 mask = 0xFFFF;
    u16 pad_ = 0;
    f32 gain_db = -12.f;
    f32 lowpass_hz = 800.f;
    // ---- engine-written, not serialized ----
    f32 amount = 0.f;  // 0 = clear .. 1 = fully occluded
};
MCE_COMPONENT(AudioOcclusion,
    (mask, Help{"physics layers that block sound"}),
    (gain_db, Range{-60, 0}),
    (lowpass_hz, Range{20, 20000}))

// Doppler for a 3D source (M-AUDIO2 5): its pitch rises while it closes on the listener and falls
// while it pulls away, from both movements per tick (speed of sound 343 units/s, scaled by factor;
// the shift is held to an octave either way). Streamed music does not shift.
struct AudioDoppler {
    f32 factor = 1.f;
    // ---- engine-written, not serialized ----
    f32 applied = 1.f;  // the shift last sent
};
MCE_COMPONENT(AudioDoppler, (factor, Range{0, 10}, Help{"0 = off, 1 = real-world strength"}))

// The ear (usually on the camera entity). Exactly one is used: the first in query order. Its
// LocalTransform gives position and orientation (forward = rotation * -Z, up = rotation * +Y).
struct AudioListener {
    f32 gain_db = 0.f;
};
MCE_COMPONENT(AudioListener, (gain_db, Range{-60, 12}))

// Bus effects from the scene (M-AUDIO2 3): a bus's low-pass and high-pass, and how much of it feeds
// a reverb unit. One entity per bus (the first in query order wins); removing it puts the bus back
// to dry and unfiltered. Timelines and sequences can key the fields like any component's, e.g. a
// low-pass that closes while the player is underwater.
struct AudioBusEffects {
    AudioBus bus = AudioBus::SFX;
    u8 reverb_unit = 0;
    u8 pad_[2] = {};  // explicit and zeroed: World::state_hash hashes component bytes (ADR-0007)
    f32 lowpass_hz = 0.f;
    f32 highpass_hz = 0.f;
    f32 reverb_send_db = -96.f;
};
MCE_COMPONENT(AudioBusEffects,
    (bus, Help{"the bus these effects run on (Master filters everything; it has no send)"}),
    (lowpass_hz, Range{0, 20000}, Help{"cut above this frequency, 12 dB/octave; 0 = off. Changes glide"}),
    (highpass_hz, Range{0, 20000}, Help{"cut below this frequency, 12 dB/octave; 0 = off"}),
    (reverb_unit, Range{0, 1}, Help{"which AudioReverb the send feeds"}),
    (reverb_send_db, Range{-96, 12}, Help{"post-fader send into the reverb; -96 = none"}))

// A reverb unit (Freeverb) that buses send to with AudioBusEffects; its return goes into Master.
// One entity per unit; removing it turns the unit off.
struct AudioReverb {
    u8 unit = 0;
    u8 pad_[3] = {};
    f32 room_size = 0.5f;
    f32 damping = 0.5f;
    f32 width = 1.f;
    f32 wet_db = 0.f;
};
MCE_COMPONENT(AudioReverb,
    (unit, Range{0, 1}),
    (room_size, Range{0, 1}, Help{"decay length: 0.5 is a room, 0.9 a cave or hall"}),
    (damping, Range{0, 1}, Help{"how fast high frequencies die away"}),
    (width, Range{0, 1}, Help{"0 = mono return, 1 = full stereo"}),
    (wet_db, Range{-96, 12}, Help{"return level"}))

// The master limiter: while one exists, no output sample exceeds the ceiling (5 ms of added latency).
struct AudioLimiter {
    f32 ceiling_db = -1.f;
    f32 release_ms = 80.f;
};
MCE_COMPONENT(AudioLimiter, (ceiling_db, Range{-24, 0}), (release_ms, Range{1, 1000}))

// Sidechain ducking (M-AUDIO2 4): `bus` (Music, SFX or UI) drops by depth_db while `key` plays louder
// than threshold_db, e.g. the music under a voice line. One entity per ducked bus; removing it lets
// the bus come back.
struct AudioDuck {
    AudioBus bus = AudioBus::Music;
    AudioBus key = AudioBus::Voice;
    u8 pad_[2] = {};
    f32 depth_db = -10.f;
    f32 threshold_db = -40.f;
    f32 attack_ms = 20.f;
    f32 release_ms = 400.f;
};
MCE_COMPONENT(AudioDuck,
    (bus, Help{"the bus that ducks: Music, SFX or UI"}),
    (key, Help{"the bus it ducks under (not Master)"}),
    (depth_db, Range{-60, 0}),
    (threshold_db, Range{-96, 0}, Help{"key level that starts the duck"}),
    (attack_ms, Range{0, 2000}),
    (release_ms, Range{0, 5000}))

// A mixer snapshot (M-AUDIO2 4): while active, it fades in over fade_s and changes one bus: a gain
// offset and a low-pass or high-pass (0 = leave as is). Several on the same bus add their gains and
// the tightest filter wins. `while_paused` makes it the pause snapshot: the stock player turns it on
// while its pause overlay is up. Gameplay, sequences and timelines turn the rest on through `active`.
// Fade progress is not sim state: it lives outside the component, so the state hash never moves.
struct AudioSnapshot {
    AudioBus bus = AudioBus::Master;
    bool active = false;
    bool while_paused = false;
    u8 pad_ = 0;
    f32 fade_s = 0.5f;
    f32 gain_db = 0.f;
    f32 lowpass_hz = 0.f;
    f32 highpass_hz = 0.f;
};
MCE_COMPONENT(AudioSnapshot,
    bus,
    active,
    (while_paused, Help{"on while the game is paused (the stock player's pause overlay)"}),
    (fade_s, Range{0, 10}),
    (gain_db, Range{-60, 12}),
    (lowpass_hz, Range{0, 20000}, Help{"0 = leave the bus's low-pass as it is"}),
    (highpass_hz, Range{0, 20000}))

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
    // ---- bridge-written: what the scene's effect components set last tick, so removing one resets it
    u16 fx_buses = 0;      // filters (AudioBusEffects or a snapshot)
    u16 fx_sends = 0;      // AudioBusEffects sends
    u16 fx_trims = 0;      // snapshot gains
    u8 fx_reverbs = 0;
    u8 fx_ducks = 0;
    bool fx_limiter = false;
    // Occlusion (M-AUDIO2 5): whether something in `mask` blocks the straight line from the listener
    // at `from` to the source at `to` (neither entity counts). Null: nothing is occluded.
    using OcclusionTest = bool (*)(World& world, Entity source, Entity listener, math::vec3 from, math::vec3 to, u16 mask);
    OcclusionTest occlusion = nullptr;
    // The listener last tick, for Doppler.
    math::vec3 last_listener{};
    bool have_last_listener = false;
    // Snapshot fades (0..1) by entity: sound state, kept out of the components the sim hashes.
    static constexpr u32 kMaxSnapshots = 16;
    struct SnapshotFade {
        Entity entity;
        f32 weight = 0.f;
    };
    SnapshotFade snapshots[kMaxSnapshots] = {};
    u32 snapshot_count = 0;
};

// Registers AudioSource, AudioSpatial, AudioOcclusion, AudioDoppler, AudioListener, AudioBusEffects, AudioReverb, AudioLimiter, AudioDuck,
// AudioSnapshot, PlaySfx, the AudioRef resource, the remove hook (stop with a 50 ms
// fade) and the audio_bridge system in FixedUpdate_Post. Call after any system named in `after`.
// The host calls clips->sync() once per frame before the sim ticks.
void install(World& world, AudioDevice& device, ClipBank* clips = nullptr, const char* after = nullptr);

// The bridge system itself (exposed for tests and custom pipelines).
void audio_bridge(Frame& f);

// Applies the scene's mix components (AudioBusEffects, AudioReverb, AudioLimiter, AudioDuck,
// AudioSnapshot) to the device and advances snapshot fades by `dt` seconds. The bridge calls it
// every tick with paused = false; a host whose sim is paused calls it every frame with paused =
// true, so the pause snapshot fades in although no tick runs.
void update_mix(World& world, f32 dt, bool paused);

}  // namespace mce::audio
