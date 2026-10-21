#pragma once
// The sky's cloud layer as data (M-CLOUDS step 9c). One per scene (the first in ECS order is read). install_weather
// registers it; there is no system, Kit3DExtract reads it.
//
//   CloudLayer  `strength` (1) scales the coverage; `fair_coverage` (0.35) the sky cover in clear weather;
//               `overcast_coverage` (0.9) the cover at full overcast; `scale_m` (900) the size of one noise cell in
//               metres; `speed_scale` (1) scales the drift speed; `height_m` (2000) the height of the layer above
//               the camera.
//
// Kit3DExtract builds render::Clouds only when the scene has a Weather and a CloudLayer: the coverage blends from
// `fair_coverage` toward `overcast_coverage` by the kind factor of Weather.current (Clear 0, Overcast 1, Rain 1,
// Storm 1, Snow 0.9) times Weather.intensity, times `strength`; the drift speed is Weather.wind * `speed_scale`; the
// drift direction is the first WindDirection's heading (+x without one); scale_m and height_m are copied. Then the
// quality tier applies (render::apply_quality: Low and `r.clouds 0` drop the layer). A scene lacking either keeps
// coverage 0, bit-identical to before. The render clock is Scene3D time, nothing is stored here.

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct CloudLayer {
    f32 strength = 1.0f;           // x the derived coverage
    f32 fair_coverage = 0.35f;     // 0..1 cover in clear weather
    f32 overcast_coverage = 0.9f;  // 0..1 cover at full overcast
    f32 scale_m = 900.0f;          // metres per noise cell
    f32 speed_scale = 1.0f;        // x Weather.wind
    f32 height_m = 2000.0f;        // layer height above the camera
};
MCE_COMPONENT(CloudLayer, (strength, Save, Range{0, 1}, Help{"scales the cloud cover; 0 = no clouds"}),
              (fair_coverage, Save, Range{0, 1}, Help{"sky cover in clear weather"}),
              (overcast_coverage, Save, Range{0, 1}, Help{"sky cover at full overcast"}),
              (scale_m, Save, Range{50, 20000}, Help{"size of one cloud noise cell in metres"}),
              (speed_scale, Save, Range{0, 10}, Help{"scales the drift speed (the weather's wind)"}),
              (height_m, Save, Range{100, 20000}, Help{"height of the cloud layer above the camera in metres"}))

}  // namespace mce::kit
