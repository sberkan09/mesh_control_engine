#pragma once
// Main → audio-thread commands and audio-thread → main replies (audio-input-ui §3.3). POD,
// ≤ 32 bytes, carried by the core SPSC RingBuffer.

#include "mce/audio/audio.h"

namespace mce::audio::detail {

enum class CmdType : u8 {
    Play, Stop, VoiceGain, VoicePan, BusGain, RegisterClip, ReleaseClip, StopAll,
    // v0.5
    VoiceGainPan, BusParent, BusEq, Limiter,
    // M-AUDIO2 2
    VoicePitch,
    // M-AUDIO2 3
    BusFilter, BusSend, Reverb,
};

namespace CmdFlag {
enum : u8 { Loop = 1u << 0 };
}

// Playback rate as a 4.12 fixed-point number (4096 = 1.0, the original pitch; M-AUDIO2 2).
inline constexpr u16 kRateUnit = 4096;

struct VoiceArgs {
    u32 generation;
    u16 clip;
    u16 rate;  // Play / VoicePitch: playback rate, kRateUnit = 1
    f32 gain;  // linear; Limiter: ceiling
    f32 pan;   // Play/VoicePan/VoiceGainPan; Stop: fade frames (as f32); Limiter: release coefficient
    u64 start_sample;  // Play: mixer frame to start on (0 = as soon as drained)
};

struct EqArgs {
    BiquadCoeffs coeffs;
};

// Freeverb's derived parameters, computed on the main thread.
struct ReverbArgs {
    f32 feedback;  // comb feedback (room size)
    f32 damp;      // comb low-pass amount (damping)
    f32 wet1, wet2;  // same-side and cross-side return gains (wet level and width)
};

struct ClipArgs {
    const f32* frames;
    u32 frame_count;
    u32 loop_start;
};

struct AudioCommand {
    CmdType type = CmdType::Play;
    u8 bus = 0;
    u8 priority = 0;
    u8 flags = 0;
    u32 index = 0;  // voice handle index (Play/Stop/Gain/Pan), clip slot (Register/Release), bus (BusGain)
    union {
        VoiceArgs voice;
        ClipArgs clip;
        EqArgs eq;
        ReverbArgs reverb;
    };
    // RegisterClip extras packed in the header bytes: bus = channels, priority = stream index + 1
    // (0 = in-memory clip). BusParent: index = bus, bus = parent. BusEq: index = bus, flags = band
    // (bit 7 = bypass). Limiter: flags = enabled. BusFilter: index = bus, voice.gain = low-pass Hz,
    // voice.pan = high-pass Hz (0 = off). BusSend: index = bus, flags = unit, voice.gain = linear send.
    // Reverb: index = unit, flags = enabled.

    AudioCommand() : voice{0, 0, kRateUnit, 0.f, 0.f, 0} {}
};
static_assert(sizeof(AudioCommand) <= 32, "AudioCommand must stay a small POD (§3.3)");

enum class ReplyType : u8 { VoiceEnded, ClipReleased };

struct AudioReply {
    ReplyType type = ReplyType::VoiceEnded;
    u32 index = 0;
    u32 generation = 0;
};

}  // namespace mce::audio::detail
