#pragma once
// An opt-in meteor shower as data (M-METEOR step 18a). One optional Meteor per scene (the first in ECS order is read).
// install_weather registers the component; there is no system and nothing is stored between ticks.
//
//   Meteor  Authoring (all Save, clamped to the Range when read and never rewritten): `enabled` (1); `rate` (4, 0..30)
//           meteors per minute across the whole visible sky (0 draws none); `brightness` (1, 0..4) gain; `length` (1,
//           0.5..2) scales the angular length of the head-to-tail streak; `speed` (1, 0.5..2) scales the angular speed
//           (a faster meteor lasts a shorter time); `min_darkness` (0.6, 0..1) the night fade below which no meteor
//           shows (a smoothstep from it to 1); `night_only` (1, 0..1) 1: scaled by the night gate, 0: shows by day too;
//           `seed` (0, 0..65535) folded into the hash so two scenes differ.
//
// The helpers are pure functions of the authoring, the night fade, the cloud coverage, the rain, the wrapped time and
// an integer hash of (slot, index, seed); nothing is written anywhere. No rand, no hash of pointers, no pow/exp and
// no sin() of an unbounded argument (the trigonometry here is of angles bounded by one turn); smoothsteps and
// polynomials otherwise, evaluated in a fixed order. The sky shader mirrors the hash (pcg2d, one integer function).
//
// The time is wrapped in f64 to a 60 s period. The period divides into 12 slots of 5 s, so the pattern repeats
// exactly after it. A slot holds up to kMeteorMaxPerSlot (3) candidate streaks; a streak starts in the first half of
// its slot and lasts at most 2.4 s (1.2 s divided by the smallest `speed` 0.5), so it always ends inside its own slot:
// 2.5 s + 2.4 s = 4.9 s < 5 s. A streak is a great-circle arc: the head is the start rotated about
// cross(start, travel) by angular_speed * age; the tail is `length * 8` degrees behind it on the same arc.
//
// A zero level, `rate`, `brightness` or `enabled` 0 gives exactly 0 from meteor_level and no active streak, which the
// render off-check relies on.

#include <algorithm>
#include <cmath>

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct Meteor {
    f32 rate = 4.0f;
    f32 brightness = 1.0f;
    f32 length = 1.0f;
    f32 speed = 1.0f;
    f32 min_darkness = 0.6f;
    u32 seed = 0;
    u8 enabled = 1;
    u8 night_only = 1;
    u8 pad_[2] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(Meteor, (enabled, Save, Range{0, 1}, Help{"0: the meteors are off"}),
              (rate, Save, Range{0, 30}, Help{"meteors per minute across the whole visible sky; 0 draws none"}),
              (brightness, Save, Range{0, 4}, Help{"brightness gain"}),
              (length, Save, Range{0.5f, 2.0f}, Help{"scales the angular length of the streak"}),
              (speed, Save, Range{0.5f, 2.0f}, Help{"scales the angular speed; a faster meteor lasts a shorter time"}),
              (min_darkness, Save, Range{0, 1}, Help{"night fade below which no meteor shows"}),
              (night_only, Save, Range{0, 1}, Help{"1: scaled by the night gate; 0: shows by day too"}),
              (seed, Save, Range{0, 65535}, Help{"folded into the hash so two scenes differ"}))

// The seconds after which the pattern repeats exactly, the slot length and the slots per period.
inline constexpr f64 kMeteorPeriodSeconds = 60.0;
inline constexpr f32 kMeteorSlotSeconds = 5.0f;
inline constexpr u32 kMeteorSlotsPerPeriod = 12;
// Candidate streaks per slot; equal to the shader's simultaneous streak count.
inline constexpr u32 kMeteorMaxPerSlot = 3;

namespace detail {

// NaN falls back to `def`, anything else clamps to [lo, hi].
inline f32 meteor_clamp(f32 v, f32 def, f32 lo, f32 hi) { return std::isnan(v) ? def : std::clamp(v, lo, hi); }

// 0 at or below lo (exactly), 1 at or above hi (exactly), the cubic smoothstep between; lo >= hi counts as a step
// that is 1 from hi on.
inline f32 meteor_smooth(f32 lo, f32 hi, f32 x) {
    if (!(lo < hi)) return x >= hi ? 1.0f : 0.0f;
    if (!(x > lo)) return 0.0f;
    if (!(x < hi)) return 1.0f;
    const f32 t = (x - lo) / (hi - lo);
    return t * t * (3.0f - 2.0f * t);
}

inline constexpr f32 kMeteorTwoPi = 6.2831853071795864769f;
inline constexpr f32 kMeteorDegToRad = 0.017453292519943295769f;
// The head's angular speed at `speed` 1 and the tail length per unit `length`, degrees.
inline constexpr f32 kMeteorSpeedDegPerSec = 35.0f;
inline constexpr f32 kMeteorTailDeg = 8.0f;
// The start window (fraction of the slot) and the duration range at `speed` 1, seconds.
inline constexpr f32 kMeteorStartWindow = 0.5f;
inline constexpr f32 kMeteorDurationMin = 0.4f;
inline constexpr f32 kMeteorDurationSpan = 0.8f;
// The sine of the start elevation is drawn from [kMin, kMin + kSpan] = [0.1, 0.95].
inline constexpr f32 kMeteorSinElMin = 0.1f;
inline constexpr f32 kMeteorSinElSpan = 0.85f;

// pcg2d (Jarzynski and Olano 2020), one lane: the same integer hash as aurora_hash and the sky shader's cloud_hash.
inline u32 meteor_hash(u32 x, u32 y) {
    u32 vx = x * 1664525u + 1013904223u;
    u32 vy = y * 1664525u + 1013904223u;
    vx += vy * 1664525u;
    vy += vx * 1664525u;
    vx ^= vx >> 16u;
    vy ^= vy >> 16u;
    vx += vy * 1664525u;
    vy += vx * 1664525u;
    vx ^= vx >> 16u;
    return vx;
}

// A hash as a uniform in [0, 1): the top 24 bits.
inline f32 meteor_unit(u32 h) { return static_cast<f32>(h >> 8u) * (1.0f / 16777216.0f); }

inline u32 meteor_seed(const Meteor& m) { return std::min(m.seed, 65535u); }

inline f32 meteor_rate(const Meteor& m) { return meteor_clamp(m.rate, 4.0f, 0.0f, 30.0f); }
inline f32 meteor_brightness(const Meteor& m) { return meteor_clamp(m.brightness, 1.0f, 0.0f, 4.0f); }
// Zero and inverted-from-the-range values fall back to the default for the two scales (0 is outside their range).
inline f32 meteor_length(const Meteor& m) {
    return (std::isnan(m.length) || !(m.length > 0.0f)) ? 1.0f : std::clamp(m.length, 0.5f, 2.0f);
}
inline f32 meteor_speed(const Meteor& m) {
    return (std::isnan(m.speed) || !(m.speed > 0.0f)) ? 1.0f : std::clamp(m.speed, 0.5f, 2.0f);
}

// True when the component draws nothing at all: off, rate 0 or brightness 0.
inline bool meteor_silent(const Meteor& m) {
    return m.enabled == 0 || !(meteor_rate(m) > 0.0f) || !(meteor_brightness(m) > 0.0f);
}

}  // namespace detail

// One candidate streak of a slot. `start` and `travel` are unit vectors, travel perpendicular to start (a unit tangent
// at the start). Times are seconds from the slot's start; angles are radians.
struct MeteorStreak {
    math::vec3 start{0.0f, 1.0f, 0.0f};
    math::vec3 travel{1.0f, 0.0f, 0.0f};
    f32 start_time = 0.0f;     // seconds after the slot start, 0..2.5
    f32 duration = 0.0f;       // seconds, 0.4/speed .. 1.2/speed
    f32 angular_speed = 0.0f;  // radians per second
    f32 tail_angle = 0.0f;     // radians, the head-to-tail length
    bool active = false;
};

// The brightness gain's gate: enabled * night_gate * (1 - cloud)^2 * (1 - rain), with
// night_gate = smoothstep(min_darkness, 1, night_fade) (1 when `night_only` is 0). The inputs are clamped to 0..1 (NaN
// counts as 0). Brightness and rate are not folded in: the renderer reads them from the carrier. Exactly 0 when the
// component is off, `rate` or `brightness` is 0, or a gate is closed. In [0, 1].
inline f32 meteor_level(const Meteor& m, f32 night_fade, f32 cloud_coverage, f32 rain_intensity) {
    using detail::meteor_clamp;
    if (detail::meteor_silent(m)) return 0.0f;
    const f32 gate = m.night_only == 0
                         ? 1.0f
                         : detail::meteor_smooth(meteor_clamp(m.min_darkness, 0.6f, 0.0f, 1.0f), 1.0f,
                                                 meteor_clamp(night_fade, 0.0f, 0.0f, 1.0f));
    if (gate <= 0.0f) return 0.0f;
    const f32 clear = 1.0f - meteor_clamp(cloud_coverage, 0.0f, 0.0f, 1.0f);
    const f32 dry = 1.0f - meteor_clamp(rain_intensity, 0.0f, 0.0f, 1.0f);
    const f32 level = gate * (clear * clear) * dry;
    return level > 0.0f ? level : 0.0f;
}

// The time wrapped to [0, kMeteorPeriodSeconds) in f64; NaN and infinity count as 0.
inline f32 meteor_wrap_time(f64 t) {
    if (!std::isfinite(t)) return 0.0f;
    const f64 w = t - std::floor(t / kMeteorPeriodSeconds) * kMeteorPeriodSeconds;
    return static_cast<f32>(w >= kMeteorPeriodSeconds ? 0.0 : w);
}

struct MeteorSlot {
    u32 slot = 0;      // 0..11
    f32 frac = 0.0f;   // 0..1, the position in the slot
};

// The slot number (0..11) and the position in it (0..1) of the wrapped time (clamped to the period; NaN counts as 0).
inline MeteorSlot meteor_slot(f32 t_wrapped) {
    const f32 t = std::isnan(t_wrapped) ? 0.0f : std::clamp(t_wrapped, 0.0f, static_cast<f32>(kMeteorPeriodSeconds));
    const u32 slot = std::min(static_cast<u32>(t / kMeteorSlotSeconds), kMeteorSlotsPerPeriod - 1u);
    const f32 frac = (t - static_cast<f32>(slot) * kMeteorSlotSeconds) / kMeteorSlotSeconds;
    return MeteorSlot{slot, std::clamp(frac, 0.0f, 1.0f)};
}

// How many of the kMeteorMaxPerSlot candidate indices are active in `slot`: rate / 12 meteors per 5 s slot, so
// floor(rate / 12) of them always plus one more when the slot's hash draw is below the fractional part. 0 when silent.
// At most 3 (the maximum rate 30 gives 2.5).
inline u32 meteor_slot_count(const Meteor& m, u32 slot) {
    if (detail::meteor_silent(m)) return 0;
    const f32 per_slot = detail::meteor_rate(m) / static_cast<f32>(kMeteorSlotsPerPeriod);
    const f32 whole = std::floor(per_slot);
    u32 n = static_cast<u32>(whole);
    const f32 frac = per_slot - whole;
    if (frac > 0.0f && detail::meteor_unit(detail::meteor_hash(slot, 0xC0FFEEu ^ detail::meteor_seed(m))) < frac) ++n;
    return std::min(n, kMeteorMaxPerSlot);
}

// The streak of candidate `index` in `slot` (any slot number is accepted; the period repeats it only for slot % 12,
// which is how a caller derives it from meteor_slot). A pure function of (slot, index, seed) and the authoring's
// `speed` and `length`: pcg2d draws in a fixed order, from base = hash(slot, index << 16 | seed):
//   draw 0 azimuth (uniform turn), 1 sine of the elevation in [0.1, 0.95], 2 travel angle at the start (uniform turn),
//   3 start time (0..0.5 of the slot, 2.5 s), 4 duration (0.4..1.2 s divided by `speed`).
// The angular speed is 35 degrees per second times `speed`, so the arc is 14..42 degrees whatever the speed; the tail
// is `length * 8` degrees. `active` is false for an index at or above meteor_slot_count and for a silent component.
inline MeteorStreak meteor_make(const Meteor& m, u32 slot, u32 index) {
    using namespace detail;
    MeteorStreak s;
    s.active = index < meteor_slot_count(m, slot);
    const u32 base = meteor_hash(slot, ((index & 0xFFFFu) << 16u) | meteor_seed(m));
    const f32 az = meteor_unit(meteor_hash(base, 0u)) * kMeteorTwoPi;
    const f32 y = kMeteorSinElMin + kMeteorSinElSpan * meteor_unit(meteor_hash(base, 1u));
    const f32 ta = meteor_unit(meteor_hash(base, 2u)) * kMeteorTwoPi;
    const f32 st = meteor_unit(meteor_hash(base, 3u));
    const f32 du = meteor_unit(meteor_hash(base, 4u));
    const f32 r = std::sqrt(1.0f - y * y);
    s.start = math::vec3{r * std::cos(az), y, r * std::sin(az)};
    // The horizontal unit tangent e = cross(up, start) / r and the upward tangent n = cross(start, e).
    const math::vec3 e{s.start.z / r, 0.0f, -s.start.x / r};
    const math::vec3 n{s.start.y * e.z - s.start.z * e.y, s.start.z * e.x - s.start.x * e.z, s.start.x * e.y - s.start.y * e.x};
    const f32 ca = std::cos(ta);
    const f32 sa = std::sin(ta);
    s.travel = math::vec3{ca * e.x + sa * n.x, ca * e.y + sa * n.y, ca * e.z + sa * n.z};
    const f32 speed = meteor_speed(m);
    s.start_time = st * kMeteorStartWindow * kMeteorSlotSeconds;
    s.duration = (kMeteorDurationMin + kMeteorDurationSpan * du) / speed;
    s.angular_speed = kMeteorSpeedDegPerSec * kMeteorDegToRad * speed;
    s.tail_angle = kMeteorTailDeg * kMeteorDegToRad * meteor_length(m);
    return s;
}

namespace detail {

// The point on the streak's great circle at `angle` radians from the start towards `travel`.
inline math::vec3 meteor_arc(const MeteorStreak& s, f32 angle) {
    const f32 c = std::cos(angle);
    const f32 sn = std::sin(angle);
    return math::vec3{c * s.start.x + sn * s.travel.x, c * s.start.y + sn * s.travel.y, c * s.start.z + sn * s.travel.z};
}

inline f32 meteor_age(const MeteorStreak& s, f32 age) { return std::isnan(age) ? 0.0f : std::clamp(age, 0.0f, std::max(s.duration, 0.0f)); }

}  // namespace detail

// The head position (a unit vector) `age` seconds after the streak's start (clamped to 0..duration, NaN counts as 0).
inline math::vec3 meteor_head(const MeteorStreak& s, f32 age) {
    return detail::meteor_arc(s, s.angular_speed * detail::meteor_age(s, age));
}

// The tail position (a unit vector): `tail_angle` behind the head on the same great circle (the arc extends behind the
// start, so a new streak grows out of its start point).
inline math::vec3 meteor_tail(const MeteorStreak& s, f32 age) {
    return detail::meteor_arc(s, s.angular_speed * detail::meteor_age(s, age) - s.tail_angle);
}

// The brightness envelope 0..1 at `age` seconds after the start: a fast rise over the first 15 percent of the
// duration (smoothstep to 1), then a slow fall to 0 at the end. Exactly 0 outside [0, duration], for NaN, for an
// inactive streak and for a zero duration.
inline f32 meteor_envelope(const MeteorStreak& s, f32 age) {
    if (!s.active || std::isnan(age) || !(s.duration > 0.0f) || age < 0.0f || age > s.duration) return 0.0f;
    const f32 x = age / s.duration;
    return detail::meteor_smooth(0.0f, 0.15f, x) * (1.0f - detail::meteor_smooth(0.15f, 1.0f, x));
}

}  // namespace mce::kit
