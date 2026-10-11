#pragma once
// Audio output device seam (audio-input-ui §4.3). The one SDL audio call site; mce_audio sees only
// this SDL-free surface. The device pulls interleaved float32 frames from a render callback that
// runs on SDL's audio thread.

#include "mce/memory/allocator.h"
#include "mce/result.h"

namespace mce::platform {

struct AudioOutputDesc {
    u32 sample_rate = 48000;
    u32 channels = 2;
    u32 period_frames = 480;       // requested device period (10 ms at 48 kHz); drivers may differ
    const char* driver = nullptr;  // SDL audio driver override ("dummy", "wasapi", ...); null = default
};

// Called on the device's real-time thread: fill `out` with frames * channels interleaved samples.
// Must not allocate, lock, or block (FR-AUD-010).
using AudioRenderFn = void (*)(void* user, f32* out, u32 frames);

class AudioOutput {
public:
    // Opens the default playback device and starts pulling from `fn` immediately. Works without
    // platform::init (initializes SDL's audio subsystem on its own).
    static Result<mem::UniquePtr<AudioOutput>> open(const AudioOutputDesc& desc, AudioRenderFn fn, void* user);
    // Like open(), but opening the device runs on a worker thread and this returns at once: with no
    // usable output (a closed MacBook lid, a missing sound server) the platform can block there for
    // seconds. The audio subsystem is initialized here, on the caller's (main) thread. Poll state();
    // `fn` may be called as soon as the device opens, before state() reports it.
    static Result<mem::UniquePtr<AudioOutput>> open_async(const AudioOutputDesc& desc, AudioRenderFn fn, void* user);
    enum class State : u8 { Opening, Open, Failed };
    State state() const;
    // Why the open failed (state() == Failed), else empty.
    const char* error() const;
    // Closes the device. When the destructor returns, the callback is not running and never will.
    // An open still in flight is waited for first.
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    u32 sample_rate() const { return sample_rate_; }
    u32 channels() const { return channels_; }
    const char* driver() const;
    // Number of render callbacks served so far (diagnostics, tests).
    u64 callbacks() const;

    struct Impl;
    AudioOutput() = default;  // use open()

private:
    // Validation, hints and the audio subsystem (main thread): the part open and open_async share.
    static Result<mem::UniquePtr<AudioOutput>> prepare(const AudioOutputDesc& desc, AudioRenderFn fn, void* user);

    Impl* impl_ = nullptr;
    u32 sample_rate_ = 0;
    u32 channels_ = 0;
};

}  // namespace mce::platform
