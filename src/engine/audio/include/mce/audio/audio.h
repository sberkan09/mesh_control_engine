#pragma once
// mce_audio — main-thread API (audio-input-ui §2.3). Everything here enqueues POD commands into
// the SPSC FIFO the device callback drains; nothing blocks on the audio thread. Main thread only.
//
// MVP tier (ROADMAP M3): 8 real voices + virtual voices, one-shot/loop, gain/pan, streamed music
// loop, bus gains, 60 Hz command granularity. v0.5 adds 48 voices, sample-accurate play_at, DSP
// chains, 3D spatialization (AudioListener) — the seams below are sized for them.

#include "mce/memory/allocator.h"
#include "mce/memory/handle.h"
#include "mce/result.h"

namespace mce::audio {

inline constexpr u32 kSampleRate = 48000;  // internal mix rate; clips are cooked to it
inline constexpr u32 kChannels = 2;        // internal mix is stereo float32
inline constexpr u32 kMaxRealVoices = 8;   // MVP tier (A7); 48 at v0.5 — a constant, not a design change
inline constexpr u32 kMaxVirtualVoices = 248;
inline constexpr u32 kMaxHandles = kMaxRealVoices + kMaxVirtualVoices;
inline constexpr u32 kMaxClips = 1024;
inline constexpr u32 kMaxStreams = 2;           // concurrent streamed clips (music, ambience)
inline constexpr u32 kStreamPageFrames = 4800;  // 100 ms
inline constexpr u32 kStreamPages = 4;          // 400 ms ring per stream
inline constexpr u8 kProtectedPriority = 32;    // voices below this are only stolen by peers

using BusId = u8;
enum : BusId { Bus_Master = 0, Bus_Music = 1, Bus_SFX = 2, Bus_UI = 3 };
inline constexpr u32 kBusCount = 4;  // user buses 4..11 join with the v0.5 bus graph

using ClipHandle = Handle<class AudioClip>;  // generational; stale handles are silent no-ops
using AudioHandle = Handle<class Voice>;     // gameplay-visible voice handle

// A cooked clip as the mixer reads it: 48 kHz float32 frames, mono or interleaved stereo. The
// memory belongs to the caller (assets' mapped chunk) and must stay valid until release_clip's
// release completes (is_clip_registered() turns false after update()).
struct ClipPayload {
    const f32* frames = nullptr;
    u32 frame_count = 0;
    u32 channels = 1;            // 1 or 2
    u32 sample_rate = kSampleRate;
    u32 loop_start = 0;          // frame a looping voice wraps back to (intro + loop body)
};

struct PlayParams {
    f32 gain_db = 0.f;
    f32 pan = 0.f;        // -1..1; mono: constant power, stereo: balance
    bool loop = false;
    u8 priority = 128;    // 0 = most important; steal order is priority, then quieter, then older
    BusId bus = Bus_SFX;
};

enum class Backend : u8 {
    Device,   // the platform audio output (SDL3 callback thread)
    Offline,  // no device: the caller pulls blocks with render_offline (tests, offline renders)
    Silent,   // no device: update() mixes and discards one block per call (headless, failed open)
};

struct AudioDesc {
    Backend backend = Backend::Device;
    const char* driver = nullptr;  // platform audio driver override (e.g. "dummy")
    u32 period_frames = 480;       // requested device period
    u32 command_capacity = 1024;   // FIFO depth (power of two)
    bool bind_convars = false;     // drive bus gains from audio.*_db convars
    // When the device cannot open, fall back to Silent (the game runs mute + one error) instead
    // of failing create() — FR-DX-050's failure taxonomy.
    bool silent_on_failure = true;
    mem::IAllocator* allocator = nullptr;  // null = heap()
};

struct AudioStats {
    u32 voices_real = 0;
    u32 voices_virtual = 0;
    u64 steals = 0;          // cumulative
    u64 dropped = 0;         // plays that found no real or virtual slot
    u64 underruns = 0;       // stream pages the filler had not delivered in time
    u64 commands_dropped = 0;  // FIFO-full commands (play() returns null for these)
    u32 device_recoveries = 0;
    f32 master_peak = 0.f;   // linear peak of the most recent block
    u64 mix_sample = 0;      // frames mixed since open: the mixer's authoritative clock
};

class AudioDevice {
public:
    static Result<mem::UniquePtr<AudioDevice>> create(const AudioDesc& desc = {});
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    Backend backend() const { return backend_; }

    // ---- clips ------------------------------------------------------------------------
    // In-memory SFX: the mixer reads the payload frames directly.
    Result<ClipHandle> register_clip(const ClipPayload& clip);
    // Streamed music/dialog: pages are filled ahead of the play cursor off the audio thread
    // (update(), later the IO worker) into a ~100 ms page ring; loops are gapless by construction.
    Result<ClipHandle> register_stream(const ClipPayload& clip);
    // Stops every voice on the clip and frees the slot once the mixer acknowledges (update()).
    void release_clip(ClipHandle clip);
    bool is_clip_registered(ClipHandle clip) const;

    // ---- playback ---------------------------------------------------------------------
    // Returns a null handle when the clip is stale, the FIFO is full, or no handle is free (one
    // warning per frame). A streamed clip plays on one voice at a time.
    AudioHandle play(ClipHandle clip, const PlayParams& params = {});
    void stop(AudioHandle voice, f32 fade_s = 0.f);  // always ramps (≥ 2 ms): no click
    void set_handle_gain(AudioHandle voice, f32 gain_db);
    void set_handle_pan(AudioHandle voice, f32 pan);
    // True until the mixer reports the voice finished (one-shot end, stop, steal); updated by
    // update(). A stolen handle fails cleanly — handles are generational and never alias.
    bool is_playing(AudioHandle voice) const;

    // ---- mixer surface --------------------------------------------------------------------
    void set_bus_gain(BusId bus, f32 gain_db);  // smooth-ramped
    f32 bus_gain_db(BusId bus) const { return bus < kBusCount ? bus_db_[bus] : 0.f; }

    // ---- per frame (main thread) ---------------------------------------------------------------
    // Drains mixer replies (finished voices, released clips), refills stream pages, and — on the
    // Silent backend — mixes one block.
    void update();

    // Offline backend only: runs the exact callback body for `frames` stereo frames.
    void render_offline(f32* out_interleaved, u32 frames);

    AudioStats stats() const;
    // Order-sensitive hash of every sim-relevant command issued (play/stop/gain/pan/bus). The
    // determinism harness hashes audio *commands*, not audio output (§4.7).
    u64 command_hash() const { return command_hash_; }
    u32 live_handles() const { return kMaxHandles - free_handle_count_; }

    struct Impl;
    AudioDevice() = default;  // use create()

private:
    friend struct Impl;

    Backend backend_ = Backend::Silent;
    Impl* impl_ = nullptr;
    f32 bus_db_[kBusCount] = {};
    u64 command_hash_ = 0;
    u32 free_handle_count_ = kMaxHandles;
};

// dB <-> linear helpers (gain_db <= -96 is silence).
f32 db_to_linear(f32 db);
f32 linear_to_db(f32 lin);

}  // namespace mce::audio
