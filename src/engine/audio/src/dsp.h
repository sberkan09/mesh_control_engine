#pragma once
// Real-time DSP nodes (audio-input-ui §3.7, FR-AUD-060): plain state structs + one process call,
// scalar-first. Runs on the audio thread: no heap, no locks.

#include <cmath>

#include "mce/audio/audio.h"

namespace mce::audio::detail {

// One biquad band on a stereo bus, transposed direct form II (good float behavior at low cutoffs).
struct BiquadRT {
    BiquadCoeffs c;
    f32 z1[2] = {}, z2[2] = {};
    bool on = false;

    void set(const BiquadCoeffs& coeffs, bool enabled) {
        if (!enabled || !on) {
            for (u32 ch = 0; ch < 2; ++ch) z1[ch] = z2[ch] = 0.f;  // a fresh band starts at rest
        }
        c = coeffs;
        on = enabled;
    }

    // In-place over `frames` interleaved stereo frames.
    void process(f32* buf, u32 frames) {
        if (!on) return;
        for (u32 ch = 0; ch < 2; ++ch) {
            f32 s1 = z1[ch], s2 = z2[ch];
            for (u32 i = 0; i < frames; ++i) {
                const f32 x = buf[i * 2 + ch];
                const f32 y = c.b0 * x + s1;
                s1 = c.b1 * x - c.a1 * y + s2;
                s2 = c.b2 * x - c.a2 * y;
                buf[i * 2 + ch] = y;
            }
            // Flush denormals: a decayed tail must not slow the callback down.
            z1[ch] = std::fabs(s1) < 1e-20f ? 0.f : s1;
            z2[ch] = std::fabs(s2) < 1e-20f ? 0.f : s2;
        }
    }
};

// A 12 dB/octave low-pass or high-pass as a trapezoidal state-variable filter (Simper's SVF), for
// the bus filters (M-AUDIO2 3). Its response is the bilinear Butterworth biquad's, but its state is
// the two integrators', so a cutoff that moves every few samples does not ring the way a direct
// form's does near Nyquist.
struct SvfRT {
    f32 a1 = 0.f, a2 = 0.f, a3 = 0.f;
    f32 ic1[2] = {}, ic2[2] = {};
    bool high = false;
    bool on = false;

    void tune(f32 cutoff_hz, bool highpass) {
        const f32 g = std::tan(3.14159265f * cutoff_hz / static_cast<f32>(kSampleRate));
        constexpr f32 k = 1.41421356f;  // 1 / Q, Q = 1/sqrt(2)
        a1 = 1.f / (1.f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
        high = highpass;
    }
    void rest() {
        for (u32 ch = 0; ch < 2; ++ch) ic1[ch] = ic2[ch] = 0.f;
    }
    // The state a low-pass settles to with `frame` held as its input.
    void prime(const f32* frame) {
        for (u32 ch = 0; ch < 2; ++ch) {
            ic1[ch] = 0.f;
            ic2[ch] = frame[ch];
        }
    }

    void process(f32* buf, u32 frames) {
        if (!on) return;
        constexpr f32 k = 1.41421356f;
        for (u32 ch = 0; ch < 2; ++ch) {
            f32 s1 = ic1[ch], s2 = ic2[ch];
            for (u32 i = 0; i < frames; ++i) {
                const f32 v0 = buf[i * 2 + ch];
                const f32 v3 = v0 - s2;
                const f32 v1 = a1 * s1 + a2 * v3;
                const f32 v2 = s2 + a2 * s1 + a3 * v3;
                s1 = 2.f * v1 - s1;
                s2 = 2.f * v2 - s2;
                buf[i * 2 + ch] = high ? v0 - k * v1 - v2 : v2;
            }
            ic1[ch] = std::fabs(s1) < 1e-20f ? 0.f : s1;
            ic2[ch] = std::fabs(s2) < 1e-20f ? 0.f : s2;
        }
    }
};

// Look-ahead limiter (§3.7). Per sample: required gain r = min(1, ceiling / peak). A sliding-window
// minimum of r over the last A samples (monotonic deque) is released upward exponentially, then
// box-averaged over A samples, and applied to the input delayed by A - 1. Every value inside the
// box window holds the minimum over a window that contains the delayed sample, so the applied gain
// never exceeds what that sample needs: no overshoot, and the gain ramps down over A samples
// instead of jumping (no click).
struct LimiterRT {
    static constexpr u32 A = kLimiterLookahead;
    static constexpr u32 kDelay = A - 1;

    bool enabled = false;
    f32 ceiling = 1.f;
    f32 release = 0.f;  // per-sample approach factor toward unity gain

    f32 delay_l[A] = {}, delay_r[A] = {};
    u32 delay_pos = 0;
    f32 dq_val[A] = {};
    u64 dq_idx[A] = {};
    u32 dq_head = 0, dq_size = 0;
    f32 box[A] = {};
    u32 box_pos = 0;
    f64 box_sum = 0.0;
    f32 env = 1.f;
    u64 n = 0;

    void configure(bool on, f32 ceiling_lin, f32 release_coef) {
        if (on && !enabled) reset();
        enabled = on;
        ceiling = ceiling_lin;
        release = release_coef;
    }

    void reset() {
        for (u32 i = 0; i < A; ++i) {
            delay_l[i] = delay_r[i] = 0.f;
            box[i] = 1.f;
        }
        delay_pos = box_pos = dq_head = dq_size = 0;
        box_sum = A;
        env = 1.f;
        n = 0;
    }

    // Returns the gain applied to the delayed sample written back into l/r.
    f32 process(f32& l, f32& r) {
        const f32 peak = std::fabs(l) > std::fabs(r) ? std::fabs(l) : std::fabs(r);
        const f32 req = peak > ceiling ? ceiling / peak : 1.f;

        // Sliding-window minimum over [n - A + 1, n]: expire first, so the ring never holds more
        // than A entries.
        if (dq_size > 0 && dq_idx[dq_head] + A <= n) {
            dq_head = (dq_head + 1) % A;
            --dq_size;
        }
        while (dq_size > 0 && dq_val[(dq_head + dq_size - 1) % A] >= req) --dq_size;
        dq_val[(dq_head + dq_size) % A] = req;
        dq_idx[(dq_head + dq_size) % A] = n;
        ++dq_size;
        const f32 hold = dq_val[dq_head];

        const f32 released = env + (1.f - env) * release;
        env = hold < released ? hold : released;

        box_sum += static_cast<f64>(env) - static_cast<f64>(box[box_pos]);
        box[box_pos] = env;
        box_pos = (box_pos + 1) % A;
        f32 g = static_cast<f32>(box_sum / static_cast<f64>(A));
        if (g > 1.f) g = 1.f;

        // Delay line of kDelay samples: read the oldest, write the newest.
        const f32 dl = delay_l[delay_pos], dr = delay_r[delay_pos];
        delay_l[delay_pos] = l;
        delay_r[delay_pos] = r;
        delay_pos = (delay_pos + 1) % kDelay;
        l = dl * g;
        r = dr * g;
        ++n;
        return g;
    }
};

// Freeverb (Jezar at Dreampoint, public domain), M-AUDIO2 3: per channel, eight damped feedback
// combs in parallel and four allpasses in series, the right channel's delays 23 samples longer.
// Delay lengths are the original 44.1 kHz tunings scaled to 48 kHz. The state is fixed arrays,
// so a unit costs no allocation; a decayed tail is flushed to zero so silence stays cheap.
namespace freeverb {
inline constexpr u32 kCombs = 8;
inline constexpr u32 kAllpasses = 4;
inline constexpr u32 kCombTuning[kCombs] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
inline constexpr u32 kAllpassTuning[kAllpasses] = {556, 441, 341, 225};
inline constexpr u32 kSpread = 23;
inline constexpr f32 kFixedGain = 0.015f;
inline constexpr f32 kScaleRoom = 0.28f, kOffsetRoom = 0.7f, kScaleDamp = 0.4f;
inline constexpr u32 scaled(u32 n44) { return (n44 * kSampleRate + 22050) / 44100; }
inline constexpr u32 kMaxComb = scaled(1617 + kSpread);
inline constexpr u32 kMaxAllpass = scaled(556 + kSpread);
inline f32 flush(f32 v) { return std::fabs(v) < 1e-20f ? 0.f : v; }
}  // namespace freeverb

struct ReverbRT {
    struct Comb {
        f32 buf[freeverb::kMaxComb];
        u32 len = 1, pos = 0;
        f32 store = 0.f;
        f32 process(f32 x, f32 feedback, f32 damp) {
            const f32 out = buf[pos];
            store = freeverb::flush(out * (1.f - damp) + store * damp);
            buf[pos] = freeverb::flush(x + store * feedback);
            if (++pos == len) pos = 0;
            return out;
        }
    };
    struct Allpass {
        f32 buf[freeverb::kMaxAllpass];
        u32 len = 1, pos = 0;
        f32 process(f32 x) {
            const f32 b = buf[pos];
            buf[pos] = freeverb::flush(x + b * 0.5f);
            if (++pos == len) pos = 0;
            return b - x;
        }
    };

    bool enabled = false;
    f32 feedback = 0.84f, damp = 0.2f, wet1 = 1.f, wet2 = 0.f;
    Comb comb[2][freeverb::kCombs];
    Allpass allpass[2][freeverb::kAllpasses];

    ReverbRT() {
        for (u32 ch = 0; ch < 2; ++ch) {
            const u32 spread = ch == 0 ? 0 : freeverb::kSpread;
            for (u32 i = 0; i < freeverb::kCombs; ++i) comb[ch][i].len = freeverb::scaled(freeverb::kCombTuning[i] + spread);
            for (u32 i = 0; i < freeverb::kAllpasses; ++i) allpass[ch][i].len = freeverb::scaled(freeverb::kAllpassTuning[i] + spread);
        }
        reset();
    }

    void reset() {
        for (u32 ch = 0; ch < 2; ++ch) {
            for (Comb& c : comb[ch]) {
                for (f32& v : c.buf) v = 0.f;
                c.pos = 0;
                c.store = 0.f;
            }
            for (Allpass& a : allpass[ch]) {
                for (f32& v : a.buf) v = 0.f;
                a.pos = 0;
            }
        }
    }

    void configure(bool on, f32 fb, f32 dm, f32 w1, f32 w2) {
        if (on && !enabled) reset();
        enabled = on;
        feedback = fb;
        damp = dm;
        wet1 = w1;
        wet2 = w2;
    }

    // Reads `frames` stereo frames of send from `in` and adds the wet return into `out`.
    void process(const f32* in, f32* out, u32 frames) {
        if (!enabled) return;
        for (u32 i = 0; i < frames; ++i) {
            const f32 x = (in[i * 2] + in[i * 2 + 1]) * freeverb::kFixedGain;
            f32 o[2] = {0.f, 0.f};
            for (u32 ch = 0; ch < 2; ++ch) {
                for (Comb& c : comb[ch]) o[ch] += c.process(x, feedback, damp);
                for (Allpass& a : allpass[ch]) o[ch] = a.process(o[ch]);
            }
            out[i * 2] += o[0] * wet1 + o[1] * wet2;
            out[i * 2 + 1] += o[1] * wet1 + o[0] * wet2;
        }
    }
};

}  // namespace mce::audio::detail
