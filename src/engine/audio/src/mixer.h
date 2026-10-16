#pragma once
// The real-time mixer (audio-input-ui §3.3–3.5). Everything in here runs on the audio thread
// (or the caller's thread for the Offline/Silent backends): no heap, no locks, no ECS. State
// changes arrive only through the command FIFO; results leave only through the reply FIFO and
// the stats atomics.

#include <atomic>

#include "commands.h"
#include "mce/containers/ring_buffer.h"
#include "dsp.h"
#include "stream.h"

namespace mce::audio::detail {

inline constexpr u32 kBlockFrames = 512;        // internal sub-block; callbacks of any size loop
inline constexpr u32 kRampFrames = 480;         // parameter ramp (10 ms): click-free gain/pan
// A voice's step at pitch 1 (32.32 fixed point), and the step for a command's 4.12 rate.
inline constexpr u64 kUnitStep = 1ull << 32;
inline constexpr u64 rate_step(u16 rate) { return static_cast<u64>(rate) << 20; }
inline constexpr u32 kStealFadeFrames = 240;    // 5 ms fade on a stolen voice
inline constexpr u32 kMinStopFadeFrames = 96;   // 2 ms: stop() never cuts a waveform
inline constexpr u32 kMaxCommandsPerCallback = 256;

struct ClipRT {
    const f32* frames = nullptr;
    u32 frame_count = 0;
    u32 loop_start = 0;
    u8 channels = 0;
    i8 stream = -1;  // index into the stream rings, or -1 for in-memory
};

struct VoiceRT {
    u32 handle_index = 0;
    u32 handle_gen = 0;
    u32 clip = 0;
    u64 pos = 0;         // frame cursor in the clip / stream timeline
    u32 frac = 0;        // fraction of a frame past `pos` (pitched voices, 0.32 fixed point)
    u64 step = 1ull << 32;  // source frames per output frame, 32.32 fixed point (M-AUDIO2 2)
    u64 order = 0;       // start order (age) for stealing
    u64 start = 0;       // play_at: mixer frame of the first output frame (0 = already started)
    f32 gain = 1.f;      // logical linear gain
    f32 pan = 0.f;
    f32 cur_l = 0.f, cur_r = 0.f;  // channel gains being applied
    f32 inc_l = 0.f, inc_r = 0.f;
    u32 ramp_left = 0;
    u8 priority = 128;
    u8 bus = Bus_SFX;
    bool loop = false;
    bool active = false;
    bool ending = false;  // fading out; ends when the ramp completes
};

// A post-fader send level, ramped like a bus gain (M-AUDIO2 3).
struct SendRamp {
    f32 cur = 0.f, target = 0.f, inc = 0.f;
    u32 left = 0;
    void set(f32 t) {
        target = t;
        left = kRampFrames;
        inc = (target - cur) / static_cast<f32>(kRampFrames);
    }
    f32 next() {
        const f32 v = cur;
        if (left) {
            cur += inc;
            if (--left == 0) cur = target;
        }
        return v;
    }
    bool silent() const { return cur == 0.f && left == 0; }
};

// A bus low-pass or high-pass that glides to its target cutoff (M-AUDIO2 3).
struct BusFilterRT {
    SvfRT q;
    f32 cur_hz = 0.f, target_hz = 0.f;  // 0 = off (gliding to the band's open end)
    u32 fade_in = 0;  // frames into the dry-to-filtered crossfade of a band just switched on
};

class Mixer {
public:
    Mixer(RingBuffer<AudioCommand>& commands, RingBuffer<AudioReply>& replies, StreamRing* const* streams);

    // The device callback body: drain commands, mix `frames` stereo frames into `out`.
    void render(f32* out, u32 frames);

    struct Atomics {
        std::atomic<u32> voices_real{0};
        std::atomic<u32> voices_virtual{0};
        std::atomic<u64> steals{0};
        std::atomic<u64> dropped{0};
        std::atomic<u64> underruns{0};
        std::atomic<u64> mix_sample{0};
        std::atomic<f32> master_peak{0.f};
        std::atomic<f32> limiter_gain{1.f};
    };
    const Atomics& stats() const { return stats_; }

private:
    void process_commands();
    void cmd_play(const AudioCommand& c);
    void cmd_stop(const AudioCommand& c);
    VoiceRT* find_voice(u32 index, u32 gen);
    void end_voice(VoiceRT& v);
    void set_targets(VoiceRT& v, u32 ramp_frames);
    void advance_virtual(VoiceRT& v, u32 frames);
    // Mixes `frames` of `v` into `dst` (stereo interleaved); ends the voice when it finishes.
    void mix_voice(VoiceRT& v, f32* dst, u32 frames);
    void promote_virtual();
    // Sums bus `b` (filtered, EQ'd) into its parent with b's ramped gain, and into the reverb
    // inputs it sends to.
    void fold_bus(u32 b, u32 frames);
    void publish_stats(f32 peak, f32 limiter_gain);

    RingBuffer<AudioCommand>& commands_;
    RingBuffer<AudioReply>& replies_;
    StreamRing* const* streams_;

    ClipRT clips_[kMaxClips];
    VoiceRT real_[kMaxRealVoices];
    VoiceRT tails_[kMaxRealVoices];  // stolen voices fading out under their replacement
    VoiceRT virtual_[kMaxVirtualVoices];
    u64 next_order_ = 1;
    u64 mix_sample_ = 0;
    u64 steals_ = 0, dropped_ = 0, underruns_ = 0;

    struct BusRT {
        f32 cur = 1.f, target = 1.f, inc = 0.f;
        u32 ramp_left = 0;
        u8 parent = Bus_Master;
        BusFilterRT highpass, lowpass;
        BiquadRT eq[kEqBands];
        SendRamp send[kReverbUnits];
    } bus_[kBusCount];
    LimiterRT limiter_;
    ReverbRT reverb_[kReverbUnits];

    f32 bus_buf_[kBusCount][kBlockFrames * kChannels];
    f32 reverb_buf_[kReverbUnits][kBlockFrames * kChannels];
    Atomics stats_;
};

}  // namespace mce::audio::detail
