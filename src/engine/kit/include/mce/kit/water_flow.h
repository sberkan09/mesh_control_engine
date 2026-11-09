#pragma once
// A current for the water as data (M-WATER2 step 49e). One optional WaterFlow per scene (the first in ECS order is
// read); it only acts when a Water exists. install_weather registers the component; there is no system and nothing is
// stored between ticks: the drift is a pure function of the scene time, like the waves. Water (water.h) is untouched,
// so a scene without a WaterFlow hashes, saves and draws exactly as before.
//
//   WaterFlow  Authoring (all Save, clamped to the Range when read and never rewritten): `enabled` (1); `dir_deg` (0,
//              0..360) the direction the water moves to (0 = +x, counter-clockwise from above, as Water::wind_dir_deg);
//              `speed` (0, 0..5 m/s). NaN means the default.
//
// Waves on a uniform current U are the still-water waves carried along: the phase of wave i becomes
// K_i . x - (omega_i + K_i . U) t (the Doppler shift). The shift is rounded to whole cycles per kWaterPeriodSeconds
// (water_flow_cycles), so every frequency stays a whole multiple of 2 pi / 64 s and the wrapped clock never pops: the
// drift of wave i is exact to within (pi / 64) / k_i m/s (0.19 m/s for a 24 m swell, 0.01 m/s for a 1.4 m wave). The
// renderer (forward3d.cpp water_params) applies the same rounded term, from render::water_flow_cycles
// (mce/render/water_optics.h, the same f32 and f64 steps), to the drawn waves and its two detail ripples, so gameplay
// and the picture stay on one surface. Implemented in water_flow.cpp; nothing here is hashed.

#include "mce/kit/water.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct WaterFlow {
    f32 dir_deg = 0.0f;
    f32 speed = 0.0f;
    u8 enabled = 1;
    u8 pad_[3] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(WaterFlow, (enabled, Save, Range{0, 1}, Help{"0: the water does not flow"}),
              (dir_deg, Save, Range{0, 360},
               Help{"direction the water moves to in degrees: 0 = +x, counter-clockwise"}),
              (speed, Save, Range{0, 5}, Help{"speed of the current, metres per second"}))

// The current on world (x, z) in m/s: speed * (cos, sin) of dir_deg (in f64, rounded to f32 once per component).
// Exactly zero for null, `enabled` 0, a speed of 0 or NaN; the speed clamps to 0..5 and the direction to 0..360 (NaN
// 0).
math::vec2 water_flow_velocity(const WaterFlow* flow);

// The Doppler shift of a wave with wave vector `k` (rad/m on xz) on a current `velocity` (m/s on xz), in whole cycles
// per kWaterPeriodSeconds: round(dot(k, velocity) * 64 / 2 pi), the dot in f64 of the f32 inputs, round half away from
// zero, no negative zero; NaN or infinite gives 0. Bit-identical to render::water_flow_cycles.
f64 water_flow_cycles(math::vec2 k, math::vec2 velocity);

// water_wave_set(w, wind_dir_deg) with each omega Doppler-shifted by the current: omega + water_flow_cycles(k * dir,
// velocity) * 2 pi / kWaterPeriodSeconds (the sum in f64, rounded to f32 once). The velocity clamps to -5..5 per
// component (NaN 0), as render::apply_quality clamps WaterSurface::flow. A zero velocity, and any wave whose shift
// rounds to 0 cycles, keep the still set's bits exactly.
WaterWaveSet water_wave_set_flow(const Water& w, f32 wind_dir_deg, math::vec2 velocity);

// The flowing gameplay queries: water_height_at / water_normal_at (water.h) over water_wave_set_flow with
// water_flow_velocity(flow). A null `flow` (or a disabled or still one) gives the bits of the water.h overloads.
f32 water_height_at(const Water& w, const WaterFlow* flow, math::vec2 xz, f64 scene_time, f32 wind_dir_deg);
math::vec3 water_normal_at(const Water& w, const WaterFlow* flow, math::vec2 xz, f64 scene_time, f32 wind_dir_deg);
f32 water_height_at(const Water& w, const WaterFlow* flow, math::vec2 xz, f64 scene_time);
math::vec3 water_normal_at(const Water& w, const WaterFlow* flow, math::vec2 xz, f64 scene_time);

}  // namespace mce::kit
