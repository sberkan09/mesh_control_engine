#include "mce/render/lens_rain.h"

#include <cmath>

#include "mce/render/scene3d.h"
#include "unit_clamp.h"

namespace mce::render {

namespace {

constexpr f32 kWetRate = 1.0f / 4.0f;    // per second while raining
constexpr f32 kDryRate = 1.0f / 40.0f;   // per second otherwise

// 0..1, NaN -> 0.
f32 unit(f32 x) { return detail::ranged(x, 0.0f, 1.0f, 0.0f); }

u32 mix(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

}  // namespace

f32 lens_rain_wetness_step(f32 wetness, bool raining, f32 intensity, f32 dt) {
    const f32 w = unit(wetness);
    if (!(dt > 0.0f) || !std::isfinite(dt)) return w;
    if (raining) {
        const f32 target = unit(intensity);
        if (w < target) {
            const f32 up = w + kWetRate * dt;
            return up < target ? up : target;
        }
        const f32 down = w - kDryRate * dt;  // above the intensity: dries toward it
        return down > target ? down : target;
    }
    const f32 down = w - kDryRate * dt;
    return down > 0.0f ? down : 0.0f;
}

LensRainFrame lens_rain_frame(const LensRain& rain, f32 time_s, f32 aspect) {
    LensRainFrame f;
    f.amount = rain.enabled ? detail::ranged(rain.amount, 0.0f, 1.0f, 0.0f) : 0.0f;
    f.density = detail::ranged(rain.density, 0.0f, 4.0f, 1.0f);
    f.size = detail::ranged(rain.size, 0.25f, 4.0f, 1.0f);
    f.refraction = detail::ranged(rain.refraction, 0.0f, 2.0f, 1.0f);
    f.blur = detail::ranged(rain.blur, 0.0f, 1.0f, 0.35f);
    f.trail = detail::ranged(rain.trail, 0.0f, 1.0f, 0.5f);
    f.fall_speed = detail::ranged(rain.fall_speed, 0.0f, 2.0f, 0.25f);
    f.time = std::isfinite(time_s) ? time_s : 0.0f;
    f.aspect = (std::isfinite(aspect) && aspect > 0.0f) ? aspect : 1.0f;
    f.seed = rain.seed;
    return f;
}

u32 lens_rain_hash(u32 cell_x, u32 cell_y, u32 seed) {
    u32 h = mix(cell_x);
    h = mix(h ^ (cell_y * 0x9e3779b9u));
    h = mix(h ^ seed);
    return h;
}

}  // namespace mce::render
