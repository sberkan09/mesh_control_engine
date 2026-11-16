#pragma once
// Rain ripples on puddles as data (M-RAINFX step 7c). One per scene (the first in ECS order is read). install_weather
// registers it; there is no system, Kit3DExtract reads it.
//
//   RainRipples  `strength` (1) scales the ripples; `density` (0.5) how many drops land per puddle area; `speed` (1)
//                the ring animation rate (1 = nominal); `ring_width` (0.15) the ring thickness, 0..1.
//
// Kit3DExtract builds render::Ripples only when the scene has a Weather, a WetSurfaces and a RainRipples and
// Weather.intensity > 0: strength = RainRipples.strength * clamp(Weather.intensity, 0, 1), the other fields copied,
// then the quality tier applies (render::apply_quality: Low and `r.rain_ripples 0` drop them). A scene lacking any of
// the three keeps strength 0, bit-identical to before. The render clock is Scene3D time, nothing is stored here.

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct RainRipples {
    f32 strength = 1.0f;    // x Weather.intensity
    f32 density = 0.5f;     // 0..1 drops per puddle area
    f32 speed = 1.0f;       // ring animation rate
    f32 ring_width = 0.15f;  // 0..1 ring thickness
};
MCE_COMPONENT(RainRipples, (strength, Save, Range{0, 1}, Help{"scales the ripples; 0 = none"}),
              (density, Save, Range{0, 1}, Help{"how many drops land per puddle area"}),
              (speed, Save, Range{0.1f, 10}, Help{"ring animation rate; 1 = nominal"}),
              (ring_width, Save, Range{0, 1}, Help{"ring thickness"}))

}  // namespace mce::kit
