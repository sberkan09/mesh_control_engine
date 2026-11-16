#pragma once
// A world clock as data (M-DAYNIGHT step 1): the hour of the day, advanced by one tick-quantized system and
// written to Environment3D.time_of_day so the sun moves without a script. kit::install_stock installs it.
// Scenes without a WorldClock are unchanged.
//
//   WorldClock  one per scene: `hour` (0..24, the start value), `day_length_seconds` (real seconds for 24 h,
//               default 1200), `time_scale` (1 = real time, 0 and negatives stop it), `paused`,
//               `drive_environment` (default 1) and the read-only `day` counter
//
// Time is exact. The day is split into `day_length_seconds / dt` ticks (rounded) of 1024 sub-steps each; the
// clock adds `round(time_scale * 1024)` sub-steps per tick, so the hour after N ticks is an integer ratio and
// never drifts. The sub-step counter (`phase`) and its denominator (`phase_den`) are saved with the scene, so a
// save mid-day resumes on the same tick and the same run gives the same state hash. Changing
// `day_length_seconds` or `time_scale` at run time keeps the time of day (the counter is rescaled).
// Writing `hour` at run time (a script, the editor) sets the time of day from then on.
//
// The first tick takes `hour` as the start, then advances. `hour` is then written each tick with the current
// hour. When `drive_environment` is set the system writes the game's Environment3D resource (the one the
// 3D extract reads; install_kit3d copies the authored component into it at Startup) `time_of_day` every tick.
// With 0 the system leaves the environment alone.
//
// Events (producer FixedUpdate_Pre), pushed in time order when a tick crosses a boundary: HourPassed
// (`hour` 0..23, the hour that began; 0 is midnight) and DayPassed (`day`, the new count, at midnight).
//
// System: "kit_world_clock" (FixedUpdate_Pre, after "kit3d_health").

#include "mce/fixed_string.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"
#include "mce/string_id.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

struct WorldClock {
    f32 hour = 8.0f;                  // 0..24: the start value; then the current hour (engine-written)
    f32 day_length_seconds = 1200.0f; // real seconds for 24 hours
    f32 time_scale = 1.0f;            // 1 = as set by the day length; 0 (and below) stops it
    u8 paused = 0;
    u8 drive_environment = 1;         // 1: write Environment3D.time_of_day every tick
    u8 started = 0;                   // engine-written: the first tick has taken `hour`
    u8 pad_ = 0;
    u32 day = 0;                      // engine-written: whole days passed since the start
    u32 pad2_ = 0;
    u64 phase = 0;                    // engine-written: sub-steps into the day, out of phase_den
    u64 phase_den = 0;                // engine-written: sub-steps in a day
    f32 hour_seen = 0.0f;             // engine-written: the `hour` last written, to notice an edit
    u32 pad3_ = 0;
};
MCE_COMPONENT(WorldClock, (hour, Save, Range{0, 24}, Help{"hour of the day 0..24: the start value, then the current hour; write it to set the time"}),
              (day_length_seconds, Save, Range{1, 86400}, Help{"real seconds for 24 hours"}),
              (time_scale, Save, Range{0, 100}, Help{"clock speed multiplier; 0 stops it"}),
              (paused, Save, Help{"1: the clock stands still"}),
              (drive_environment, Save, Help{"1: write Environment3D.time_of_day every tick"}),
              (started, Save, ReadOnly), (day, Save, ReadOnly), (phase, Save, ReadOnly), (phase_den, Save, ReadOnly),
              (hour_seen, Save, ReadOnly))

// Events (producer FixedUpdate_Pre).
struct HourPassed {
    u32 hour = 0;                     // the hour that began, 0..23
};
struct DayPassed {
    u32 day = 0;                      // the new day count
};
MCE_EVENT(HourPassed, hour)
MCE_EVENT(DayPassed, day)

// Registers the component and events and adds the system (after install_combat3d, which it calls). Idempotent.
void install_world_clock(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
