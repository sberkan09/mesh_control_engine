#pragma once
// Water that freezes in winter as data (M-FREEZE step 20a). One optional WaterIce per scene (the first in ECS order is
// read). install_weather registers the component; there is no system and nothing is stored between ticks.
//
//   WaterIce  Authoring (all Save, clamped to the Range when read and never rewritten): `enabled` (1); `freeze_start`
//             (0.5, 0..1) and `freeze_full` (0.9, 0..1), the Season winter weight at which ice starts to form and at
//             which the water is fully frozen (an inverted pair is swapped, an empty range falls back to the defaults);
//             `manual` (0, 0..1) a floor for the amount, so a scene with no Season can still freeze; `ice_color` (0.62
//             0.78 0.85, linear); `ice_roughness` (0.35, 0..1); `ice_reflectivity` (0.55, 0..1) the reflection of fully
//             frozen water as a share of the authored one; `ice_depth_m` (0.4, 0.05..10) the depth scale of fully frozen
//             water; `calm` (1, 0..1) how far the waves are flattened at full freeze (1 = flat); `strength` (1, 0..1)
//             scales every effect, 0 = off exactly.
//
// The amount is max(manual, smoothstep(freeze_start, freeze_full, winter weight)) times strength, a pure function of the
// authoring, the Season and the phase (season_phase of the WorldClock day and hour). The look turns the amount into
// multipliers and mixes the extract (20b) applies to its local render::WaterSurface only. No rand, no pow/exp/trig;
// polynomial smoothstep in a fixed order. `enabled` 0, `strength` 0 and a missing component (a null pointer) give an amount
// of 0 and the identity look as exact values, which the bit-identical render gates rely on.

#include <algorithm>
#include <cmath>

#include "mce/kit/season.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct WaterIce {
    f32 freeze_start = 0.5f;
    f32 freeze_full = 0.9f;
    f32 manual = 0.0f;
    f32 ice_roughness = 0.35f;
    f32 ice_reflectivity = 0.55f;
    f32 ice_depth_m = 0.4f;
    f32 calm = 1.0f;
    f32 strength = 1.0f;
    math::vec3 ice_color{0.62f, 0.78f, 0.85f};
    u8 enabled = 1;
    u8 pad_[3] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(WaterIce, (enabled, Save, Range{0, 1}, Help{"0: the ice is off"}),
              (freeze_start, Save, Range{0, 1}, Help{"winter weight at which ice starts to form"}),
              (freeze_full, Save, Range{0, 1}, Help{"winter weight at which the water is fully frozen"}),
              (manual, Save, Range{0, 1}, Help{"floor for the freeze amount; freezes the water with no Season"}),
              (ice_color, Save, Help{"colour the water is mixed toward when frozen, linear"}),
              (ice_roughness, Save, Range{0, 1}, Help{"roughness of fully frozen water"}),
              (ice_reflectivity, Save, Range{0, 1}, Help{"reflection of fully frozen water as a share of the authored one"}),
              (ice_depth_m, Save, Range{0.05f, 10}, Help{"depth scale of fully frozen water, metres; short so the tint is opaque"}),
              (calm, Save, Range{0, 1}, Help{"how far the waves are flattened at full freeze; 1 is flat"}),
              (strength, Save, Range{0, 1}, Help{"scales every effect; 0 is off exactly"}))

// What a freeze amount does to the water carrier. The identity (amount 0) is {1, 1, 1, 0, 0, 1, depth target, 1}: the
// extract multiplies amplitude, steepness and speed by the scales, mixes both colours toward the ice colour by `ice_mix`,
// takes max(authored roughness, roughness_floor), multiplies reflectivity and foam, and lerps the authored depth scale
// toward `depth_scale_m` by `ice_mix` (so the target is irrelevant at amount 0).
struct WaterIceLook {
    f32 amplitude_scale = 1.0f;
    f32 steepness_scale = 1.0f;
    f32 speed_scale = 1.0f;
    f32 ice_mix = 0.0f;
    f32 roughness_floor = 0.0f;
    f32 reflectivity_scale = 1.0f;
    f32 depth_scale_m = 0.4f;
    f32 foam_scale = 1.0f;
};

namespace detail {

inline f32 water_ice_unit(f32 v, f32 def) { return season_clamp(v, def, 0.0f, 1.0f); }

}  // namespace detail

// The freeze amount 0..1 at a year phase. A null or disabled Season (or one with strength 0) is no winter weight, so the
// amount is the manual floor. Exactly 0 for `enabled` 0 and `strength` 0.
inline f32 water_ice_amount(const WaterIce& ice, const Season* season, f32 phase) {
    if (ice.enabled == 0) return 0.0f;
    const f32 st = detail::water_ice_unit(ice.strength, 1.0f);
    if (!(st > 0.0f)) return 0.0f;
    f32 lo = detail::water_ice_unit(ice.freeze_start, 0.5f);
    f32 hi = detail::water_ice_unit(ice.freeze_full, 0.9f);
    if (lo > hi) std::swap(lo, hi);
    if (!(lo < hi)) {
        lo = 0.5f;
        hi = 0.9f;
    }
    f32 w = 0.0f;
    if (season != nullptr && season->enabled != 0) {
        w = detail::season_winter_weight(detail::season_wrap(phase)) * detail::season_strength(*season);
    }
    const f32 manual = detail::water_ice_unit(ice.manual, 0.0f);
    const f32 a = std::max(manual, detail::season_smooth(lo, hi, w));
    return std::clamp(a * st, 0.0f, 1.0f);
}

// The look at an amount (clamped to 0..1, NaN is 0). `enabled` 0 and `strength` 0 give the identity exactly.
inline WaterIceLook water_ice_look(const WaterIce& ice, f32 amount) {
    WaterIceLook out;
    out.depth_scale_m = detail::season_clamp(ice.ice_depth_m, 0.4f, 0.05f, 10.0f);
    if (ice.enabled == 0 || !(detail::water_ice_unit(ice.strength, 1.0f) > 0.0f)) return out;
    const f32 a = detail::water_ice_unit(amount, 0.0f);
    const f32 calm = detail::water_ice_unit(ice.calm, 1.0f);
    const f32 refl = detail::water_ice_unit(ice.ice_reflectivity, 0.55f);
    const f32 rough = detail::water_ice_unit(ice.ice_roughness, 0.35f);
    const f32 k = 1.0f - a * calm;
    out.amplitude_scale = k;
    out.steepness_scale = k;
    out.speed_scale = k;
    out.ice_mix = a;
    out.roughness_floor = rough * a;
    out.reflectivity_scale = 1.0f - a * (1.0f - refl);
    out.foam_scale = 1.0f - a;
    return out;
}

// The ice colour, each channel clamped to 0..1 (NaN falls back to the default channel).
inline math::vec3 water_ice_color(const WaterIce& ice) {
    return math::vec3{detail::season_clamp(ice.ice_color.x, 0.62f, 0.0f, 1.0f), detail::season_clamp(ice.ice_color.y, 0.78f, 0.0f, 1.0f),
                      detail::season_clamp(ice.ice_color.z, 0.85f, 0.0f, 1.0f)};
}

// The same helpers for a component that may be missing: a null pointer is the identity.
inline f32 water_ice_amount(const WaterIce* ice, const Season* season, f32 phase) {
    return ice ? water_ice_amount(*ice, season, phase) : 0.0f;
}
inline WaterIceLook water_ice_look(const WaterIce* ice, f32 amount) {
    return ice ? water_ice_look(*ice, amount) : WaterIceLook{};
}

}  // namespace mce::kit
