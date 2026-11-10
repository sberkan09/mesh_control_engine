#pragma once
// Cloud shadows on the ground as data (M-CLOUDSHADOW step 51d). One optional CloudShadow3D per scene: Kit3DExtract reads
// the first enabled one by entity index and fills Scene3D post.cloud_shadow (the render side is 51a to 51c); install_kit3d
// registers the component next to SunShafts3D. The clouds, the wind, the clock and the sun come from CloudLayer, the
// weather and Environment3D, so the patches follow them with no authoring; a scene without a CloudLayer (coverage 0, or
// the sky layer off) gets no shadow, because the pass requires cloud coverage above 0 and the sky. There is no system and
// nothing is stored between ticks: the component never feeds the simulation or the state hash. kit3d.h is not touched.
//
//   CloudShadow3D  Authoring (all Save, clamped by the renderer to the Range): `enabled` (1); `strength` (0.6, 0..1)
//                  how much of the sun a thick cloud removes; `softness` (0.3, 0..1) how far the shadow's edge is wider
//                  than the cloud's own edge.

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct CloudShadow3D {
    f32 strength = 0.6f;
    f32 softness = 0.3f;
    u8 enabled = 1;
    u8 pad_[3] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(CloudShadow3D, (enabled, Save, Range{0, 1}, Help{"0: no cloud shadows"}),
              (strength, Save, Range{0, 1}, Help{"how much of the sun a thick cloud removes"}),
              (softness, Save, Range{0, 1}, Help{"how much wider the shadow's edge is than the cloud's own"}))

}  // namespace mce::kit
