#pragma once
// Timed lights and emissives (M-DAYNIGHT step 3): a lamp that fades on at dusk and off at dawn, following the
// WorldClock. kit::install_stock installs it. Scenes without a TimedLight3D are unchanged.
//
//   TimedLight3D  goes on an entity that has a PointLight3D and/or a SpotLight3D and/or a MeshRef (any mix):
//                 `on_hour` (default 18.5), `off_hour` (default 6), `fade_minutes` (default 20), `emissive_on` /
//                 `emissive_off` (what MeshRef.emissive_tint is multiplied to when fully on / off, default 1 / 0)
//                 and the engine-written `light_scale` / `spot_scale`: the authored intensity of the point and
//                 the spot light, captured at the first tick (saved, so a save mid-night keeps it).
//
// The ramp (0..1) is a function of the clock hour: it rises linearly from 0 at `on_hour` to 1 at
// `on_hour + fade`, stays 1 until `off_hour`, then falls linearly to 0 at `off_hour + fade`. The window wraps
// midnight (18.5 -> 6). `on_hour == off_hour` means always on. If the window is shorter than the fade the fall
// starts from the ramp reached at `off_hour`. Each tick the system sets, as a pure function of the ramp (so it
// never drifts): light intensity = base * ramp, `enabled` = 0 while the ramp is 0 (the light costs nothing)
// and 1 otherwise, MeshRef.emissive_tint = emissive_off + (emissive_on - emissive_off) * ramp on all three
// channels. The colour of the lights and the rest of the MeshRef are not touched.
//
// The hour is the first WorldClock's `hour` (ECS order); without a clock nothing is written (the base intensity
// is still captured).
//
// Events (producer FixedUpdate_Pre), payload `hour` (the hour that was crossed, f32): DuskStarted when the clock
// passes `on_hour`, DawnStarted when it passes `off_hour`. They come from the FIRST TimedLight3D in ECS order
// only (its hours stand for the scene), which remembers the clock hour of the last tick in `last_hour`
// (engine-written, -1 = none yet). A crossing is last_hour < h <= hour, wrapping midnight; a step of more than
// 6 hours (the clock was set, not run) fires nothing and neither does the first tick. One event per crossing.
//
// System: "kit_timed_light" (FixedUpdate_Pre, after "kit_day_cycle"); install_timed_light installs the day cycle.

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

struct TimedLight3D {
    f32 on_hour = 18.5f;
    f32 off_hour = 6.0f;
    f32 fade_minutes = 20.0f;
    f32 emissive_on = 1.0f;
    f32 emissive_off = 0.0f;
    f32 light_scale = 0.0f;   // engine-written: the point light's authored intensity
    f32 spot_scale = 0.0f;    // engine-written: the spot light's authored intensity
    f32 last_hour = -1.0f;    // engine-written: the clock hour of the last tick (events), -1 = none
    u8 captured = 0;          // engine-written: the base intensities were captured
    u8 pad_[3] = {};
};
MCE_COMPONENT(TimedLight3D, (on_hour, Save, Range{0, 24}, Help{"hour the light starts fading on"}),
              (off_hour, Save, Range{0, 24}, Help{"hour the light starts fading off; on_hour == off_hour: always on"}),
              (fade_minutes, Save, Range{0, 600}, Help{"game minutes of the fade, after on_hour and after off_hour"}),
              (emissive_on, Save, Range{0, 100}, Help{"MeshRef.emissive_tint multiplier when fully on"}),
              (emissive_off, Save, Range{0, 100}, Help{"MeshRef.emissive_tint multiplier when off"}),
              (light_scale, Save, ReadOnly), (spot_scale, Save, ReadOnly), (last_hour, Save, ReadOnly),
              (captured, Save, ReadOnly))

// Events (producer FixedUpdate_Pre).
struct DuskStarted {
    f32 hour = 0.0f;  // the on_hour that was crossed
};
struct DawnStarted {
    f32 hour = 0.0f;  // the off_hour that was crossed
};
MCE_EVENT(DuskStarted, hour)
MCE_EVENT(DawnStarted, hour)

// Registers the component and events and adds the system (after install_day_cycle, which it calls). Idempotent.
void install_timed_light(World& world, asset::AssetSystem* assets);

// The ramp 0..1 of `t` at clock `hour` (wraps); what the system applies. Exposed for tests and tools.
f32 timed_light_ramp(const TimedLight3D& t, f32 hour);

}  // namespace mce::kit
