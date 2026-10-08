#include "mce/platform/audio_output.h"

#include <SDL3/SDL.h>

#include <atomic>

#include "mce/log.h"

namespace mce::platform {

namespace {
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
    f32 scratch[kMaxChunkFrames * kMaxChannels];  // preallocated: the callback never allocates
};

namespace {

void SDLCALL stream_callback(void* userdata, SDL_AudioStream* stream, int additional_amount, int /*total*/) {
    auto* impl = static_cast<AudioOutput::Impl*>(userdata);
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

}  // namespace

Result<mem::UniquePtr<AudioOutput>> AudioOutput::open(const AudioOutputDesc& desc, AudioRenderFn fn, void* user) {
    MCE_ASSERT_MSG(fn != nullptr, "AudioOutput::open needs a render callback");
    if (desc.channels == 0 || desc.channels > kMaxChannels)
        return Error(Errc::InvalidArgument, "platform.audio.open", "", desc.channels);
    if (desc.driver) SDL_SetHint(SDL_HINT_AUDIO_DRIVER, desc.driver);
    if (desc.period_frames) {
        char frames[16];
        SDL_snprintf(frames, sizeof(frames), "%u", desc.period_frames);
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames);
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        MCE_LOG_ERROR("audio: SDL audio init failed: {}", SDL_GetError());
        return Error(Errc::AudioDeviceOpen, "platform.audio.init", SDL_GetError());
    }

    mem::IAllocator& a = mem::heap();
    mem::UniquePtr<AudioOutput> out(a, mem::make<AudioOutput>(a));
    out->impl_ = mem::make<Impl>(a);
    Impl& impl = *out->impl_;
    impl.fn = fn;
    impl.user = user;
    impl.channels = desc.channels;
    out->sample_rate_ = desc.sample_rate;
    out->channels_ = desc.channels;

    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_F32;
    spec.channels = static_cast<int>(desc.channels);
    spec.freq = static_cast<int>(desc.sample_rate);
    impl.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &stream_callback, &impl);
    if (!impl.stream) {
        MCE_LOG_ERROR("audio: opening the default playback device failed: {}", SDL_GetError());
        Error e(Errc::AudioDeviceOpen, "platform.audio.open", SDL_GetError());
        return e;  // ~AudioOutput quits the subsystem
    }
    if (!SDL_ResumeAudioStreamDevice(impl.stream)) {
        Error e(Errc::AudioDeviceOpen, "platform.audio.resume", SDL_GetError());
        return e;
    }
    MCE_LOG_INFO("audio: device open ({} Hz, {} ch, driver '{}')", desc.sample_rate, desc.channels, out->driver());
    return out;
}

AudioOutput::~AudioOutput() {
    if (!impl_) return;
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

u64 AudioOutput::callbacks() const { return impl_ ? impl_->callbacks.load(std::memory_order_relaxed) : 0; }

}  // namespace mce::platform
