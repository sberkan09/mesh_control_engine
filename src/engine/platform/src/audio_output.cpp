#include "mce/platform/audio_output.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <thread>

#include "internal.h"
#include "mce/log.h"
#include "mce/fixed_string.h"

namespace mce::platform {

namespace {
std::atomic<int> g_abandoned_openers{0};
// Largest chunk rendered per user-callback call; bigger device requests loop.
constexpr u32 kMaxChunkFrames = 1024;
constexpr u32 kMaxChannels = 8;
}  // namespace

struct AudioOutput::Impl {
    SDL_AudioStream* stream = nullptr;
    AudioRenderFn fn = nullptr;
    void* user = nullptr;
    u32 channels = 2;
    std::atomic<u64> callbacks{0};
    std::atomic<u8> state{static_cast<u8>(AudioOutput::State::Opening)};
    std::atomic<bool> paused{false};
    FixedString<256> error;  // written before state turns Failed
    u32 sample_rate = 48000;
    std::thread opener;
    // An owner that cannot wait for a stuck opener (below) abandons the Impl to it: whichever of
    // the two finishes last frees it. `abandoned` stops the callback from touching fn/user.
    std::atomic<bool> abandoned{false};
    std::atomic<bool> other_done{false};
    std::atomic<u32> in_callback{0};
    f32 scratch[kMaxChunkFrames * kMaxChannels];  // preallocated: the callback never allocates
};

namespace {

void SDLCALL stream_callback(void* userdata, SDL_AudioStream* stream, int additional_amount, int /*total*/) {
    auto* impl = static_cast<AudioOutput::Impl*>(userdata);
    impl->in_callback.fetch_add(1);
    if (!impl->abandoned.load()) {
        const u32 frame_bytes = static_cast<u32>(sizeof(f32)) * impl->channels;
        u32 frames = additional_amount > 0 ? static_cast<u32>(additional_amount) / frame_bytes : 0;
        while (frames > 0) {
            const u32 n = frames < kMaxChunkFrames ? frames : kMaxChunkFrames;
            impl->fn(impl->user, impl->scratch, n);
            SDL_PutAudioStreamData(stream, impl->scratch, static_cast<int>(n * frame_bytes));
            frames -= n;
        }
        impl->callbacks.fetch_add(1, std::memory_order_relaxed);
    }
    impl->in_callback.fetch_sub(1);
}

// The slow part (any thread: SDL's device calls are thread-safe). Publishes the result in state.
void open_stream(AudioOutput::Impl& impl) {
    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_F32;
    spec.channels = static_cast<int>(impl.channels);
    spec.freq = static_cast<int>(impl.sample_rate);
    impl.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &stream_callback, &impl);
    const bool opened = impl.stream != nullptr;
    if (!opened || !SDL_ResumeAudioStreamDevice(impl.stream)) {
        impl.error = FixedString<256>(StringView(opened ? "resume: " : "open: "));
        impl.error.append(StringView(SDL_GetError()));
        impl.state.store(static_cast<u8>(AudioOutput::State::Failed), std::memory_order_release);
        return;
    }
    if (impl.paused.load(std::memory_order_acquire)) SDL_PauseAudioStreamDevice(impl.stream);
    const char* d = SDL_GetCurrentAudioDriver();
    MCE_LOG_INFO("audio: device open ({} Hz, {} ch, driver '{}')", impl.sample_rate, impl.channels, d ? d : "");
    impl.state.store(static_cast<u8>(AudioOutput::State::Open), std::memory_order_release);
}

}  // namespace

// Main thread: SDL_InitSubSystem is not thread-safe.
Result<mem::UniquePtr<AudioOutput>> AudioOutput::prepare(const AudioOutputDesc& desc, AudioRenderFn fn, void* user) {
    MCE_ASSERT_MSG(fn != nullptr, "AudioOutput::open needs a render callback");
    if (desc.channels == 0 || desc.channels > kMaxChannels)
        return Error(Errc::InvalidArgument, "platform.audio.open", "", desc.channels);
    if (desc.driver) SDL_SetHint(SDL_HINT_AUDIO_DRIVER, desc.driver);
    if (desc.period_frames) {
        u32 period = desc.period_frames;
#if defined(__EMSCRIPTEN__)
        // WebAudio's ScriptProcessorNode takes power-of-two buffers of 256..16384 frames only.
        u32 pow2 = 256;
        while (pow2 < period && pow2 < 16384) pow2 <<= 1;
        period = pow2;
#endif
        char frames[16];
        SDL_snprintf(frames, sizeof(frames), "%u", period);
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames);
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        MCE_LOG_ERROR("audio: SDL audio init failed: {}", SDL_GetError());
        return Error(Errc::AudioDeviceOpen, "platform.audio.init", SDL_GetError());
    }
    mem::IAllocator& a = mem::heap();
    mem::UniquePtr<AudioOutput> out(a, mem::make<AudioOutput>(a));
    out->impl_ = mem::make<Impl>(a);  // from here ~AudioOutput quits the subsystem
    out->impl_->fn = fn;
    out->impl_->user = user;
    out->impl_->channels = desc.channels;
    out->impl_->sample_rate = desc.sample_rate;
    out->sample_rate_ = desc.sample_rate;
    out->channels_ = desc.channels;
    return out;
}

Result<mem::UniquePtr<AudioOutput>> AudioOutput::open(const AudioOutputDesc& desc, AudioRenderFn fn, void* user) {
    MCE_TRY_ASSIGN(mem::UniquePtr<AudioOutput> out, prepare(desc, fn, user));
    open_stream(*out->impl_);
    if (out->state() == State::Failed) {
        MCE_LOG_ERROR("audio: opening the default playback device failed: {}", out->error());
        return Error(Errc::AudioDeviceOpen, "platform.audio.open", out->error());  // ~AudioOutput quits the subsystem
    }
    return out;
}

Result<mem::UniquePtr<AudioOutput>> AudioOutput::open_async(const AudioOutputDesc& desc, AudioRenderFn fn, void* user) {
    MCE_TRY_ASSIGN(mem::UniquePtr<AudioOutput> out, prepare(desc, fn, user));
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    // No threads in the browser (ADR-0039); opening a WebAudio context does not block anyway.
    open_stream(*out->impl_);
    return out;
#else
    Impl* impl = out->impl_;
    impl->opener = std::thread([impl] {
        open_stream(*impl);
        if (impl->other_done.exchange(true)) {
            // The owner gave up waiting (~AudioOutput): this thread frees what it left behind.
            if (impl->stream) SDL_DestroyAudioStream(impl->stream);
            mem::destroy(mem::heap(), impl);
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            detail::audio_opener_abandoned(-1);
        }
        SDL_CleanupTLS();  // not an SDL thread: SDL frees its thread-local state only when told
    });
    return out;
#endif
}

AudioOutput::State AudioOutput::state() const {
    return impl_ ? static_cast<State>(impl_->state.load(std::memory_order_acquire)) : State::Failed;
}

const char* AudioOutput::error() const {
    return state() == State::Failed && impl_ ? impl_->error.c_str() : "";
}

AudioOutput::~AudioOutput() {
    if (!impl_) return;
    if (impl_->opener.joinable()) {
        // Opening the device can hang (a stuck coreaudiod times out for minutes, macOS and the Simulator): wait
        // a moment, then leave the opener to clean up, so quitting the game never blocks on audio.
        for (int i = 0; i < 100 && state() == State::Opening; ++i) SDL_Delay(10);
        if (state() == State::Opening) {
            impl_->abandoned.store(true);
            while (impl_->in_callback.load() != 0) SDL_Delay(1);  // a render already running finishes
            if (!impl_->other_done.exchange(true)) {
                MCE_LOG_WARN("audio: the device is still opening; leaving it to close on its own");
                detail::audio_opener_abandoned(+1);
                impl_->opener.detach();
                impl_ = nullptr;
                return;
            }
        }
        impl_->opener.join();
    }
    // SDL guarantees the stream callback is not running once the stream is destroyed.
    if (impl_->stream) SDL_DestroyAudioStream(impl_->stream);
    mem::destroy(mem::heap(), impl_);
    impl_ = nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

const char* AudioOutput::driver() const {
    const char* d = SDL_GetCurrentAudioDriver();
    return d ? d : "";
}

void AudioOutput::set_paused(bool paused) {
    if (!impl_ || impl_->paused.exchange(paused, std::memory_order_acq_rel) == paused) return;
    // Before Open the opener applies the flag itself (open_stream reads it after resuming).
    if (state() != State::Open || !impl_->stream) return;
    if (paused) SDL_PauseAudioStreamDevice(impl_->stream);
    else SDL_ResumeAudioStreamDevice(impl_->stream);
}

void detail::audio_opener_abandoned(int delta) { g_abandoned_openers.fetch_add(delta); }
bool detail::audio_openers_pending() { return g_abandoned_openers.load() > 0; }

bool AudioOutput::paused() const { return impl_ && impl_->paused.load(std::memory_order_acquire); }

u64 AudioOutput::callbacks() const { return impl_ ? impl_->callbacks.load(std::memory_order_relaxed) : 0; }

}  // namespace mce::platform
