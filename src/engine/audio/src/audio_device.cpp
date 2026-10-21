#include <atomic>
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
MCE_CVAR(cv_voice_db, "audio.voice_db", 0.0f, "Voice bus gain (dB): dialogue on the first user bus", CVarFlags::Saved);
MCE_CVAR(cv_enabled, "audio.enabled", true, "Open an audio device (false = run silent)", CVarFlags::RequiresRestart);
MCE_CVAR(cv_reverb, "audio.reverb", true, "Reverb on the buses that send to it (false = dry everywhere)", CVarFlags::Saved);

inline constexpr u32 kNamedBuses = Bus_User0 + 1;  // Master/Music/SFX/UI/Voice have options-screen convars
ConVar* const kBusConvars[kNamedBuses] = {&cv_master_db, &cv_music_db, &cv_sfx_db, &cv_ui_db, &cv_voice_db};

void on_reverb_convar(ConVar& var) {
    if (g_convar_device) g_convar_device->set_reverb_allowed(var.get_bool());
}

void on_bus_convar(ConVar& var) {
    if (!g_convar_device) return;
    for (u32 b = 0; b < kNamedBuses; ++b)
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
    // 3D voices (attenuation != 0): the main thread owns the spatial state and re-sends gain/pan
    // only when the result changes.
    u16 attenuation = 0;
    f32 base_gain = 1.f;  // linear, before attenuation
    math::vec3 position;
    f32 sent_gain = 0.f, sent_pan = 0.f;
    f32 sent_lowpass = 0.f;
};

struct StreamMain {
    bool in_use = false;   // owned by a registered stream clip
    AudioHandle playing;   // the one voice reading the ring (null when idle)
};

u16 encode_rate(f32 pitch) {
    const f32 p = std::isnan(pitch) ? 1.f : math::clamp(pitch, kMinPitch, kMaxPitch);  // NaN plays at 1
    return static_cast<u16>(std::lround(p * static_cast<f32>(kRateUnit)));
}

void hash_command(u64& h, const AudioCommand& c) {
    // Only sim-determined fields: never pointers.
    Hasher64 hs(h);
    hs.u32v(static_cast<u32>(c.type));
    hs.u32v(c.index);
    hs.u32v(c.voice.generation);
    hs.u32v(c.voice.clip);
    // Only pitched voices carry a rate: plays at the original pitch hash as they did before M-AUDIO2.
    if ((c.type == CmdType::Play || c.type == CmdType::VoicePitch) && c.voice.rate != kRateUnit) hs.u32v(c.voice.rate);
    hs.f32v(c.voice.gain);
    hs.f32v(c.voice.pan);
    hs.u32v(static_cast<u32>(c.bus) | (static_cast<u32>(c.priority) << 8) | (static_cast<u32>(c.flags) << 16));
    // Only scheduled plays carry a start: immediate plays hash exactly as they did before play_at.
    if (c.type == CmdType::Play && c.voice.start_sample != 0) hs.u64v(c.voice.start_sample);
    h = hs.value();
}

// Mixer configuration (graph/EQ/limiter): the fields that define it, never the union's other views.
void hash_config(u64& h, const AudioCommand& c, f32 a = 0.f, f32 b = 0.f) {
    Hasher64 hs(h);
    hs.u32v(static_cast<u32>(c.type));
    hs.u32v(c.index);
    hs.u32v(static_cast<u32>(c.bus) | (static_cast<u32>(c.flags) << 16));
    hs.f32v(a);
    hs.f32v(b);
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
    // The device callback mixes only once this is set. An output opened in the background
    // (silent_on_failure) starts while update() still mixes on the main thread (Silent): the main
    // thread hands the mixer over by setting it after its last block, so one thread mixes at a time.
    std::atomic<bool> device_mixes{false};
    bool device_opening = false;  // open_async in flight: update() polls it

    Array<ClipMain> clips;
    u32 free_clips[kMaxClips];
    u32 free_clip_count = 0;

    HandleSlot handles[kMaxHandles];
    u32 free_handles[kMaxHandles];

    AttenuationCurve curves[kMaxAttenuationCurves];

    u64 commands_dropped = 0;
    u32 warned_this_frame = 0;
    f32 scratch[kBlockFrames * kChannels];

    static void render_cb(void* user, f32* out, u32 frames) {
        Impl* impl = static_cast<Impl*>(user);
        if (impl->device_mixes.load(std::memory_order_acquire)) impl->mixer->render(out, frames);
        else std::memset(out, 0, sizeof(f32) * frames * kChannels);
    }
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
    for (BusId& p : dev->bus_parent_) p = Bus_Master;
    for (auto& sends : dev->send_db_)
        for (f32& db : sends) db = -96.f;

    dev->backend_ = desc.backend;
    if (desc.backend == Backend::Device && !cv_enabled.get_bool()) dev->backend_ = Backend::Silent;
    if (dev->backend_ == Backend::Device) {
        platform::AudioOutputDesc od;
        od.sample_rate = kSampleRate;
        od.channels = kChannels;
        od.period_frames = desc.period_frames;
        od.driver = desc.driver;
        if (desc.silent_on_failure) {
            // A game: never wait on the device (it can block for seconds when there is no output);
            // run Silent until update() sees it open.
            dev->backend_ = Backend::Silent;
            auto out = platform::AudioOutput::open_async(od, &Impl::render_cb, impl);
            if (out) {
                impl->output = std::move(out).value();
                impl->device_opening = true;
            } else {
                FixedString<512> msg;
                format_error(out.error(), msg);
                MCE_LOG_ERROR("audio: no output device ({}); running silent", msg.c_str());
            }
        } else {
            impl->device_mixes.store(true, std::memory_order_release);
            auto out = platform::AudioOutput::open(od, &Impl::render_cb, impl);
            if (!out) return out.error();
            impl->output = std::move(out).value();
        }
    }

    if (desc.bind_convars) {
        g_convar_device = dev.get();
        for (u32 b = 0; b < kNamedBuses; ++b) {
            kBusConvars[b]->set_on_change(&on_bus_convar);
            if (kBusConvars[b]->get_float() != 0.f) dev->set_bus_gain(static_cast<BusId>(b), kBusConvars[b]->get_float());
        }
        cv_reverb.set_on_change(&on_reverb_convar);
        dev->reverb_allowed_ = cv_reverb.get_bool();
    }
    return dev;
}

AudioDevice::~AudioDevice() {
    if (!impl_) return;
    if (g_convar_device == this) g_convar_device = nullptr;
    Impl* impl = impl_;
    if (impl->output) {
        // Waits for an open still in flight (~AudioOutput joins it) only when the game quits first.
        // Graceful: fade everything, give the callback a few blocks, then close. Closing
        // guarantees the callback is no longer running before we free the mixer.
        AudioCommand c;
        c.type = CmdType::StopAll;
        if (backend_ == Backend::Device && impl->commands.push(c)) platform::sleep_ms(30);
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
Result<void> validate_clip(const ClipPayload& c, bool stream) {
    constexpr const char* kStage = "audio.clip.register";
    const bool encoded = c.encoded && c.encoded_bytes;
    if (c.frame_count == 0 || (!c.frames && !encoded)) return Error(Errc::InvalidArgument, kStage, "empty clip");
    if (!c.frames && !stream) return Error(Errc::InvalidArgument, kStage, "an encoded clip streams, or is decoded first (ClipBank)");
    if (c.channels != 1 && c.channels != 2) return Error(Errc::Unsupported, kStage, "channels", c.channels);
    if (c.sample_rate != kSampleRate)
        return Error(Errc::Unsupported, kStage, "clips are cooked to 48 kHz", c.sample_rate);
    if (c.loop_start >= c.frame_count) return Error(Errc::InvalidArgument, kStage, "loop_start", c.loop_start);
    return ok();
}
}  // namespace

static Result<ClipHandle> register_any(AudioDevice::Impl& impl, const ClipPayload& clip, i8 stream) {
    MCE_TRY(validate_clip(clip, stream >= 0));
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

bool AudioDevice::is_stream(ClipHandle h) const { return is_clip_registered(h) && impl_->clips[h.index].stream >= 0; }

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

bool AudioDevice::finish_releases(u32 timeout_ms) {
    Impl& impl = *impl_;
    auto pending = [&impl] {
        for (const ClipMain& m : impl.clips)
            if (m.state == ClipMain::State::Releasing) return true;
        return false;
    };
    for (u32 waited = 0;; ++waited) {
        // Until the device callback owns the mixer, this thread does: run its command step only.
        if (!impl.device_mixes.load(std::memory_order_acquire)) impl.mixer->render(impl.scratch, 0);
        update();
        if (!pending()) return true;
        if (waited >= timeout_ms) {
            MCE_LOG_WARN("audio: clip releases still pending after {} ms", timeout_ms);
            return false;
        }
        platform::sleep_ms(1);
    }
}

// ---------------------------------------------------------------------------- playback

AudioHandle AudioDevice::play(ClipHandle clip, const PlayParams& p) { return play_at(clip, 0, p); }

AudioHandle AudioDevice::play_at(ClipHandle clip, u64 mix_sample, const PlayParams& p) {
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
    c.voice.clip = static_cast<u16>(clip.index);
    c.voice.rate = m.stream >= 0 ? kRateUnit : encode_rate(p.pitch);
    c.voice.gain = db_to_linear(p.gain_db);
    c.voice.pan = p.pan;
    c.voice.start_sample = mix_sample;
    const bool spatial = p.attenuation_id != 0 && p.attenuation_id < kMaxAttenuationCurves;
    const f32 base_gain = c.voice.gain;
    if (spatial) {
        const Spatial sp = spatialize(listeners(), listener_mix_, impl.curves[p.attenuation_id], p.position);
        c.voice.gain = base_gain * sp.gain;
        c.voice.pan = sp.pan;
    }
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
    slot.attenuation = spatial ? p.attenuation_id : u16(0);
    slot.base_gain = base_gain;
    slot.position = p.position;
    slot.sent_gain = c.voice.gain;
    slot.sent_pan = c.voice.pan;
    slot.sent_lowpass = 0.f;
    const AudioHandle h{idx, slot.generation};
    if (m.stream >= 0) impl.streams[m.stream].playing = h;
    return h;
}

bool AudioDevice::stream_busy(ClipHandle clip) const {
    const Impl& impl = *impl_;
    if (!is_clip_registered(clip)) return false;
    const ClipMain& m = impl.clips[clip.index];
    return m.stream >= 0 && !impl.streams[m.stream].playing.is_null();
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
    HandleSlot& slot = impl_->handles[h.index];
    if (slot.attenuation != 0) {
        slot.base_gain = db_to_linear(gain_db);
        respatialize(h.index);
        return;
    }
    AudioCommand c;
    c.type = CmdType::VoiceGain;
    c.index = h.index;
    c.voice.generation = h.generation;
    c.voice.gain = db_to_linear(gain_db);
    if (impl_->commands.push(c)) hash_command(command_hash_, c);
    else ++impl_->commands_dropped;
}

void AudioDevice::set_handle_position(AudioHandle h, math::vec3 position) {
    if (!is_playing(h)) return;
    HandleSlot& slot = impl_->handles[h.index];
    if (slot.attenuation == 0) return;
    slot.position = position;
    respatialize(h.index);
}

void AudioDevice::respatialize(u32 index) {
    HandleSlot& slot = impl_->handles[index];
    const Spatial sp = spatialize(listeners(), listener_mix_, impl_->curves[slot.attenuation], slot.position);
    const f32 gain = slot.base_gain * sp.gain;
    if (gain == slot.sent_gain && sp.pan == slot.sent_pan) return;  // nothing audible changed
    AudioCommand c;
    c.type = CmdType::VoiceGainPan;
    c.index = index;
    c.voice.generation = slot.generation;
    c.voice.gain = gain;
    c.voice.pan = sp.pan;
    if (impl_->commands.push(c)) {
        hash_command(command_hash_, c);
        slot.sent_gain = gain;
        slot.sent_pan = sp.pan;
    } else {
        ++impl_->commands_dropped;  // retried by the next position/listener change
    }
}

void AudioDevice::set_listener(const Listener& l) { set_listeners(Span<const Listener>(&l, 1), ListenerMix::First); }

void AudioDevice::set_listeners(Span<const Listener> ls, ListenerMix mix) {
    if (ls.empty()) return;
    listener_count_ = static_cast<u32>(ls.size() < kMaxListeners ? ls.size() : kMaxListeners);
    for (u32 i = 0; i < listener_count_; ++i) listeners_[i] = ls[i];
    listener_mix_ = mix;
    for (u32 i = 0; i < kMaxHandles; ++i)
        if (impl_->handles[i].live && impl_->handles[i].attenuation != 0) respatialize(i);
}

Result<void> AudioDevice::set_attenuation(u16 id, const AttenuationCurve& curve) {
    if (id == 0 || id >= kMaxAttenuationCurves)
        return Error(Errc::InvalidArgument, "audio.attenuation", "curve id out of range", id);
    if (!(curve.min_distance > 0.f) || curve.max_distance < curve.min_distance || curve.rolloff < 0.f)
        return Error(Errc::InvalidArgument, "audio.attenuation", "needs 0 < min_distance <= max_distance, rolloff >= 0");
    impl_->curves[id] = curve;
    return ok();
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

void AudioDevice::set_handle_pitch(AudioHandle h, f32 pitch) {
    if (!is_playing(h)) return;
    AudioCommand c;
    c.type = CmdType::VoicePitch;
    c.index = h.index;
    c.voice.generation = h.generation;
    c.voice.rate = encode_rate(pitch);
    if (impl_->commands.push(c)) hash_command(command_hash_, c);
    else ++impl_->commands_dropped;
}

void AudioDevice::set_handle_lowpass(AudioHandle h, f32 cutoff_hz) {
    if (!is_playing(h)) return;
    const f32 hz = cutoff_hz > 0.f ? math::clamp(cutoff_hz, 20.f, 20000.f) : 0.f;  // NaN and <= 0: off
    HandleSlot& slot = impl_->handles[h.index];
    if (hz == slot.sent_lowpass) return;
    AudioCommand c;
    c.type = CmdType::VoiceLowpass;
    c.index = h.index;
    c.voice.generation = h.generation;
    c.voice.gain = hz;
    if (!impl_->commands.push(c)) {
        ++impl_->commands_dropped;
        return;
    }
    slot.sent_lowpass = hz;
    hash_command(command_hash_, c);
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

// ---------------------------------------------------------------------------- graph + DSP

Result<void> AudioDevice::set_bus_parent(BusId bus, BusId parent) {
    if (bus == Bus_Master || bus >= kBusCount || parent >= bus)
        return Error(Errc::InvalidArgument, "audio.bus.parent", "a bus feeds a lower-id parent (Master is the root)", bus);
    AudioCommand c;
    c.type = CmdType::BusParent;
    c.index = bus;
    c.bus = parent;
    if (!impl_->commands.push(c)) return Error(Errc::AudioQueueFull, "audio.bus.parent");
    bus_parent_[bus] = parent;
    hash_config(command_hash_, c);
    return ok();
}

Result<void> AudioDevice::set_bus_eq(BusId bus, u32 band, const BiquadDesc& d) {
    if (bus >= kBusCount || band >= kEqBands) return Error(Errc::InvalidArgument, "audio.bus.eq", "bus/band", band);
    AudioCommand c;
    c.type = CmdType::BusEq;
    c.index = bus;
    c.flags = static_cast<u8>(band | (d.type == BiquadDesc::Type::Off ? 0x80u : 0u));
    c.eq.coeffs = biquad_coeffs(d);
    if (!impl_->commands.push(c)) return Error(Errc::AudioQueueFull, "audio.bus.eq");
    hash_config(command_hash_, c, d.freq_hz, d.gain_db);
    return ok();
}

void AudioDevice::set_limiter(const LimiterDesc& d) {
    if (d == limiter_) return;
    limiter_ = d;
    AudioCommand c;
    c.type = CmdType::Limiter;
    c.flags = d.enabled ? 1 : 0;
    c.voice.gain = db_to_linear(d.ceiling_db);
    const f32 frames = d.release_ms > 0.f ? d.release_ms * 0.001f * static_cast<f32>(kSampleRate) : 1.f;
    c.voice.pan = 1.f - std::exp(-1.f / frames);
    if (impl_->commands.push(c)) hash_config(command_hash_, c, c.voice.gain, c.voice.pan);
    else ++impl_->commands_dropped;
}

void AudioDevice::set_bus_filter(BusId bus, const BusFilterDesc& d) {
    if (bus >= kBusCount) return;
    const auto band = [](f32 hz) { return hz > 0.f ? math::clamp(hz, 20.f, 20000.f) : 0.f; };  // NaN and <= 0: off
    BusFilterDesc f;
    f.lowpass_hz = band(d.lowpass_hz);
    f.highpass_hz = band(d.highpass_hz);
    if (f == filter_[bus]) return;
    AudioCommand c;
    c.type = CmdType::BusFilter;
    c.index = bus;
    c.voice.gain = f.lowpass_hz;
    c.voice.pan = f.highpass_hz;
    if (!impl_->commands.push(c)) {
        ++impl_->commands_dropped;
        return;
    }
    filter_[bus] = f;
    hash_config(command_hash_, c, f.lowpass_hz, f.highpass_hz);
}

Result<void> AudioDevice::set_bus_send(BusId bus, u32 unit, f32 send_db) {
    if (bus == Bus_Master || bus >= kBusCount || unit >= kReverbUnits)
        return Error(Errc::InvalidArgument, "audio.bus.send", "a bus other than Master sends to reverb unit 0 or 1", bus);
    const f32 db = std::isnan(send_db) || send_db <= -96.f ? -96.f : math::clamp(send_db, -96.f, 12.f);
    if (db == send_db_[bus][unit]) return ok();
    AudioCommand c;
    c.type = CmdType::BusSend;
    c.index = bus;
    c.flags = static_cast<u8>(unit);
    c.voice.gain = db_to_linear(db);
    if (!impl_->commands.push(c)) return Error(Errc::AudioQueueFull, "audio.bus.send");
    send_db_[bus][unit] = db;
    hash_config(command_hash_, c, db);
    return ok();
}

Result<void> AudioDevice::set_reverb(u32 unit, const ReverbDesc& d) {
    if (unit >= kReverbUnits) return Error(Errc::InvalidArgument, "audio.reverb", "unit", unit);
    const auto unit_range = [](f32 v) { return std::isnan(v) ? 0.5f : math::clamp(v, 0.f, 1.f); };
    ReverbDesc r = d;
    r.room_size = unit_range(d.room_size);
    r.damping = unit_range(d.damping);
    r.width = std::isnan(d.width) ? 1.f : math::clamp(d.width, 0.f, 1.f);
    r.wet_db = std::isnan(d.wet_db) ? 0.f : math::clamp(d.wet_db, -96.f, 12.f);
    if (r == reverb_[unit]) return ok();
    reverb_[unit] = r;
    send_reverb(unit);
    return ok();
}

void AudioDevice::set_bus_trim(BusId bus, f32 trim_db) {
    if (bus >= kBusCount) return;
    const f32 db = std::isnan(trim_db) || trim_db <= -96.f ? -96.f : math::clamp(trim_db, -96.f, 12.f);
    if (db == trim_db_[bus]) return;
    AudioCommand c;
    c.type = CmdType::BusTrim;
    c.index = bus;
    c.voice.gain = db_to_linear(db);
    if (!impl_->commands.push(c)) {
        ++impl_->commands_dropped;
        return;
    }
    trim_db_[bus] = db;
    hash_config(command_hash_, c, db);
}

Result<void> AudioDevice::set_bus_duck(BusId bus, const DuckDesc& d) {
    if (bus == Bus_Master || bus >= Bus_User0 || d.key == Bus_Master || d.key >= kBusCount || d.key == bus)
        return Error(Errc::InvalidArgument, "audio.bus.duck", "Music, SFX or UI ducks under another bus (not Master)", bus);
    const auto finite = [](f32 v, f32 lo, f32 hi, f32 fallback) { return std::isnan(v) ? fallback : math::clamp(v, lo, hi); };
    DuckDesc k = d;
    k.depth_db = finite(d.depth_db, -96.f, 0.f, -10.f);
    k.threshold_db = finite(d.threshold_db, -96.f, 0.f, -40.f);
    k.attack_ms = finite(d.attack_ms, 0.f, 5000.f, 20.f);
    k.release_ms = finite(d.release_ms, 0.f, 10000.f, 400.f);
    if (k == duck_[bus]) return ok();
    // One-pole approach factors: ~63% of the way in the given time (0 ms = at once).
    const auto coef = [](f32 ms) { return ms <= 0.f ? 1.f : 1.f - std::exp(-1.f / (ms * 0.001f * static_cast<f32>(kSampleRate))); };
    AudioCommand c;
    c.type = CmdType::BusDuck;
    c.index = bus;
    c.bus = k.key;
    c.flags = k.enabled ? 1 : 0;
    c.duck.threshold = db_to_linear(k.threshold_db);
    c.duck.depth = db_to_linear(k.depth_db);
    c.duck.attack = coef(k.attack_ms);
    c.duck.release = coef(k.release_ms);
    if (!impl_->commands.push(c)) return Error(Errc::AudioQueueFull, "audio.bus.duck");
    duck_[bus] = k;
    hash_config(command_hash_, c, c.duck.threshold, c.duck.depth);
    hash_config(command_hash_, c, c.duck.attack, c.duck.release);
    return ok();
}

void AudioDevice::set_reverb_allowed(bool allowed) {
    if (allowed == reverb_allowed_) return;
    reverb_allowed_ = allowed;
    for (u32 u = 0; u < kReverbUnits; ++u)
        if (reverb_[u].enabled) send_reverb(u);
}

void AudioDevice::send_reverb(u32 unit) {
    const ReverbDesc& r = reverb_[unit];
    AudioCommand c;
    c.type = CmdType::Reverb;
    c.index = unit;
    c.flags = r.enabled && reverb_allowed_ ? 1 : 0;
    // Freeverb's mapping: room size to comb feedback, damping to the combs' low-pass, and wet
    // level with width to the same-side and cross-side return gains.
    const f32 wet = db_to_linear(r.wet_db);
    c.reverb.feedback = r.room_size * detail::freeverb::kScaleRoom + detail::freeverb::kOffsetRoom;
    c.reverb.damp = r.damping * detail::freeverb::kScaleDamp;
    c.reverb.wet1 = wet * (r.width * 0.5f + 0.5f);
    c.reverb.wet2 = wet * ((1.f - r.width) * 0.5f);
    if (!impl_->commands.push(c)) {
        ++impl_->commands_dropped;
        return;
    }
    hash_config(command_hash_, c, c.reverb.feedback, c.reverb.damp);
    hash_config(command_hash_, c, c.reverb.wet1, c.reverb.wet2);
}

void AudioDevice::set_paused(bool paused) {
    paused_ = paused;
    if (impl_->output) impl_->output->set_paused(paused);
}

u64 AudioDevice::mix_clock() const { return impl_->mixer->stats().mix_sample.load(std::memory_order_acquire); }

// ---------------------------------------------------------------------------- per frame

void AudioDevice::update() {
    Impl& impl = *impl_;
    impl.warned_this_frame = 0;
    if (impl.device_opening) {
        const platform::AudioOutput::State st = impl.output->state();
        if (st == platform::AudioOutput::State::Open) {
            impl.device_opening = false;
            impl.device_mixes.store(true, std::memory_order_release);  // after our last Silent block
            backend_ = Backend::Device;
        } else if (st == platform::AudioOutput::State::Failed) {
            impl.device_opening = false;
            MCE_LOG_ERROR("audio: no output device ({}); running silent", impl.output->error());
            impl.output.reset();
        }
    }
    if (backend_ == Backend::Silent && !paused_) impl.mixer->render(impl.scratch, kBlockFrames);

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
    s.limiter_gain = a.limiter_gain.load(std::memory_order_relaxed);
    s.mix_sample = a.mix_sample.load(std::memory_order_acquire);
    s.commands_dropped = impl_->commands_dropped;
    return s;
}

}  // namespace mce::audio
