// M-WATER2 49e: the water's current (mce/kit/water_flow.h). Pure functions of the authoring, the position and the time.

#include "mce/kit/water_flow.h"

#include <cmath>

namespace mce::kit {

namespace {

constexpr f32 kMaxFlow = 5.0f;  // m/s, per component and for the speed (render::apply_quality's flow clamp)

}  // namespace

math::vec2 water_flow_velocity(const WaterFlow* flow) {
    if (!flow || !flow->enabled) return math::vec2{0.0f, 0.0f};
    const f32 speed = detail::water_clamp(flow->speed, 0.0f, 0.0f, kMaxFlow);
    if (!(speed > 0.0f)) return math::vec2{0.0f, 0.0f};
    const f32 deg = detail::water_clamp(flow->dir_deg, 0.0f, 0.0f, 360.0f);
    const f64 rad = static_cast<f64>(deg) * (3.14159265358979323846 / 180.0);
    return math::vec2{static_cast<f32>(static_cast<f64>(speed) * std::cos(rad)),
                      static_cast<f32>(static_cast<f64>(speed) * std::sin(rad))};
}

f64 water_flow_cycles(math::vec2 k, math::vec2 velocity) {
    const f64 d =
        static_cast<f64>(k.x) * static_cast<f64>(velocity.x) + static_cast<f64>(k.y) * static_cast<f64>(velocity.y);
    if (!std::isfinite(d)) return 0.0;
    const f64 c = std::round(d * kWaterPeriodSeconds / detail::kWaterTwoPi);
    return c == 0.0 ? 0.0 : c;  // no negative zero
}

WaterWaveSet water_wave_set_flow(const Water& w, f32 wind_dir_deg, math::vec2 velocity) {
    WaterWaveSet s = water_wave_set(w, wind_dir_deg);
    const math::vec2 v{detail::water_clamp(velocity.x, 0.0f, -kMaxFlow, kMaxFlow),
                       detail::water_clamp(velocity.y, 0.0f, -kMaxFlow, kMaxFlow)};
    if (v.x == 0.0f && v.y == 0.0f) return s;
    const f64 base = detail::kWaterTwoPi / kWaterPeriodSeconds;
    for (u32 i = 0; i < s.count; ++i) {
        WaterWave& wave = s.wave[i];
        const f64 cycles = water_flow_cycles(math::vec2{wave.k * wave.dir.x, wave.k * wave.dir.y}, v);
        if (cycles == 0.0) continue;
        // the still omega is f32(mult * base) for a whole mult (water_wave_set); recover it exactly and add the shift
        const f64 mult = std::round(static_cast<f64>(wave.omega) / base);
        wave.omega = static_cast<f32>((mult + cycles) * base);
    }
    return s;
}

f32 water_height_at(const Water& w, const WaterFlow* flow, math::vec2 xz, f64 scene_time, f32 wind_dir_deg) {
    return water_height(water_wave_set_flow(w, wind_dir_deg, water_flow_velocity(flow)), xz,
                        water_wrap_time(scene_time));
}

math::vec3 water_normal_at(const Water& w, const WaterFlow* flow, math::vec2 xz, f64 scene_time, f32 wind_dir_deg) {
    return water_normal(water_wave_set_flow(w, wind_dir_deg, water_flow_velocity(flow)), xz,
                        water_wrap_time(scene_time));
}

f32 water_height_at(const Water& w, const WaterFlow* flow, math::vec2 xz, f64 scene_time) {
    return water_height_at(w, flow, xz, scene_time, w.wind_dir_deg);
}

math::vec3 water_normal_at(const Water& w, const WaterFlow* flow, math::vec2 xz, f64 scene_time) {
    return water_normal_at(w, flow, xz, scene_time, w.wind_dir_deg);
}

}  // namespace mce::kit
