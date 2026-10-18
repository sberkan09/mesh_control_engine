#pragma once
// Wet surfaces as data (M-WETSURFACE step 5c): the look of a wet world, read by Kit3DExtract. One per scene (the first
// in ECS order is read). install_weather registers it. Nothing here runs per tick; it only tunes the look.
//
//   WetSurfaces  `strength` (1) scales Weather.wetness into the scene's wetness; `darken` (0.5) how much a fully wet
//                dielectric loses of its albedo; `wet_roughness` (0.15) the roughness a fully wet surface approaches;
//                `puddles` (0.5) the share of up-facing ground that collects puddles at full wetness; `puddle_scale_m`
//                (4) the size of a puddle in metres.
//
// Kit3DExtract builds render::WetSurfaces only when the scene has both a Weather and a WetSurfaces:
// wetness = Weather.wetness * strength. A scene lacking either stays dry (wetness 0), bit-identical to before.
// The quality tier then applies (render::apply_quality): the Low tier drops the puddles, `r.wet_surfaces 0` drops all.

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct WetSurfaces {
    f32 strength = 1.0f;        // x Weather.wetness
    f32 darken = 0.5f;          // albedo loss of a fully wet dielectric, 0..1
    f32 wet_roughness = 0.15f;  // roughness a fully wet surface approaches
    f32 puddles = 0.5f;         // 0..1 share of up-facing ground with puddles at full wetness
    f32 puddle_scale_m = 4.0f;  // puddle size in metres
    u32 pad_ = 0;               // explicit pad (state_hash hashes whole columns); reserved
};
MCE_COMPONENT(WetSurfaces, (strength, Save, Range{0, 4}, Help{"scales Weather.wetness into the scene's wetness; 0 = dry"}),
              (darken, Save, Range{0, 1}, Help{"albedo a fully wet dielectric loses, 0..1"}),
              (wet_roughness, Save, Range{0, 1}, Help{"roughness a fully wet surface approaches"}),
              (puddles, Save, Range{0, 1}, Help{"share of up-facing ground that puddles at full wetness"}),
              (puddle_scale_m, Save, Range{0.1f, 100}, Help{"puddle size in metres"}))

}  // namespace mce::kit
