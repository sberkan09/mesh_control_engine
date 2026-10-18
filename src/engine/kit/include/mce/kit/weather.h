#pragma once
// Weather as data (M-WEATHER step 1): a weather kind, its blend 0..1, the wind and the wetness of the world,
// advanced by one tick-quantized system. kit::install_stock installs it. Scenes without a Weather are unchanged.
// This step writes NO environment fields (step 2 does); it only maintains the component and its event.
//
//   Weather  one per scene (the first in ECS order drives): `target` (clear, overcast, rain, storm, snow; write it
//            at run time to change the weather), `transition_seconds` (default 30), `dry_seconds` (default 120),
//            `auto_change_seconds` (0 = off) and `seed`, and the engine-written `current`, `intensity`, `wind`,
//            `wetness`.
//
// Blending is exact. A transition is `transition_seconds / dt` ticks (rounded, at least 1); `fade` counts ticks
// out of `fade_den` (that count), and `intensity` = fade / fade_den, so after N ticks of a 30 s transition at 60 Hz
// the intensity is N / 1800 with no drift. Changing `transition_seconds` at run time keeps the ratio.
//   * `current` != `target`: the old kind fades out (intensity falls 1 / fade_den per tick), and when it reaches 0
//     `current` becomes `target` (event WeatherChanged) and the new kind fades in, the same tick. A target of
//     clear only fades out. So a change between two non-clear kinds takes a fade out plus a fade in.
//   * `current` == `target` and not clear: intensity rises to 1. Clear has intensity 0.
// `wind` is a function of the state, no extra state: 0.1 + (kind wind - 0.1) * intensity, with the kind wind of
// `current`: clear 0.1, overcast 0.3, rain 0.5, storm 1.0, snow 0.2.
// `wetness` (0..1, kept as the integer `wet_q` out of 2^24): while `current` is rain, storm or snow it rises by
// `intensity` / 3600 per tick at 60 Hz (fully wet after 60 s at full intensity), otherwise it falls by
// 1 / (`dry_seconds` / dt) per tick (dry_seconds <= 0 dries at once).
// Schedule: with `auto_change_seconds` > 0, every that many seconds (as ticks, counted in `timer`) the system
// writes `target` = kind (hash(seed, counter) mod 5) and increments `counter`. The hash is an integer mix; there is
// no RNG state and no wall clock. `timer`, `counter`, `fade`, `fade_den` and `wet_q` are saved, so a save mid-run
// resumes on the same tick and the same inputs give the same state hash.
//
// Event (producer FixedUpdate_Pre): WeatherChanged {from, to} (WeatherKind as u32) once when `current` changes.
//
// System: "kit_weather" (FixedUpdate_Pre, after "kit_timed_light"); install_weather installs timed lights.

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

enum class WeatherKind : u8 { Clear, Overcast, Rain, Storm, Snow };
MCE_ENUM(WeatherKind, Clear, Overcast, Rain, Storm, Snow)

struct Weather {
    WeatherKind target = WeatherKind::Clear;   // the weather to move to; write it to change the weather
    WeatherKind current = WeatherKind::Clear;  // engine-written: the kind in effect
    u8 pad_[2] = {};
    f32 transition_seconds = 30.0f;            // real seconds of one fade in or out
    f32 intensity = 0.0f;                      // engine-written: 0..1 blend of `current` (clear is 0)
    f32 wind = 0.1f;                           // engine-written: 0..1 wind strength
    f32 wetness = 0.0f;                        // engine-written: 0..1
    f32 dry_seconds = 120.0f;                  // real seconds from fully wet to dry
    f32 auto_change_seconds = 0.0f;            // 0 = no random schedule
    u32 seed = 0;
    u32 counter = 0;                           // engine-written: schedule picks so far
    u32 timer = 0;                             // engine-written: ticks since the last pick
    u32 fade = 0;                              // engine-written: intensity numerator
    u32 fade_den = 0;                          // engine-written: intensity denominator (ticks of a transition)
    u32 wet_q = 0;                             // engine-written: wetness numerator out of 2^24
};
MCE_COMPONENT(Weather, (target, Save, Help{"weather to move to: clear, overcast, rain, storm, snow; write it to change the weather"}),
              (current, Save, ReadOnly),
              (transition_seconds, Save, Range{0, 3600}, Help{"real seconds of one fade in or out"}),
              (intensity, Save, ReadOnly), (wind, Save, ReadOnly), (wetness, Save, ReadOnly),
              (dry_seconds, Save, Range{0, 86400}, Help{"real seconds for the world to dry from fully wet"}),
              (auto_change_seconds, Save, Range{0, 86400}, Help{"0: off; else pick a new weather by seed every this many seconds"}),
              (seed, Save, Help{"seed of the random schedule"}),
              (counter, Save, ReadOnly), (timer, Save, ReadOnly), (fade, Save, ReadOnly), (fade_den, Save, ReadOnly),
              (wet_q, Save, ReadOnly))

// Event (producer FixedUpdate_Pre): `current` changed.
struct WeatherChanged {
    u32 from = 0;  // WeatherKind
    u32 to = 0;
};
MCE_EVENT(WeatherChanged, from, to)

// Registers the component and event and adds the system (after install_timed_light, which it calls). Idempotent.
void install_weather(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
