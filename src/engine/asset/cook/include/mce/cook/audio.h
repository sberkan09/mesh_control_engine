#pragma once
// Cook-time audio processing (assets.md §5.2 Audio row): own WAV reader (PCM 8/16/24/32-bit,
// IEEE float 32/64, WAVE_FORMAT_EXTENSIBLE), windowed-sinc resampler, peak normalization.
// Exposed for tests; the runtime never sees WAV.

#include "mce/containers/array.h"
#include "mce/fixed_string.h"

namespace mce::cook {

struct PcmClip {
    u32 sample_rate = 0;
    u32 channels = 0;
    Array<f32> samples;  // interleaved, [-1, 1]
    bool has_loop = false;  // from a 'smpl' chunk
    u64 loop_start = 0;     // frames, [start, end)
    u64 loop_end = 0;

    u64 frames() const { return channels ? samples.size() / channels : 0; }
};

// Returns false and sets `why` on malformed or unsupported input.
bool decode_wav(Span<const byte> bytes, PcmClip& out, FixedString<128>& why);

// Band-limited resampling with a fixed 32-tap Blackman-windowed sinc kernel (cutoff at the lower
// Nyquist). Exact rational stepping (no accumulated drift), double accumulation, deterministic.
// Output length = ceil(frames * dst / src).
void resample(Span<const f32> in, u32 channels, u32 src_rate, u32 dst_rate, Array<f32>& out);

// Frame position mapped from src_rate to dst_rate, rounded to nearest.
u64 rescale_frames(u64 frames, u32 src_rate, u32 dst_rate);

// Scales so the absolute peak sits at `target` (linear); returns the gain applied (1 if silent).
f32 normalize_peak(Span<f32> samples, f32 target);

inline constexpr u32 kCookSampleRate = 48000;
inline constexpr f32 kMinus1dBFS = 0.891250938f;

}  // namespace mce::cook
