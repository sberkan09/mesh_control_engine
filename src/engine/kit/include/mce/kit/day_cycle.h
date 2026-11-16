#pragma once
// A day cycle as data (M-DAYNIGHT step 2): keyed curves over the hour of the day for the sun, the ambient light,
// the fog and the exposure, written to the game's Environment3D resource each tick. kit::install_stock installs it.
// Scenes without a DayCycle are unchanged.
//
//   DayCycle  one per scene: `enabled` (default 1), `key_count` (2..8, default 5) and 8 key slots. Slot N
//             (kN_*) is one keyframe: its hour 0..24 and the value of every curve at that hour: sun colour
//             (`sun`), `sun_i` (sun intensity), ambient `sky` and `ground` colours, `fog` colour, `fog_d`
//             (fog density) and `exp` (exposure). Reflected components have no arrays, so the slots are plain
//             fields; only the first `key_count` are used and they may be in any order (the system sorts by hour).
//
// The default is a preset with five keys: night (0:00), dawn (6:00), noon (12:00), dusk (18:00) and night
// (21:00), so adding an empty DayCycle already looks right. Between two keys the values are interpolated
// linearly; before the first key and after the last the curve runs from the last key (24 h earlier) to the
// first, so it wraps at midnight. At a key's own hour the result is exactly the key's values.
//
// The system reads Environment3D.time_of_day (hours, written by a WorldClock; the system does nothing while it
// is below 0, so without a clock the environment stays as authored) and writes sun_color, sun_intensity,
// ambient_sky, ambient_ground, fog_color, fog_density and exposure of the Environment3D resource. With
// `enabled` 0 it writes nothing. Installing the day cycle installs the world clock.
//
// System: "kit_day_cycle" (FixedUpdate_Pre, after "kit_world_clock").

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

constexpr u32 kDayCycleKeys = 8;

struct DayCycle {
    u8 enabled = 1;
    u8 pad_[3] = {};
    u32 key_count = 5;  // the first N slots are used
    f32 k0_hour = 0.0f;
    math::Color k0_sun = {0.35f, 0.45f, 0.9f, 1.0f};
    f32 k0_sun_i = 0.05f;
    math::Color k0_sky = {0.02f, 0.03f, 0.07f, 1.0f};
    math::Color k0_ground = {0.01f, 0.01f, 0.02f, 1.0f};
    math::Color k0_fog = {0.02f, 0.03f, 0.06f, 1.0f};
    f32 k0_fog_d = 0.004f;
    f32 k0_exp = 1.2f;
    f32 k1_hour = 6.0f;
    math::Color k1_sun = {1.0f, 0.6f, 0.35f, 1.0f};
    f32 k1_sun_i = 1.0f;
    math::Color k1_sky = {0.35f, 0.38f, 0.5f, 1.0f};
    math::Color k1_ground = {0.15f, 0.12f, 0.1f, 1.0f};
    math::Color k1_fog = {0.75f, 0.6f, 0.5f, 1.0f};
    f32 k1_fog_d = 0.006f;
    f32 k1_exp = 1.0f;
    f32 k2_hour = 12.0f;
    math::Color k2_sun = {1.0f, 0.96f, 0.9f, 1.0f};
    f32 k2_sun_i = 2.0f;
    math::Color k2_sky = {0.35f, 0.42f, 0.55f, 1.0f};
    math::Color k2_ground = {0.2f, 0.18f, 0.16f, 1.0f};
    math::Color k2_fog = {0.62f, 0.72f, 0.85f, 1.0f};
    f32 k2_fog_d = 0.002f;
    f32 k2_exp = 1.0f;
    f32 k3_hour = 18.0f;
    math::Color k3_sun = {1.0f, 0.5f, 0.25f, 1.0f};
    f32 k3_sun_i = 1.0f;
    math::Color k3_sky = {0.4f, 0.3f, 0.4f, 1.0f};
    math::Color k3_ground = {0.15f, 0.1f, 0.1f, 1.0f};
    math::Color k3_fog = {0.8f, 0.5f, 0.4f, 1.0f};
    f32 k3_fog_d = 0.006f;
    f32 k3_exp = 1.0f;
    f32 k4_hour = 21.0f;
    math::Color k4_sun = {0.35f, 0.45f, 0.9f, 1.0f};
    f32 k4_sun_i = 0.05f;
    math::Color k4_sky = {0.02f, 0.03f, 0.07f, 1.0f};
    math::Color k4_ground = {0.01f, 0.01f, 0.02f, 1.0f};
    math::Color k4_fog = {0.02f, 0.03f, 0.06f, 1.0f};
    f32 k4_fog_d = 0.004f;
    f32 k4_exp = 1.2f;
    f32 k5_hour = 24.0f;
    math::Color k5_sun = {1.0f, 1.0f, 1.0f, 1.0f};
    f32 k5_sun_i = 2.0f;
    math::Color k5_sky = {0.35f, 0.42f, 0.55f, 1.0f};
    math::Color k5_ground = {0.2f, 0.18f, 0.16f, 1.0f};
    math::Color k5_fog = {0.62f, 0.72f, 0.85f, 1.0f};
    f32 k5_fog_d = 0.0f;
    f32 k5_exp = 1.0f;
    f32 k6_hour = 24.0f;
    math::Color k6_sun = {1.0f, 1.0f, 1.0f, 1.0f};
    f32 k6_sun_i = 2.0f;
    math::Color k6_sky = {0.35f, 0.42f, 0.55f, 1.0f};
    math::Color k6_ground = {0.2f, 0.18f, 0.16f, 1.0f};
    math::Color k6_fog = {0.62f, 0.72f, 0.85f, 1.0f};
    f32 k6_fog_d = 0.0f;
    f32 k6_exp = 1.0f;
    f32 k7_hour = 24.0f;
    math::Color k7_sun = {1.0f, 1.0f, 1.0f, 1.0f};
    f32 k7_sun_i = 2.0f;
    math::Color k7_sky = {0.35f, 0.42f, 0.55f, 1.0f};
    math::Color k7_ground = {0.2f, 0.18f, 0.16f, 1.0f};
    math::Color k7_fog = {0.62f, 0.72f, 0.85f, 1.0f};
    f32 k7_fog_d = 0.0f;
    f32 k7_exp = 1.0f;
};
MCE_COMPONENT(DayCycle, (enabled, Save, Help{"1: write the environment from the hour; 0: leave it alone"}),
              (key_count, Save, Range{1, 8}, Help{"how many key slots are used (the first N)"}),
              (k0_hour, Save, Range{0, 24}, Help{"key 0: key hour 0..24 (keys are sorted by hour when read)"}),
              (k0_sun, Save, Help{"key 0: sun colour (linear)"}),
              (k0_sun_i, Save, Range{0, 100}, Help{"key 0: sun intensity"}),
              (k0_sky, Save, Help{"key 0: ambient sky colour"}),
              (k0_ground, Save, Help{"key 0: ambient ground colour"}),
              (k0_fog, Save, Help{"key 0: fog colour"}),
              (k0_fog_d, Save, Range{0, 1}, Help{"key 0: fog density (exp2)"}),
              (k0_exp, Save, Range{0.01f, 20}, Help{"key 0: camera exposure"}),
              (k1_hour, Save, Range{0, 24}, Help{"key 1: key hour 0..24 (keys are sorted by hour when read)"}),
              (k1_sun, Save, Help{"key 1: sun colour (linear)"}),
              (k1_sun_i, Save, Range{0, 100}, Help{"key 1: sun intensity"}),
              (k1_sky, Save, Help{"key 1: ambient sky colour"}),
              (k1_ground, Save, Help{"key 1: ambient ground colour"}),
              (k1_fog, Save, Help{"key 1: fog colour"}),
              (k1_fog_d, Save, Range{0, 1}, Help{"key 1: fog density (exp2)"}),
              (k1_exp, Save, Range{0.01f, 20}, Help{"key 1: camera exposure"}),
              (k2_hour, Save, Range{0, 24}, Help{"key 2: key hour 0..24 (keys are sorted by hour when read)"}),
              (k2_sun, Save, Help{"key 2: sun colour (linear)"}),
              (k2_sun_i, Save, Range{0, 100}, Help{"key 2: sun intensity"}),
              (k2_sky, Save, Help{"key 2: ambient sky colour"}),
              (k2_ground, Save, Help{"key 2: ambient ground colour"}),
              (k2_fog, Save, Help{"key 2: fog colour"}),
              (k2_fog_d, Save, Range{0, 1}, Help{"key 2: fog density (exp2)"}),
              (k2_exp, Save, Range{0.01f, 20}, Help{"key 2: camera exposure"}),
              (k3_hour, Save, Range{0, 24}, Help{"key 3: key hour 0..24 (keys are sorted by hour when read)"}),
              (k3_sun, Save, Help{"key 3: sun colour (linear)"}),
              (k3_sun_i, Save, Range{0, 100}, Help{"key 3: sun intensity"}),
              (k3_sky, Save, Help{"key 3: ambient sky colour"}),
              (k3_ground, Save, Help{"key 3: ambient ground colour"}),
              (k3_fog, Save, Help{"key 3: fog colour"}),
              (k3_fog_d, Save, Range{0, 1}, Help{"key 3: fog density (exp2)"}),
              (k3_exp, Save, Range{0.01f, 20}, Help{"key 3: camera exposure"}),
              (k4_hour, Save, Range{0, 24}, Help{"key 4: key hour 0..24 (keys are sorted by hour when read)"}),
              (k4_sun, Save, Help{"key 4: sun colour (linear)"}),
              (k4_sun_i, Save, Range{0, 100}, Help{"key 4: sun intensity"}),
              (k4_sky, Save, Help{"key 4: ambient sky colour"}),
              (k4_ground, Save, Help{"key 4: ambient ground colour"}),
              (k4_fog, Save, Help{"key 4: fog colour"}),
              (k4_fog_d, Save, Range{0, 1}, Help{"key 4: fog density (exp2)"}),
              (k4_exp, Save, Range{0.01f, 20}, Help{"key 4: camera exposure"}),
              (k5_hour, Save, Range{0, 24}, Help{"key 5: key hour 0..24 (keys are sorted by hour when read)"}),
              (k5_sun, Save, Help{"key 5: sun colour (linear)"}),
              (k5_sun_i, Save, Range{0, 100}, Help{"key 5: sun intensity"}),
              (k5_sky, Save, Help{"key 5: ambient sky colour"}),
              (k5_ground, Save, Help{"key 5: ambient ground colour"}),
              (k5_fog, Save, Help{"key 5: fog colour"}),
              (k5_fog_d, Save, Range{0, 1}, Help{"key 5: fog density (exp2)"}),
              (k5_exp, Save, Range{0.01f, 20}, Help{"key 5: camera exposure"}),
              (k6_hour, Save, Range{0, 24}, Help{"key 6: key hour 0..24 (keys are sorted by hour when read)"}),
              (k6_sun, Save, Help{"key 6: sun colour (linear)"}),
              (k6_sun_i, Save, Range{0, 100}, Help{"key 6: sun intensity"}),
              (k6_sky, Save, Help{"key 6: ambient sky colour"}),
              (k6_ground, Save, Help{"key 6: ambient ground colour"}),
              (k6_fog, Save, Help{"key 6: fog colour"}),
              (k6_fog_d, Save, Range{0, 1}, Help{"key 6: fog density (exp2)"}),
              (k6_exp, Save, Range{0.01f, 20}, Help{"key 6: camera exposure"}),
              (k7_hour, Save, Range{0, 24}, Help{"key 7: key hour 0..24 (keys are sorted by hour when read)"}),
              (k7_sun, Save, Help{"key 7: sun colour (linear)"}),
              (k7_sun_i, Save, Range{0, 100}, Help{"key 7: sun intensity"}),
              (k7_sky, Save, Help{"key 7: ambient sky colour"}),
              (k7_ground, Save, Help{"key 7: ambient ground colour"}),
              (k7_fog, Save, Help{"key 7: fog colour"}),
              (k7_fog_d, Save, Range{0, 1}, Help{"key 7: fog density (exp2)"}),
              (k7_exp, Save, Range{0.01f, 20}, Help{"key 7: camera exposure"}))

// Registers the component and adds the system (after install_world_clock, which it calls). Idempotent.
void install_day_cycle(World& world, asset::AssetSystem* assets);

// The curve values at `hour` (0..24, wraps) for `c`; what the system writes. Exposed for tests and tools.
struct DayCycleValues {
    math::Color sun, sky, ground, fog;
    f32 sun_intensity = 0.0f, fog_density = 0.0f, exposure = 1.0f;
};
DayCycleValues evaluate_day_cycle(const DayCycle& c, f32 hour);

}  // namespace mce::kit
