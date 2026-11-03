#pragma once
// Light flicker as data (M-FLICKER step 36a). Any number of LightFlicker3D entities; install_kit3d registers the component
// next to the light components. There is no system and nothing is stored between ticks: the scale is a pure function of the
// authoring, the render clock and a per-entity salt, so it never feeds the simulation or the state hash.
//
//   LightFlicker3D  Authoring (all Save, clamped to the Range when read and never rewritten): `enabled` (1); `style` (0 fire,
//                   1 failing lamp, 2 pulse; anything else counts as fire); `depth` (0.35, 0..1) how far the intensity may
//                   drop, 0 = off; `rate_hz` (8, 0.1..30) the main noise rate; `radius_depth` (0.05, 0..0.5) how far the
//                   radius may shrink with the intensity, 0 = steady; `seed` (1); `phase` (0, 0..1) the start of the pulse
//                   cycle (the pulse style only).
//
// light_flicker_scale(f, time_s, entity_salt) = 1 - depth * x and light_flicker_radius_scale(...) = 1 - radius_depth * x with
// the same x in 0..1, so the pool of light breathes with the intensity. x per style:
//   fire          smooth value noise from an integer hash at rate_hz, plus a second octave at 2.7x rate weighted 0.4 and a
//                 third at 6.1x weighted 0.2, normalised to 0..1;
//   failing lamp  mostly 0 (steady), with dropouts to 1: the hash of floor(time * rate_hz) makes a cell a dropout with
//                 probability 0.15, and the edge between two cells is a smoothstep over the first 20% of the later cell;
//   pulse         0.5 - 0.5 * cos(2 pi (time * rate_hz * 0.1 + phase)), a sine at rate_hz / 10 Hz (a polynomial sine, so
//                 the result is the same on every libm).
// The salt mixes with the seed (not an addition), so two lamps with the same seed still differ; the pulse style has no hash
// and two pulses differ by `phase` only. Integer hash and polynomials only, a fixed evaluation order, no rand, no state.
// Time is reduced in f64 (fmod) before it narrows, so a large render clock keeps its precision; the noise is periodic with
// 65536 cells of its octave (2.3 hours at 8 Hz), seamless at the wrap. NaN or infinite time counts as 0; NaN authoring falls
// back to the defaults above and out-of-range values clamp. `enabled` 0 or `depth` 0 (radius_depth 0 for the radius) return
// exactly 1.0f. The result is always finite and in [1 - depth, 1] ([1 - radius_depth, 1]).

#include <algorithm>
#include <cmath>

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct LightFlicker3D {
    u32 style = 0;  // 0 fire, 1 failing lamp, 2 pulse
    f32 depth = 0.35f;
    f32 rate_hz = 8.0f;
    f32 radius_depth = 0.05f;
    u32 seed = 1;
    f32 phase = 0.0f;
    u8 enabled = 1;
    u8 pad_[3] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(LightFlicker3D, (enabled, Save, Range{0, 1}, Help{"0: the flicker is off"}),
              (style, Save, Range{0, 2}, Help{"0 fire, 1 failing lamp, 2 pulse"}),
              (depth, Save, Range{0, 1}, Help{"how far the intensity may drop; 0 = off"}),
              (rate_hz, Save, Range{0.1f, 30}, Help{"main noise rate, Hz (pulse: a tenth of it)"}),
              (radius_depth, Save, Range{0, 0.5f}, Help{"how far the radius may shrink with the intensity"}),
              (seed, Save, Help{"hash seed of the noise"}),
              (phase, Save, Range{0, 1}, Help{"start of the pulse cycle (pulse style)"}))

namespace detail {

constexpr u32 kFlickerPeriodCells = 65536u;  // cells of an octave before the noise repeats (seamlessly)

inline f32 flicker_clamp(f32 v, f32 def, f32 lo, f32 hi) { return std::isnan(v) ? def : std::clamp(v, lo, hi); }

// A 32-bit finaliser (lowbias32 style) over (key, cell, octave).
inline u32 flicker_hash(u32 key, u32 cell, u32 octave) {
    u32 h = key * 0x9E3779B1u + 0x7F4A7C15u;
    h ^= cell * 0x85EBCA6Bu + 0xC2B2AE35u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= octave * 0x27D4EB2Fu + 0x165667B1u;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

// 0..1 (exclusive of 1) from the top 24 bits.
inline f32 flicker_unit(u32 h) { return static_cast<f32>(h >> 8) * (1.0f / 16777216.0f); }

// The per-lamp key: the seed and the salt go through the finaliser separately, so (seed, salt) and (salt, seed) differ.
inline u32 flicker_key(u32 seed, u32 salt) { return flicker_hash(seed ^ 0xA511E9B3u, salt, 0x51Fu); }

inline f32 flicker_smooth(f32 t) { return t * t * (3.0f - 2.0f * t); }

// time * mul, reduced into [0, kFlickerPeriodCells) in f64; non-finite time counts as 0.
inline f64 flicker_cells(f64 time_s, f64 mul) {
    if (!std::isfinite(time_s)) time_s = 0.0;
    f64 u = std::fmod(time_s * mul, static_cast<f64>(kFlickerPeriodCells));
    if (u < 0.0) u += static_cast<f64>(kFlickerPeriodCells);
    if (!(u < static_cast<f64>(kFlickerPeriodCells))) u = 0.0;  // -0 or a rounding to the period itself
    return u;
}

struct FlickerCell {
    u32 index;  // 0..kFlickerPeriodCells-1
    f32 frac;   // 0..1 (exclusive of 1)
};

inline FlickerCell flicker_split(f64 u) {
    const f64 fl = std::floor(u);
    FlickerCell c;
    c.index = static_cast<u32>(fl) % kFlickerPeriodCells;
    c.frac = std::clamp(static_cast<f32>(u - fl), 0.0f, 1.0f);
    return c;
}

// Smooth value noise 0..1 at `u` cells: the hashes of the two cells around it, eased.
inline f32 flicker_noise(u32 key, f64 u, u32 octave) {
    const FlickerCell c = flicker_split(u);
    const f32 a = flicker_unit(flicker_hash(key, c.index, octave));
    const f32 b = flicker_unit(flicker_hash(key, (c.index + 1u) % kFlickerPeriodCells, octave));
    return a + (b - a) * flicker_smooth(c.frac);
}

inline f32 flicker_dropout(u32 key, u32 cell) { return flicker_unit(flicker_hash(key, cell, 7u)) < 0.15f ? 1.0f : 0.0f; }

// sin(pi p) squared for p in [0, 1]: sin(pi p) ~ y (0.775 + 0.225 y) with y = 4 p (1 - p) (error < 0.002), so x =
// 0.5 - 0.5 cos(2 pi p) is 0 at p 0 and 1 and exactly 1 at 0.5.
inline f32 flicker_pulse(f64 time_s, f32 rate, f32 phase) {
    if (!std::isfinite(time_s)) time_s = 0.0;
    f64 p = std::fmod(time_s * static_cast<f64>(rate) * 0.1, 1.0) + static_cast<f64>(phase);
    p -= std::floor(p);
    const f32 pf = std::clamp(static_cast<f32>(p), 0.0f, 1.0f);
    const f32 y = 4.0f * pf * (1.0f - pf);
    const f32 s = y * (0.775f + 0.225f * y);
    return std::clamp(s * s, 0.0f, 1.0f);
}

// The shared x in 0..1 (0 = full brightness, 1 = deepest dip) of one lamp at one time.
inline f32 flicker_x(const LightFlicker3D& f, f64 time_s, u32 entity_salt) {
    const f32 rate = flicker_clamp(f.rate_hz, 8.0f, 0.1f, 30.0f);
    const u32 key = flicker_key(f.seed, entity_salt);
    f32 x = 0.0f;
    if (f.style == 2u) {
        x = flicker_pulse(time_s, rate, flicker_clamp(f.phase, 0.0f, 0.0f, 1.0f));
    } else if (f.style == 1u) {
        const f64 u = flicker_cells(time_s, static_cast<f64>(rate));
        const FlickerCell c = flicker_split(u);
        const u32 prev = (c.index + kFlickerPeriodCells - 1u) % kFlickerPeriodCells;
        const f32 d0 = flicker_dropout(key, prev);
        const f32 d1 = flicker_dropout(key, c.index);
        const f32 t = std::min(c.frac * 5.0f, 1.0f);  // the edge: the first 20% of the cell
        x = d0 + (d1 - d0) * flicker_smooth(t);
    } else {
        const f32 n1 = flicker_noise(key, flicker_cells(time_s, static_cast<f64>(rate)), 0u);
        const f32 n2 = flicker_noise(key, flicker_cells(time_s, static_cast<f64>(rate) * 2.7), 1u);
        const f32 n3 = flicker_noise(key, flicker_cells(time_s, static_cast<f64>(rate) * 6.1), 2u);
        x = (n1 + 0.4f * n2 + 0.2f * n3) * (1.0f / 1.6f);
    }
    return std::isnan(x) ? 0.0f : std::clamp(x, 0.0f, 1.0f);
}

}  // namespace detail

// The intensity multiplier 1 - depth * x, in [1 - depth, 1]; exactly 1.0f when disabled or depth is 0.
inline f32 light_flicker_scale(const LightFlicker3D& f, f64 time_s, u32 entity_salt) {
    if (f.enabled == 0u) return 1.0f;
    const f32 depth = detail::flicker_clamp(f.depth, 0.35f, 0.0f, 1.0f);
    if (depth == 0.0f) return 1.0f;
    const f32 x = detail::flicker_x(f, time_s, entity_salt);
    return std::clamp(1.0f - depth * x, 1.0f - depth, 1.0f);
}

// The radius multiplier 1 - radius_depth * x with the same x, in [1 - radius_depth, 1]; exactly 1.0f when disabled or
// radius_depth is 0. It does not depend on `depth`: a light with depth 0 can still breathe its pool.
inline f32 light_flicker_radius_scale(const LightFlicker3D& f, f64 time_s, u32 entity_salt) {
    if (f.enabled == 0u) return 1.0f;
    const f32 rd = detail::flicker_clamp(f.radius_depth, 0.05f, 0.0f, 0.5f);
    if (rd == 0.0f) return 1.0f;
    const f32 x = detail::flicker_x(f, time_s, entity_salt);
    return std::clamp(1.0f - rd * x, 1.0f - rd, 1.0f);
}

}  // namespace mce::kit
