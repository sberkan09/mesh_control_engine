#pragma once
// Precipitation and lightning (M-WEATHER step 3): rain and snow driven from the Weather state, and seeded lightning
// in storms. kit::install_stock installs it. Scenes without a Precipitation3D or a Lightning are unchanged.
//
//   Precipitation3D  goes on an entity that has a ParticleEmitter3D and a LocalTransform (opt-in): `rain_rate`
//                    (particles/s at intensity 1, default 3000), `snow_rate` (default 800), `follow_camera` (default 1),
//                    `height` (default 12, metres above the camera), `area` (default 14, half extent of the spawn
//                    box), `rain_speed` (default 18), `snow_speed` (default 1.5).
//   Lightning        one per scene (the first in ECS order drives): `min_gap_seconds` (default 6), `max_gap_seconds`
//                    (default 20), `flash_ticks` (default 8), `flash_boost` (default 4), `seed`, and the engine-written
//                    `counter`, `timer`, `remaining`.
//
// Precipitation. System "kit_precipitation" (FixedUpdate_Pre, after "kit_lightning", which is after "kit_weather")
// reads the first Weather's `current` and `intensity` (I). Every emitter field below is a pure function of the
// Precipitation3D fields and the Weather state, never multiplied in place, so nothing drifts; a field is written only
// when its bits change, so a quiet scene is untouched. The authored values of the fields the table sets are replaced
// while a Precipitation3D is on the entity. With no Weather nothing is written.
//   all kinds   box = (area, 0.5, area); offset, texture, seed, burst*, bounce 0, drag 0, collide 0, additive 0
//   clear, overcast, or I = 0:  emitting = 0, rate = 0 (live particles play out); nothing else is written
//   rain  (storm = rain, with rate x1.6):
//     rate = rain_rate * I (x1.6 storm); emitting 1; wind w = Weather.wind
//     direction = normalize(0.5 w, -1, 0); gravity = (3 w, -0.5 rain_speed, 0)
//     speed_min/max = 0.9 / 1.1 rain_speed; spread 2 degrees; lifetime_min/max = height / rain_speed, x1.2
//     size 0.04 -> 0.04; colour (0.55, 0.62, 0.75) alpha 0.35 -> 0.15; max_particles = ceil(1.6 rain_rate lifetime_max) + 64
//   snow:
//     rate = snow_rate * I; emitting 1; direction = normalize(0.8 w, -1, 0); gravity = (2 w, -0.3 snow_speed, 0)
//     speed_min/max = 0.8 / 1.2 snow_speed; spread 25 degrees; lifetime_min/max = height / snow_speed, x1.25
//     size 0.09 -> 0.07; colour (0.95, 0.97, 1.0) alpha 0.9 -> 0.6; max_particles = ceil(snow_rate lifetime_max) + 64
// With `follow_camera` != 0 the entity's LocalTransform.position is set to the primary Camera3DComponent's position
// plus (0, height, 0) (rotation and scale untouched; the entity should have no parent). Without a camera the
// transform is left alone.
//
// Lightning. System "kit_lightning" (FixedUpdate_Pre, after "kit_weather") runs only while the first Weather's
// `current` is Storm. State machine per tick (ticks, 60 Hz as the engine's fixed step):
//   * idle (timer 0, remaining 0): pick the next gap g = round(seconds / dt) ticks where seconds = min + (max - min) *
//     (hash(seed, 2 counter) / 2^32), at least 1; timer = g.
//   * remaining > 0: --remaining (the flash decays; the next gap starts after the flash ends).
//   * else --timer, and at 0 the strike happens: remaining = flash_ticks, counter += 1 and the event is pushed.
// The hash is an integer mix of (seed, counter), no RNG state. Outside a Storm timer and remaining are cleared.
// Flash: Lightning writes Weather.flash_mul = 1 + (flash_boost - 1) * remaining / flash_ticks, a pure function of
// `remaining` (exactly 1.0f when remaining is 0). kit_weather multiplies sun_intensity, ambient_sky and
// ambient_ground by it when it writes the environment (see weather.h), so the base values are never touched and
// there is no drift; after the flash the environment is bit-identical to a run without lightning. Lightning runs
// after kit_weather, so a flash reaches the environment on the next tick (one tick of latency).
// Event (producer FixedUpdate_Pre): LightningStruck {thunder_delay_ticks} once per strike, in 30..300 (hash of
// (seed, counter) for the strike), for audio to play thunder later.
//
// A ThunderDelay (thunder_delay.h) turns each strike into a delayed one-shot; install_precipitation adds its system.
// A RainSplash (rain_splash.h) drives splash bursts while it rains; install_precipitation adds "kit_rain_splash".
//
// install_precipitation calls install_weather. Saved: all engine-written fields of Lightning.

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

struct Precipitation3D {
    f32 rain_rate = 3000.0f;   // particles per second at intensity 1
    f32 snow_rate = 800.0f;
    u32 follow_camera = 1;     // 1: sit `height` above the primary camera
    f32 height = 12.0f;
    f32 area = 14.0f;          // half extent of the spawn box
    f32 rain_speed = 18.0f;
    f32 snow_speed = 1.5f;
};
MCE_COMPONENT(Precipitation3D, (rain_rate, Range{0, 100000}, Help{"particles/s at weather intensity 1"}),
              (snow_rate, Range{0, 100000}), (follow_camera, Help{"1: move to height metres above the primary camera"}),
              (height, Range{0.5f, 200}), (area, Range{0.5f, 500}, Help{"half extent of the spawn box"}),
              (rain_speed, Range{0.1f, 200}), (snow_speed, Range{0.05f, 50}))

struct Lightning {
    f32 min_gap_seconds = 6.0f;
    f32 max_gap_seconds = 20.0f;
    u32 flash_ticks = 8;
    f32 flash_boost = 4.0f;    // sun and ambient multiplier at the flash peak
    u32 seed = 0;
    u32 counter = 0;           // engine-written: strikes so far
    u32 timer = 0;             // engine-written: ticks to the next strike (0 = none scheduled)
    u32 remaining = 0;         // engine-written: flash ticks left
};
MCE_COMPONENT(Lightning, (min_gap_seconds, Save, Range{0.1f, 3600}, Help{"shortest time between strikes in a storm"}),
              (max_gap_seconds, Save, Range{0.1f, 3600}), (flash_ticks, Save, Range{0, 600}, Help{"ticks a flash lasts"}),
              (flash_boost, Save, Range{1, 50}, Help{"sun and ambient multiplier at the peak of a flash"}),
              (seed, Save), (counter, Save, ReadOnly), (timer, Save, ReadOnly), (remaining, Save, ReadOnly))

// Event (producer FixedUpdate_Pre): a strike; thunder should sound `thunder_delay_ticks` (30..300) later.
struct LightningStruck {
    u32 thunder_delay_ticks = 0;
};
MCE_EVENT(LightningStruck, thunder_delay_ticks)

// Registers the components and event and adds the systems (after install_weather, which it calls). Idempotent.
void install_precipitation(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
