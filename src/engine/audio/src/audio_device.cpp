#include <cmath>
#include <cstring>

#include "mce/audio/audio.h"
#include "mce/convar.h"
#include "mce/hash.h"
#include "mce/log.h"
#include "mce/platform/audio_output.h"
#include "mce/platform/platform.h"
#include "mixer.h"

namespace mce::audio {

using namespace detail;

namespace {

AudioDevice* g_convar_device = nullptr;

void on_bus_convar(ConVar& var);

// Saved convars (options screen); same-frame effect through the FIFO (FR-DBG-040).
MCE_CVAR(cv_master_db, "audio.master_db", 0.0f, "Master bus gain (dB)", CVarFlags::Saved);
MCE_CVAR(cv_music_db, "audio.music_db", 0.0f, "Music bus gain (dB)", CVarFlags::Saved);
MCE_CVAR(cv_sfx_db, "audio.sfx_db", 0.0f, "SFX bus gain (dB)", CVarFlags::Saved);
MCE_CVAR(cv_ui_db, "audio.ui_db", 0.0f, "UI bus gain (dB)", CVarFlags::Saved);
MCE_CVAR(cv_enabled, "audio.enabled", true, "Open an audio device (false = run silent)", CVarFlags::RequiresRestart);

ConVar* const kBusConvars[kBusCount] = {&cv_master_db, &cv_music_db, &cv_sfx_db, &cv_ui_db};

void on_bus_convar(ConVar& var) {
    if (!g_convar_device) return;
    for (u32 b = 0; b < kBusCount; ++b)
        if (kBusConvars[b] == &var) g_convar_device->set_bus_gain(static_cast<BusId>(b), var.get_float());
}

struct ClipMain {
    enum class State : u8 { Free, Live, Releasing };
    u32 generation = 1;
    State state = State::Free;
    i8 stream = -1;
    bool release_pending = false;  // ReleaseClip not yet accepted by a full FIFO
    ClipPayload payload;
};

struct HandleSlot {
    u32 generation = 1;
    bool live = false;
};

struct StreamMain {
    bool in_use = false;   // owned by a registered stream clip
    AudioHandle playing;   // the one voice reading the ring (null when idle)
};

void hash_command(u64& h, const AudioCommand& c) {
    // Only sim-determined fields: never pointers.
    Hasher64 hs(h);
    hs.u32v(static_cast<u32>(c.type));
    hs.u32v(c.index);
    hs.u32v(c.voice.generation);
    hs.u32v(c.voice.clip);
    hs.f32v(c.voice.gain);
    hs.f32v(c.voice.pan);
    hs.u32v(static_cast<u32>(c.bus) | (static_cast<u32>(c.priority) << 8) | (static_cast<u32>(c.flags) << 16));
    h = hs.value();
}

}  // namespace

struct AudioDevice::Impl {
    explicit Impl(mem::IAllocator& a, usize fifo)
        : alloc(a), commands(a, fifo), replies(a, next_pow2(kMaxHandles + kMaxClips + 64)), clips(a) {}

    mem::IAllocator& alloc;
    RingBuffer<AudioCommand> commands;
    RingBuffer<AudioReply> replies;
    StreamRing* stream_rings[kMaxStreams] = {};
    StreamMain streams[kMaxStreams];
    Mixer* mixer = nullptr;
    mem::UniquePtr<platform::AudioOutput> output;

    Array<ClipMain> clips;
    u32 free_clips[kMaxClips];
    u32 free_clip_count = 0;

    HandleSlot handles[kMaxHandles];
    u32 free_handles[kMaxHandles];

    u64 commands_dropped = 0;
    u32 warned_this_frame = 0;
    f32 scratch[kBlockFrames * kChannels];

    static void render_cb(void* user, f32* out, u32 frames) { static_cast<Impl*>(user)->mixer->render(out, frames); }
};

f32 db_to_linear(f32 db) { return db <= -96.f ? 0.f : std::pow(10.f, db / 20.f); }
f32 linear_to_db(f32 lin) { return lin <= 0.f ? -96.f : 20.f * std::log10(lin); }

Result<mem::UniquePtr<AudioDevice>> AudioDevice::create(const AudioDesc& desc) {
    mem::IAllocator& a = desc.allocator ? *desc.allocator : mem::heap();
    if (!is_pow2(desc.command_capacity))
        return Error(Errc::InvalidArgument, "audio.create", "command_capacity must be a power of two");

    mem::UniquePtr<AudioDevice> dev(a, mem::make<AudioDevice>(a));
    Impl* impl = mem::make<Impl>(a, a, desc.command_capacity);
    dev->impl_ = impl;
    for (u32 s = 0; s < kMaxStreams; ++s) impl->stream_rings[s] = mem::make<StreamRing>(a);
    impl->mixer = mem::make<Mixer>(a, impl->commands, impl->replies, impl->stream_rings);
    impl->clips.resize(kMaxClips);
    for (u32 i = 0; i < kMaxClips; ++i) impl->free_clips[i] = kMaxClips - 1 - i;
    impl->free_clip_count = kMaxClips;
    for (u32 i = 0; i < kMaxHandles; ++i) impl->free_handles[i] = kMaxHandles - 1 - i;
    dev->free_handle_count_ = kMaxHandles;
    dev->command_hash_ = kFnv64Offset;

    dev->backend_ = desc.backend;
    if (desc.backend == Backend::Device && !cv_enabled.get_bool()) dev->backend_ = Backend::Silent;
    if (dev->backend_ == Backend::Device) {
        platform::AudioOutputDesc od;
        od.sample_rate = kSampleRate;
        od.channels = kChannels;
        od.period_frames = desc.period_frames;
        od.driver = desc.driver;
        auto out = platform::AudioOutput::open(od, &Impl::render_cb, impl);
        if (out) {
            impl->output = std::move(out).value();
        } else if (desc.silent_on_failure) {
            FixedString<512> msg;
            format_error(out.error(), msg);
            MCE_LOG_ERROR("audio: no output device ({}); running silent", msg.c_str());
            dev->backend_ = Backend::Silent;
        } else {
            return out.error();
        }
    }

    if (desc.bind_convars) {
        g_convar_device = dev.get();
        for (u32 b = 0; b < kBusCount; ++b) {
            kBusConvars[b]->set_on_change(&on_bus_convar);
            if (kBusConvars[b]->get_float() != 0.f) dev->set_bus_gain(static_cast<BusId>(b), kBusConvars[b]->get_float());
        }
    }
    return dev;
}

AudioDevice::~AudioDevice() {
    if (!impl_) return;
    if (g_convar_device == this) g_convar_device = nullptr;
    Impl* impl = impl_;
    if (impl->output) {
        // Graceful: fade everything, give the callback a few blocks, then close. Closing
        // guarantees the callback is no longer running before we free the mixer.
        AudioCommand c;
        c.type = CmdType::StopAll;
        if (impl->commands.push(c)) platform::sleep_ms(30);
        impl->output.reset();
    }
    mem::IAllocator& a = impl->alloc;
    mem::destroy(a, impl->mixer);
    for (StreamRing* r : impl->stream_rings) mem::destroy(a, r);
    mem::destroy(a, impl);
    impl_ = nullptr;
}

// ---------------------------------------------------------------------------- clips

namespace {
Result<void> validate_clip(const ClipPayload& c) {
    constexpr const char* kStage = "audio.clip.register";
    if (!c.frames || c.frame_count == 0) return Error(Errc::InvalidArgument, kStage, "empty clip");
    if (c.channels != 1 && c.channels != 2) return Error(Errc::Unsupported, kStage, "channels", c.channels);
    if (c.sample_rate != kSampleRate)
        return Error(Errc::Unsupported, kStage, "clips are cooked to 48 kHz", c.sample_rate);
    if (c.loop_start >= c.frame_count) return Error(Errc::InvalidArgument, kStage, "loop_start", c.loop_start);
    return ok();
}
}  // namespace

static Result<ClipHandle> register_any(AudioDevice::Impl& impl, const ClipPayload& clip, i8 stream) {
    MCE_TRY(validate_clip(clip));
    if (impl.free_clip_count == 0) return Error(Errc::OutOfMemory, "audio.clip.register", "clip table full");
    const u32 slot = impl.free_clips[impl.free_clip_count - 1];
    AudioCommand c;
    c.type = CmdType::RegisterClip;
    c.index = slot;
    c.bus = static_cast<u8>(clip.channels);
    c.priority = static_cast<u8>(stream + 1);
    c.clip.frames = clip.frames;
    c.clip.frame_count = clip.frame_count;
    c.clip.loop_start = clip.loop_start;
    if (!impl.commands.push(c)) return Error(Errc::AudioQueueFull, "audio.clip.register");
    --impl.free_clip_count;
    ClipMain& m = impl.clips[slot];
    m.state = ClipMain::State::Live;
    m.stream = stream;
    m.payload = clip;
    if (stream >= 0) impl.streams[stream].in_use = true;
    return ClipHandle{slot, m.generation};
}

Result<ClipHandle> AudioDevice::register_clip(const ClipPayload& clip) { return register_any(*impl_, clip, -1); }

Result<ClipHandle> AudioDevice::register_stream(const ClipPayload& clip) {
    for (u32 s = 0; s < kMaxStreams; ++s)
        if (!impl_->streams[s].in_use) return register_any(*impl_, clip, static_cast<i8>(s));
    return Error(Errc::OutOfMemory, "audio.stream.register", "all stream rings in use", kMaxStreams);
}

bool AudioDevice::is_clip_registered(ClipHandle h) const {
    return h.index < kMaxClips && !h.is_null() && impl_->clips[h.index].generation == h.generation &&
           impl_->clips[h.index].state != ClipMain::State::Free;
}

void AudioDevice::release_clip(ClipHandle h) {
    if (!is_clip_registered(h)) return;
    ClipMain& m = impl_->clips[h.index];
    if (m.state == ClipMain::State::Releasing) return;
    m.state = ClipMain::State::Releasing;
    AudioCommand c;
    c.type = CmdType::ReleaseClip;
    c.index = h.index;
    m.release_pending = !impl_->commands.push(c);  // retried by update()
}

// ---------------------------------------------------------------------------- playback

AudioHandle AudioDevice::play(ClipHandle clip, const PlayParams& p) {
    Impl& impl = *impl_;
    if (!is_clip_registered(clip) || impl.clips[clip.index].state != ClipMain::State::Live) return {};
    ClipMain& m = impl.clips[clip.index];
    if (m.stream >= 0 && !impl.streams[m.stream].playing.is_null()) {
        MCE_LOG_WARN("audio: streamed clip is already playing; stop it first");
        return {};
    }
    if (free_handle_count_ == 0) {
        if (impl.warned_this_frame++ == 0) MCE_LOG_WARN("audio: all {} voice handles in use; play dropped", kMaxHandles);
        return {};
    }
    const u32 idx = impl.free_handles[free_handle_count_ - 1];
    HandleSlot& slot = impl.handles[idx];

    AudioCommand c;
    c.type = CmdType::Play;
    c.index = idx;
    c.bus = p.bus;
    c.priority = p.priority;
    c.flags = p.loop ? CmdFlag::Loop : 0;
    c.voice.generation = slot.generation;
    c.voice.clip = clip.index;
    c.voice.gain = db_to_linear(p.gain_db);
    c.voice.pan = p.pan;
    // A stream's ring is rewound and primed while no voice reads it, before the Play is visible.
    if (m.stream >= 0) impl.stream_rings[m.stream]->reset(m.payload, p.loop);
    if (!impl.commands.push(c)) {
        ++impl.commands_dropped;
        if (impl.warned_this_frame++ == 0) MCE_LOG_WARN("audio: command FIFO full; play dropped");
        return {};
    }
    hash_command(command_hash_, c);
    --free_handle_count_;
    slot.live = true;
    const AudioHandle h{idx, slot.generation};
    if (m.stream >= 0) impl.streams[m.stream].playing = h;
    return h;
}

bool AudioDevice::is_playing(AudioHandle h) const {
    return !h.is_null() && h.index < kMaxHandles && impl_->handles[h.index].live &&
           impl_->handles[h.index].generation == h.generation;
}

void AudioDevice::stop(AudioHandle h, f32 fade_s) {
    if (!is_playing(h)) return;
    AudioCommand c;
    c.type = CmdType::Stop;
    c.index = h.index;
    c.voice.generation = h.generation;
    c.voice.pan = std::floor(fade_s * static_cast<f32>(kSampleRate));
    if (impl_->commands.push(c)) hash_command(command_hash_, c);
    else ++impl_->commands_dropped;
}

void AudioDevice::set_handle_gain(AudioHandle h, f32 gain_db) {
    if (!is_playing(h)) return;
    AudioCommand c;
    c.type = CmdType::VoiceGain;
    c.index = h.index;
    c.voice.generation = h.generation;
    c.voice.gain = db_to_linear(gain_db);
    if (impl_->commands.push(c)) hash_command(command_hash_, c);
    else ++impl_->commands_dropped;
}

void AudioDevice::set_handle_pan(AudioHandle h, f32 pan) {
    if (!is_playing(h)) return;
    AudioCommand c;
    c.type = CmdType::VoicePan;
    c.index = h.index;
    c.voice.generation = h.generation;
    c.voice.pan = pan;
    if (impl_->commands.push(c)) hash_command(command_hash_, c);
    else ++impl_->commands_dropped;
}

void AudioDevice::set_bus_gain(BusId bus, f32 gain_db) {
    if (bus >= kBusCount) return;
    bus_db_[bus] = gain_db;
    AudioCommand c;
    c.type = CmdType::BusGain;
    c.index = bus;
    c.voice.gain = db_to_linear(gain_db);
    if (impl_->commands.push(c)) hash_command(command_hash_, c);
    else ++impl_->commands_dropped;
}

// ---------------------------------------------------------------------------- per frame

void AudioDevice::update() {
    Impl& impl = *impl_;
    impl.warned_this_frame = 0;
    if (backend_ == Backend::Silent) impl.mixer->render(impl.scratch, kBlockFrames);

    AudioReply r;
    while (impl.replies.pop(r)) {
        if (r.type == ReplyType::VoiceEnded) {
            HandleSlot& s = impl.handles[r.index];
            MCE_ASSERT_MSG(s.live && s.generation == r.generation, "audio: VoiceEnded for a dead handle");
            s.live = false;
            if (++s.generation == 0) s.generation = 1;
            impl.free_handles[free_handle_count_++] = r.index;
            for (StreamMain& st : impl.streams)
                if (st.playing == AudioHandle{r.index, r.generation}) st.playing = {};
        } else {
            ClipMain& m = impl.clips[r.index];
            if (m.stream >= 0) impl.streams[m.stream] = StreamMain{};
            m.state = ClipMain::State::Free;
            m.stream = -1;
            m.payload = {};
            if (++m.generation == 0) m.generation = 1;
            impl.free_clips[impl.free_clip_count++] = r.index;
        }
    }

    for (u32 i = 0; i < kMaxClips; ++i) {
        ClipMain& m = impl.clips[i];
        if (!m.release_pending) continue;
        AudioCommand c;
        c.type = CmdType::ReleaseClip;
        c.index = i;
        m.release_pending = !impl.commands.push(c);
    }

    for (u32 s = 0; s < kMaxStreams; ++s)
        if (!impl.streams[s].playing.is_null()) impl.stream_rings[s]->pump();
}

void AudioDevice::render_offline(f32* out, u32 frames) {
    MCE_ASSERT_MSG(backend_ == Backend::Offline, "render_offline needs the Offline backend");
    if (backend_ != Backend::Offline) return;
    impl_->mixer->render(out, frames);
}

AudioStats AudioDevice::stats() const {
    const Mixer::Atomics& a = impl_->mixer->stats();
    AudioStats s;
    s.voices_real = a.voices_real.load(std::memory_order_relaxed);
    s.voices_virtual = a.voices_virtual.load(std::memory_order_relaxed);
    s.steals = a.steals.load(std::memory_order_relaxed);
    s.dropped = a.dropped.load(std::memory_order_relaxed);
    s.underruns = a.underruns.load(std::memory_order_relaxed);
    s.master_peak = a.master_peak.load(std::memory_order_relaxed);
    s.mix_sample = a.mix_sample.load(std::memory_order_acquire);
    s.commands_dropped = impl_->commands_dropped;
    return s;
}

}  // namespace mce::audio
