#pragma once
// mce_audio — main-thread API (audio-input-ui §2.3). Everything here enqueues POD commands into
// the SPSC FIFO the device callback drains; nothing blocks on the audio thread. Main thread only.
//
// MVP tier (ROADMAP M3): virtual voices, one-shot/loop, gain/pan, streamed music loop, bus gains.
// v0.5 (ROADMAP M6, ADR-0026): 48 real voices, a bus graph (Master/Music/SFX/UI + 8 user buses)
// with a per-bus RBJ biquad EQ, a look-ahead master limiter, sample-accurate play_at on the mixer
// clock, and 3D spatialization (listener + cooked attenuation curves, computed at command time).

#include "mce/math/math.h"
#include "mce/memory/allocator.h"
#include "mce/memory/handle.h"
#include "mce/result.h"

namespace mce::audio {

inline constexpr u32 kSampleRate = 48000;  // internal mix rate; clips are cooked to it
inline constexpr u32 kChannels = 2;        // internal mix is stereo float32
inline constexpr u32 kMaxRealVoices = 48;  // v0.5 tier (A7: 8 at MVP — a constant, not a design change)
inline constexpr u32 kMaxVirtualVoices = 208;
inline constexpr u32 kMaxHandles = kMaxRealVoices + kMaxVirtualVoices;
inline constexpr u32 kMaxClips = 1024;
inline constexpr u32 kMaxStreams = 2;           // concurrent streamed clips (music, ambience)
inline constexpr u32 kStreamPageFrames = 4800;  // 100 ms
inline constexpr u32 kStreamPages = 4;          // 400 ms ring per stream
inline constexpr u8 kProtectedPriority = 32;    // voices below this are only stolen by peers

using BusId = u8;
enum : BusId { Bus_Master = 0, Bus_Music = 1, Bus_SFX = 2, Bus_UI = 3, Bus_User0 = 4 };
inline constexpr u32 kBusCount = 12;     // Master/Music/SFX/UI + user buses 4..11
inline constexpr u32 kEqBands = 4;       // biquads per bus, run in band order
inline constexpr u32 kMaxAttenuationCurves = 32;  // id 0 = 2D (pan only)
inline constexpr u32 kLimiterLookahead = 240;     // 5 ms at 48 kHz: the limiter's added latency

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
    f32 pan = 0.f;        // -1..1; mono: constant power, stereo: balance. Ignored by 3D voices.
    bool loop = false;
    u8 priority = 128;    // 0 = most important; steal order is priority, then quieter, then older
    BusId bus = Bus_SFX;
    // 3D (FR-AUD-050): a non-zero id spatializes the voice against the listener with that
    // attenuation curve; `position` is the source in world space.
    u16 attenuation_id = 0;
    math::vec3 position;
};

// ---- DSP (FR-AUD-060) ----------------------------------------------------------------------------

// One RBJ Audio-EQ-Cookbook band. Coefficients are computed on the main thread (biquad_coeffs).
struct BiquadDesc {
    enum class Type : u8 { Off, LowPass, HighPass, Peaking, LowShelf, HighShelf };
    Type type = Type::Off;
    f32 freq_hz = 1000.f;
    f32 q = 0.7071f;     // shelves: the cookbook's S slope when <= 1 is wanted, pass q = 0.7071
    f32 gain_db = 0.f;   // Peaking/shelves only
};

// Normalized (a0 = 1) direct-form coefficients: y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2.
struct BiquadCoeffs {
    f32 b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f;
};
BiquadCoeffs biquad_coeffs(const BiquadDesc& d, f32 sample_rate = static_cast<f32>(kSampleRate));
// |H(e^jw)| of a band at `freq_hz` (tests, editor curve preview).
f32 biquad_magnitude(const BiquadCoeffs& c, f32 freq_hz, f32 sample_rate = static_cast<f32>(kSampleRate));

// Look-ahead peak limiter on the master bus: a 5 ms delay line, a sliding-window peak hold and a
// box-smoothed gain that reaches the needed reduction before the peak leaves the delay line, so
// no output sample exceeds the ceiling. Off by default (it adds kLimiterLookahead - 1 frames of
// latency); enable it once at init — toggling it later restarts the delay line.
struct LimiterDesc {
    bool enabled = false;
    f32 ceiling_db = -1.f;
    f32 release_ms = 80.f;
};

// ---- 3D spatialization (FR-AUD-050) -------------------------------------------------------------

// Distance -> gain. Distances are clamped to [min_distance, max_distance] (OpenAL's clamped models).
struct AttenuationCurve {
    enum class Model : u8 { Inverse, Linear, Exponential };
    Model model = Model::Inverse;
    f32 min_distance = 1.f;   // full gain inside this radius
    f32 max_distance = 50.f;  // gain stops changing beyond it (Linear reaches 0 here at rolloff 1)
    f32 rolloff = 1.f;
};
f32 attenuation_gain(const AttenuationCurve& c, f32 distance);

// The ear: usually the camera. `right` = cross(forward, up). The default looks down -Z with +Y up,
// so a 2D game in the XY plane pans along +X.
struct Listener {
    math::vec3 position;
    math::vec3 forward{0.f, 0.f, -1.f};
    math::vec3 up{0.f, 1.f, 0.f};
    f32 gain_db = 0.f;  // applied to 3D voices only
};

struct Spatial {
    f32 gain = 1.f;  // linear attenuation (listener gain included)
    f32 pan = 0.f;   // -1..1, from the source direction projected on the listener's right axis
};
// Gain from the curve; pan = sin(azimuth), faded toward centre inside min_distance so a source
// passing through the listener does not flip sides.
Spatial spatialize(const Listener& l, const AttenuationCurve& c, math::vec3 source);

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
    f32 limiter_gain = 1.f;  // lowest limiter gain applied in the most recent block (1 = idle)
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
    // Blocks until the mixer has acknowledged every release_clip so far (or `timeout_ms` passes;
    // false then), so the callers can free clip memory, e.g. before a pack unmounts. Mixes no
    // audio: on the Silent and Offline backends it only drains the command FIFO.
    bool finish_releases(u32 timeout_ms = 500);

    // ---- playback ---------------------------------------------------------------------
    // Returns a null handle when the clip is stale, the FIFO is full, or no handle is free (one
    // warning per frame). A streamed clip plays on one voice at a time.
    AudioHandle play(ClipHandle clip, const PlayParams& params = {});
    // Sample-accurate start (FR-AUD-020, §3.4): the voice's first frame lands exactly on mixer
    // frame `mix_sample` (the clock in stats().mix_sample / mix_clock()). A time already past
    // starts the voice that far into the clip (in-memory clips; streams start at once) — the
    // documented late-request degradation.
    AudioHandle play_at(ClipHandle clip, u64 mix_sample, const PlayParams& params = {});
    void stop(AudioHandle voice, f32 fade_s = 0.f);  // always ramps (≥ 2 ms): no click
    void set_handle_gain(AudioHandle voice, f32 gain_db);
    void set_handle_pan(AudioHandle voice, f32 pan);  // 2D voices; a 3D voice's pan is spatial
    // Moves a 3D voice; gain/pan are recomputed and sent only when they change.
    void set_handle_position(AudioHandle voice, math::vec3 position);
    // True until the mixer reports the voice finished (one-shot end, stop, steal); updated by
    // update(). A stolen handle fails cleanly — handles are generational and never alias.
    bool is_playing(AudioHandle voice) const;

    // ---- mixer surface --------------------------------------------------------------------
    void set_bus_gain(BusId bus, f32 gain_db);  // smooth-ramped
    f32 bus_gain_db(BusId bus) const { return bus < kBusCount ? bus_db_[bus] : 0.f; }
    // Bus graph (FR-AUD-030): every bus but Master feeds one parent with a lower id (so the graph
    // is acyclic by construction and mixes in one descending pass). Default parent: Master.
    Result<void> set_bus_parent(BusId bus, BusId parent);
    BusId bus_parent(BusId bus) const { return bus < kBusCount ? bus_parent_[bus] : BusId{Bus_Master}; }
    // Per-bus EQ band (Off bypasses it). Applied after the bus's children are summed, before its gain.
    Result<void> set_bus_eq(BusId bus, u32 band, const BiquadDesc& desc);
    void set_limiter(const LimiterDesc& desc);

    // ---- 3D -------------------------------------------------------------------------------------
    // Curves 1..kMaxAttenuationCurves-1 (cooked ids); every id starts as the default curve.
    Result<void> set_attenuation(u16 id, const AttenuationCurve& curve);
    // Re-spatializes every live 3D voice (sending only what changed).
    void set_listener(const Listener& listener);
    const Listener& listener() const { return listener_; }

    // The mixer clock: frames mixed since open. play_at schedules against it.
    u64 mix_clock() const;

    // ---- per frame (main thread) ---------------------------------------------------------------
    // Drains mixer replies (finished voices, released clips), refills stream pages, and — on the
    // Silent backend — mixes one block.
    // Freezes the mix (device stops pulling, Silent stops rendering; voices keep their place)
    // until resumed: the app went to the background on a phone (FR-RUN-100).
    void set_paused(bool paused);
    bool paused() const { return paused_; }
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
    void respatialize(u32 handle_index);

    Backend backend_ = Backend::Silent;
    bool paused_ = false;
    Impl* impl_ = nullptr;
    f32 bus_db_[kBusCount] = {};
    BusId bus_parent_[kBusCount] = {};
    Listener listener_;
    u64 command_hash_ = 0;
    u32 free_handle_count_ = kMaxHandles;
};

// dB <-> linear helpers (gain_db <= -96 is silence).
f32 db_to_linear(f32 db);
f32 linear_to_db(f32 lin);

}  // namespace mce::audio
