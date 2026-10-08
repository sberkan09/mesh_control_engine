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

}  // namespace mce::audio::detail
