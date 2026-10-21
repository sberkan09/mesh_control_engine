#pragma once
// Shared by SimNetwork and Host::set_conditions: the deterministic rolls that turn LinkConditions
// into a delay or a drop for one message (M-NET2 step 1).

#include "mce/net/transport.h"

namespace mce::net::detail {

// xorshift64*: small, fast and the same on every platform and compiler.
inline u32 next_random(u64& state) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return static_cast<u32>((state * 0x2545F4914F6CDD1DULL) >> 32);
}

// A seed of 0 would stick at 0.
inline u64 seed_random(u64 seed) { return seed ? seed : 0x9E3779B97F4A7C15ULL; }

// True when `loss` (0..1) hits. Compared in 1/65536ths so no float rounding differs by platform.
inline bool roll_loss(u64& state, f32 loss) {
    if (!(loss > 0.0f)) return false;
    const u32 threshold = loss >= 1.0f ? 65536u : static_cast<u32>(loss * 65536.0f);
    return (next_random(state) & 0xFFFFu) < threshold;
}

// The delay of one message in ms, or false when it is lost. A lost Reliable message is resent after
// a round trip (twice the latency, at least 1 ms), up to 16 times, so it arrives late instead.
inline bool roll_delay(u64& state, const LinkConditions& c, Delivery delivery, u32& delay_ms) {
    const u32 jitter = c.jitter_ms ? next_random(state) % (c.jitter_ms + 1) : 0;
    delay_ms = c.latency_ms + jitter;
    if (delivery != Delivery::Reliable) return !roll_loss(state, c.loss);
    const u32 resend = c.latency_ms * 2 > 0 ? c.latency_ms * 2 : 1;
    for (u32 tries = 0; tries < 16 && roll_loss(state, c.loss); ++tries) delay_ms += resend;
    return true;
}

}  // namespace mce::net::detail
