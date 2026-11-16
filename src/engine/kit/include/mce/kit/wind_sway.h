#pragma once
// Foliage that sways with the weather's wind as data (M-WINDSWAY step 1). One per scene (the first in ECS order is
// read). install_weather registers it. Nothing here runs per tick; Kit3DExtract reads it.
//
//   WindSway  `amplitude_gain` (4) the amplitude multiplier at Weather.wind 1; `frequency_gain` (1.5) the same for the
//             sway frequency; `gust_gain` (0.6) added to the authored gust at wind 1; `flutter_gain` (3) the flutter
//             multiplier at wind 1.
//
// Kit3DExtract scales the batch fields of Foliage3D and TerrainFoliage3D only when the scene has both a Weather and a
// WindSway: authored * (1 + (gain - 1) * Weather.wind) for amplitude, frequency and flutter, and
// clamp(authored_gust + gust_gain * wind, 0, 1) for the gust. The foliage components are never written, and the
// scatter cache is untouched. A scene lacking either extracts exactly as before.

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct WindSway {
    f32 amplitude_gain = 4.0f;   // amplitude multiplier at wind 1
    f32 frequency_gain = 1.5f;   // frequency multiplier at wind 1
    f32 gust_gain = 0.6f;        // added to wind_gust at wind 1
    f32 flutter_gain = 3.0f;     // flutter multiplier at wind 1
    u32 pad_ = 0;                // explicit pad (state_hash hashes whole columns); reserved
};
MCE_COMPONENT(WindSway, (amplitude_gain, Save, Range{0, 50}, Help{"foliage sway amplitude multiplier at Weather.wind 1; 1 = no change"}),
              (frequency_gain, Save, Range{0, 10}, Help{"foliage sway frequency multiplier at Weather.wind 1; 1 = no change"}),
              (gust_gain, Save, Range{-1, 1}, Help{"added to the foliage gust (0..1) at Weather.wind 1"}),
              (flutter_gain, Save, Range{0, 50}, Help{"foliage leaf-flutter multiplier at Weather.wind 1; 1 = no change"}))

}  // namespace mce::kit
