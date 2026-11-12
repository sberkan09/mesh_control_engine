// The halo strength helper (M-HALO 54c): see mce/kit/halo.h. Pure arithmetic, no sin(), no rand.

#include "mce/kit/halo.h"

#include <cmath>

namespace mce::kit {

f32 halo_level(const Halo& h, f32 cirrus_coverage) {
    const Halo defaults;
    f32 strength = std::isfinite(h.strength) ? h.strength : defaults.strength;
    strength = strength < 0.0f ? 0.0f : (strength > 1.0f ? 1.0f : strength);
    if (strength == 0.0f) return 0.0f;
    if (!(cirrus_coverage > kHaloCoverLo)) return 0.0f;  // NaN fails the test too
    f32 t = (cirrus_coverage - kHaloCoverLo) / (kHaloCoverHi - kHaloCoverLo);
    t = t < 1.0f ? t : 1.0f;
    return strength * (t * t * (3.0f - 2.0f * t));
}

}  // namespace mce::kit
