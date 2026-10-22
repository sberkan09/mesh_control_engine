#include "mce/kit/lightning_bolt.h"

#include <algorithm>
#include <cmath>

namespace mce::kit {
namespace {

constexpr f64 kPi = 3.14159265358979323846;
constexpr f64 kNominalSpeed = 343.0;
constexpr f64 kMaxTicks = 2147483648.0;  // 2^31

// The same mixer as precipitation.cpp (kept private there): the strike streams share one seed.
u32 hash_mix(u32 seed, u32 counter) {
    u64 z = (static_cast<u64>(seed) << 32 | counter) + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<u32>(z >> 32);
}

f64 range_of(f32 v, f64 fallback) {
    if (std::isnan(v)) return fallback;
    return std::clamp(static_cast<f64>(v), 10.0, 5000.0);
}

}  // namespace

BoltStrike bolt_strike_position(const LightningBolt& b, u32 seed, u32 counter, const math::vec3& origin) {
    f64 lo = range_of(b.min_range_m, 150.0), hi = range_of(b.max_range_m, 900.0);
    if (lo > hi) std::swap(lo, hi);
    const f64 ub = static_cast<f64>(hash_mix(seed, counter * 4u + 2u)) / 4294967296.0;
    const f64 ur = static_cast<f64>(hash_mix(seed, counter * 4u + 3u)) / 4294967296.0;
    const f64 bearing = 2.0 * kPi * ub;
    const f64 range = lo + (hi - lo) * ur;
    const f64 gy = std::isnan(b.ground_y) ? 0.0 : static_cast<f64>(b.ground_y);
    const f64 x = static_cast<f64>(origin.x) + range * std::sin(bearing);
    const f64 z = static_cast<f64>(origin.z) + range * std::cos(bearing);
    const f64 dx = x - static_cast<f64>(origin.x), dz = z - static_cast<f64>(origin.z);
    const f64 dy = gy - static_cast<f64>(origin.y);
    BoltStrike s;
    s.x = static_cast<f32>(x);
    s.z = static_cast<f32>(z);
    s.distance_m = static_cast<f32>(std::sqrt(dx * dx + dy * dy + dz * dz));
    return s;
}

u32 bolt_delay_ticks(f32 distance_m, f64 dt) {
    if (!(dt > 0.0) || !(distance_m > 0.0f)) return 1;
    const f64 n = std::ceil(static_cast<f64>(distance_m) / kNominalSpeed / dt);
    if (!(n >= 1.0)) return 1;
    return static_cast<u32>(std::min(n, kMaxTicks));
}

}  // namespace mce::kit
