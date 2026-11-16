#pragma once
// Rain splashes where the drops land (M-RAINFX 7f). Optional and per entity: put RainSplash on an entity that also has
// a ParticleEmitter3D and a LocalTransform (the same opt-in shape as Precipitation3D). install_weather registers the
// component; install_precipitation adds the system "kit_rain_splash" (FixedUpdate_Pre, after "kit_precipitation").
//
//   RainSplash  `rate` (1) scales how many splashes land per tick; `height_m` (0.15) how high a splash jumps;
//               `life_seconds` (0.25) how long it lasts; `radius_m` (25) how far from the primary camera they land;
//               `seed` (1) picks the pattern. Engine-written and saved: `counter` (bursts so far).
//
// While Weather.current is Rain or Storm and Weather.intensity > 0 (the first Weather in ECS order, as the other
// weather systems read it) and the scene has a Camera3DComponent (the primary one, else the first), every tick the
// system fires ONE burst of the entity's ParticleEmitter3D:
//   * where: a point of the disc of radius_m around the camera on the x/z plane, from hash(seed, counter);
//     uniform in area. The height is the ground: the entity's own LocalTransform y is left as authored (there is no
//     ground query to ask; put the entity at the floor height). The system writes only x and z.
//   * how many: round(4 * clamp(intensity, 0, 1) * rate) drops (0 fires nothing). At most 256.
//   * look: a short upward splash. Speed gives an apex of height_m under gravity 9.81, so the jump is
//     height_m high and lasts life_seconds; no per-drop physics, the particles are the GPU emitter's.
// A burst is `burst` = count and `burst_version` = counter + 1, which Kit3DExtract turns into a one-shot spawn.
// Otherwise (clear, overcast, snow, no Weather, no camera) the emitter is switched off (emitting 0, rate 0, burst 0)
// and nothing spawns; the fields are written only when their bits change.
//
// Pool: there is no pool of extra entities. The system never creates or destroys entities, so save and load cannot
// lose or duplicate them: the one emitter you author is the pool, and the only state the system keeps is `counter`,
// saved with the component. Every other emitter field is rewritten each tick from the component and the weather.
// A scene without a RainSplash is unchanged.
//
// Clamps (the fields are not rewritten): rate [0, 64] (NaN: 1), height_m [0.01, 5] (NaN: 0.15), life_seconds
// [0.02, 5] (NaN: 0.25), radius_m [0.5, 500] (NaN: 25).

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::kit {

struct RainSplash {
    f32 rate = 1.0f;           // x intensity: splashes per tick scale
    f32 height_m = 0.15f;      // apex of a splash
    f32 life_seconds = 0.25f;  // lifetime of a splash particle
    f32 radius_m = 25.0f;      // from the primary camera
    u32 seed = 1;
    u32 counter = 0;  // engine-written: bursts fired
    u32 pad_ = 0;     // explicit pad (state_hash hashes whole columns); reserved
};
MCE_COMPONENT(RainSplash, (rate, Save, Range{0, 64}, Help{"scales how many splashes land per tick; 0 = none"}),
              (height_m, Save, Range{0.01f, 5}, Help{"how high a splash jumps in metres"}),
              (life_seconds, Save, Range{0.02f, 5}, Help{"how long a splash lasts"}),
              (radius_m, Save, Range{0.5f, 500}, Help{"splashes land within this distance of the primary camera"}),
              (seed, Save, Help{"seed of the splash pattern"}), (counter, Save, ReadOnly))

// Adds the system (after "kit_precipitation": call from install_precipitation). Idempotent per world.
void install_rain_splash(World& world);

}  // namespace mce::kit
