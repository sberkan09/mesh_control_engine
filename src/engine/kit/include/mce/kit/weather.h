#pragma once
// Weather as data (M-WEATHER step 1): a weather kind, its blend 0..1, the wind and the wetness of the world,
// advanced by one tick-quantized system. kit::install_stock installs it. Scenes without a Weather are unchanged.
// Step 2 (below) also modulates the Environment3D resource by the current kind and intensity.
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
// Environment (step 2). After advancing the state, "kit_weather" modulates the Environment3D resource fields
// sun_intensity, sun_color, ambient_sky, ambient_ground, exposure, fog_density and fog_color. Each field goes from
// its base value b to the kind's target t at intensity 1, linearly: env = b + (t - b) * intensity.
//   kind      sun_intensity  sun_color   ambient               exposure  fog_density  fog_color
//             (x)            (to grey)   (x, to grey, tint)    (x)       (add)        (to colour, mix)
//   clear     1              0           1, 0, -               1         +0           -
//   overcast  0.35           0.4         0.85, 0.5, -          1.0       +0.002       (0.60,0.60,0.62) 0.4
//   rain      0.2            0.5         0.7, 0.6, -           1.1       +0.006       (0.50,0.52,0.55) 0.6
//   storm     0.08           0.5         0.5, 0.5, (.85,.9,1.05) 1.2     +0.012       (0.30,0.32,0.36) 0.7
//   snow      0.4            0.3         1.05, 0.3, (.95,1,1.08) 1.0     +0.008       (0.85,0.88,0.92) 0.7
// "To grey" mixes the colour toward its own luminance (0.2126 r + 0.7152 g + 0.0722 b); ambient_sky and
// ambient_ground take the ambient column (mix toward grey, then x, then the tint); fog_color mixes toward the colour.
// No compounding: the base is not read back from the weathered fields. The component keeps `env_applied` (what the
// system wrote last tick) and `env_base`. Each tick a field whose value differs bitwise from `env_applied` was
// rewritten by a DayCycle or an edit, and that value becomes the new base; then env = f(base) and env_applied = env.
// At intensity 0 the base is written back, so a scene at clear is bit-identical to one without a Weather, and
// clearing the weather returns the authored bits. `env_valid` is 0 until the first tick. The 15 slots are:
// sun_intensity, exposure, fog_density, sun_color, ambient_sky, ambient_ground, fog_color (the vec3s as 3 floats).
// Lightning (step 3): after the table, sun_intensity, ambient_sky and ambient_ground are multiplied by `flash_mul`
// (1 = no flash, bit-exact). It is part of the written value (env_applied), so the base logic above is unchanged.
// Wind is not applied to Foliage3D here: it is exposed as Weather.wind for scripts and step 3.
//
// WetSurfaces (wet_surfaces.h, M-WETSURFACE 5c) is a separate component, so Weather's bytes stay as they are.
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
    f32 flash_mul = 1.0f;                      // written by the Lightning system (precipitation.h): x on sun and ambient
    u8 env_valid = 0;                          // engine-written: 1 once env_base holds values
    u8 pad2_[3] = {};
    f32 env_base[15] = {};                     // engine-written: the environment before weather (see above)
    f32 env_applied[15] = {};                  // engine-written: the environment as the system last wrote it
};
MCE_COMPONENT(Weather, (target, Save, Help{"weather to move to: clear, overcast, rain, storm, snow; write it to change the weather"}),
              (current, Save, ReadOnly),
              (transition_seconds, Save, Range{0, 3600}, Help{"real seconds of one fade in or out"}),
              (intensity, Save, ReadOnly), (wind, Save, ReadOnly), (wetness, Save, ReadOnly),
              (dry_seconds, Save, Range{0, 86400}, Help{"real seconds for the world to dry from fully wet"}),
              (auto_change_seconds, Save, Range{0, 86400}, Help{"0: off; else pick a new weather by seed every this many seconds"}),
              (seed, Save, Help{"seed of the random schedule"}),
              (counter, Save, ReadOnly), (timer, Save, ReadOnly), (fade, Save, ReadOnly), (fade_den, Save, ReadOnly),
              (wet_q, Save, ReadOnly), (flash_mul, Save, ReadOnly, Help{"lightning flash multiplier on sun and ambient; 1 = none"}),
              (env_valid, Save, ReadOnly), (env_base, Save, ReadOnly),
              (env_applied, Save, ReadOnly))

// Event (producer FixedUpdate_Pre): `current` changed.
struct WeatherChanged {
    u32 from = 0;  // WeatherKind
    u32 to = 0;
};
MCE_EVENT(WeatherChanged, from, to)

// Registers the component and event and adds the system (after install_timed_light, which it calls). Idempotent.
void install_weather(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
