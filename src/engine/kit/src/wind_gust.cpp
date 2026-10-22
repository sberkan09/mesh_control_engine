#include "mce/kit/wind_gust.h"

#include <algorithm>
#include <cmath>

#include "mce/ecs/world.h"
#include "mce/kit/weather.h"

namespace mce::kit {
namespace {

constexpr u32 kUnit = 1u << 24;
constexpr u64 kMaxPhaseTicks = 1ull << 16;  // every phase is capped here so the Q24 products stay inside u64

// The same mixer as lightning_bolt.cpp and precipitation.cpp (kept private there).
u32 hash_mix(u32 seed, u32 counter) {
    u64 z = (static_cast<u64>(seed) << 32 | counter) + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<u32>(z >> 32);
}

f64 clean(f32 v, f64 fallback, f64 lo, f64 hi) { return std::isnan(v) ? fallback : std::clamp(static_cast<f64>(v), lo, hi); }

// [lo, hi] authoring pair: NaN to the default, clamp, swap if inverted.
void pair_of(f32 a, f32 b, f64 da, f64 db, f64 lo, f64 hi, f64& out_a, f64& out_b) {
    out_a = clean(a, da, lo, hi);
    out_b = clean(b, db, lo, hi);
    if (out_a > out_b) std::swap(out_a, out_b);
}

// A value between a and b chosen by the top 24 bits of h.
f64 pick(u32 h, f64 a, f64 b) { return a + (b - a) * (static_cast<f64>(h >> 8) / static_cast<f64>(kUnit)); }

// ceil(seconds * rate) with rate = ticks per second; the 1e-6 absorbs the f32 to f64 error of an authored 0.4 or 0.1.
u32 ticks_of(f64 seconds, f64 rate, u64 least) {
    f64 x = std::ceil(seconds * rate - 1e-6);
    if (!(x < static_cast<f64>(kMaxPhaseTicks))) x = static_cast<f64>(kMaxPhaseTicks);
    return static_cast<u32>(std::max<u64>(least, static_cast<u64>(std::max(x, 0.0))));
}

void build_segment(WindGust& g, f64 dt) {
    f64 gap_lo, gap_hi, rise_lo, rise_hi, fall_lo, fall_hi, peak_lo, peak_hi;
    pair_of(g.min_gap_s, g.max_gap_s, 2.0, 8.0, 0.0, 120.0, gap_lo, gap_hi);
    pair_of(g.min_rise_s, g.max_rise_s, 0.4, 1.5, 0.05, 30.0, rise_lo, rise_hi);
    pair_of(g.min_fall_s, g.max_fall_s, 1.0, 3.0, 0.05, 60.0, fall_lo, fall_hi);
    pair_of(g.min_peak, g.max_peak, 0.4, 1.0, 0.0, 1.0, peak_lo, peak_hi);
    // dt snapped to a whole tick rate (f32 1/60 is not exactly 1/60), as kit_wind_direction does.
    f64 rate = 1.0 / dt;
    const f64 r = std::round(rate);
    if (r >= 1.0 && std::abs(rate - r) < 1e-4 * r) rate = r;
    const u32 base = g.segment * 4u;
    g.gap_ticks = ticks_of(pick(hash_mix(g.seed, base), gap_lo, gap_hi), rate, 0);
    g.rise_ticks = ticks_of(pick(hash_mix(g.seed, base + 1u), rise_lo, rise_hi), rate, 1);
    g.fall_ticks = ticks_of(pick(hash_mix(g.seed, base + 2u), fall_lo, fall_hi), rate, 1);
    const f64 peak = pick(hash_mix(g.seed, base + 3u), peak_lo, peak_hi);
    g.peak_q = static_cast<u32>(std::clamp<i64>(std::llround(peak * static_cast<f64>(kUnit)), 0, kUnit));
}

// smoothstep(n / d) in Q24, for 0 <= n <= d <= 2^16: x^2 (3 - 2x), integer only.
u64 smooth_q(u64 n, u64 d) {
    const u64 x = (n << 24) / d;
    const u64 x2 = (x * x) >> 24;
    return (x2 * (3ull * kUnit - 2ull * x)) >> 24;
}

u32 level_of(const WindGust& g) {
    const u64 gap = g.gap_ticks;
    const u64 rise = std::max<u64>(1, std::min<u64>(g.rise_ticks, kMaxPhaseTicks));
    const u64 fall = std::max<u64>(1, std::min<u64>(g.fall_ticks, kMaxPhaseTicks));
    const u64 peak = std::min<u32>(g.peak_q, kUnit);
    const u64 t = g.tick;
    if (t <= gap) return 0;
    const u64 r = t - gap;
    u64 s;
    if (r <= rise) {
        s = smooth_q(r, rise);
    } else {
        const u64 f = r - rise;
        s = f >= fall ? 0 : smooth_q(fall - f, fall);
    }
    return static_cast<u32>((peak * s) >> 24);
}

void kit_wind_gust(Frame& f) {
    const Weather* weather = nullptr;
    f.query<const Weather>().each([&](Entity, const Weather& w) {
        if (!weather) weather = &w;
    });
    if (!weather) return;
    const f64 dt = static_cast<f64>(f.dt);
    bool first = true;  // one WindGust per scene: the first in ECS order drives
    f.query<WindGust>().each([&](Entity, WindGust& g) {
        if (!first) return;
        first = false;
        if (!g.enabled) return;
        if (!(dt > 0.0) || std::isinf(dt)) return;  // no advance
        g.pad_[0] = 0;
        g.pad_[1] = 0;
        if (g.seeded != 1) {
            g.seeded = 1;
            g.tick = 0;
            g.gust_q = 0;
            build_segment(g, dt);
        }
        ++g.tick;
        g.gust_q = level_of(g);
        const u64 total = static_cast<u64>(g.gap_ticks) + std::max<u64>(1, g.rise_ticks) + std::max<u64>(1, g.fall_ticks);
        if (g.tick >= total) {  // the fall is over (level 0): the next segment
            ++g.segment;
            g.tick = 0;
            build_segment(g, dt);
        }
    });
}

}  // namespace

void install_wind_gust(World& world) { world.add_system(Phase::FixedUpdate_Pre, "kit_wind_gust", &kit_wind_gust, "kit_wind_direction"); }

}  // namespace mce::kit
