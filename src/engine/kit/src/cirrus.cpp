// The cirrus coverage helper (M-CIRRUS 53c): see mce/kit/cirrus.h. Integer hash and smoothstep only, no sin(), no rand.

#include "mce/kit/cirrus.h"

#include <cmath>

#include "mce/kit/weather.h"

namespace mce::kit {

namespace {

// A value in -1..1 from a whole day number (a 32-bit integer mix, the murmur finalizer).
f32 day_value(u32 day) {
    u32 h = day * 0x9E3779B1u + 0x85EBCA6Bu;
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return static_cast<f32>(h >> 8) * (2.0f / 16777215.0f) - 1.0f;
}

f32 unit_or(f32 v, f32 fallback) {
    if (!std::isfinite(v)) return fallback;
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

}  // namespace

f32 cirrus_day_fraction(u32 day, f32 hour) {
    const f32 h = hour > 0.0f ? (hour < 24.0f ? hour : 24.0f) : 0.0f;  // NaN fails both tests: 0
    return static_cast<f32>(day) + h / 24.0f;
}

f32 cirrus_coverage(const Cirrus& c, const Weather& w, f32 day_fraction) {
    const Cirrus defaults;
    const f32 strength = unit_or(c.strength, defaults.strength);
    if (strength == 0.0f) return 0.0f;
    f32 fair = unit_or(c.fair_coverage, defaults.fair_coverage);
    f32 front = unit_or(c.front_coverage, defaults.front_coverage);
    if (front < fair) {  // inverted: the cirrus would thin before a front
        fair = defaults.fair_coverage;
        front = defaults.front_coverage;
    }

    const f32 inten = unit_or(w.intensity, 0.0f);
    const bool clear = static_cast<u32>(w.current) >= 5 || w.current == WeatherKind::Clear;  // out-of-range reads as Clear
    f32 progress = 0.0f;
    if (w.target != w.current) progress = 1.0f;
    else if (!clear) progress = 1.0f - inten;
    f32 cover = fair + (front - fair) * progress;

    const f64 df = std::isfinite(day_fraction) && day_fraction > 0.0f ? std::fmin(static_cast<f64>(day_fraction), 4.0e9) : 0.0;
    const f64 whole = std::floor(df);
    const f32 t = static_cast<f32>(df - whole);
    const f32 s = t * t * (3.0f - 2.0f * t);
    const u32 day = static_cast<u32>(whole);
    const f32 a = day_value(day);
    const f32 b = day_value(day + 1u);
    cover += kCirrusDayVariation * (a + (b - a) * s);

    const f32 kind = clear ? 0.0f : (w.current == WeatherKind::Snow ? 0.9f : 1.0f);
    cover *= 1.0f - kCirrusOvercastCut * kind * inten;
    cover = cover > 0.0f ? (cover < 1.0f ? cover : 1.0f) : 0.0f;
    return cover * strength;
}

}  // namespace mce::kit
