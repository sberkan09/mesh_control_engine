#pragma once
// Sun light shafts as data (M-SUNSHAFTS step 45d). One optional SunShafts3D per scene: Kit3DExtract reads the first
// enabled one by entity index and fills Scene3D post.shafts (the render side is 45a to 45c); install_kit3d registers
// the component next to LensFlare3D. The sun and the fog are not here: they come from Environment3D (and a DayCycle)
// through Scene3D::light() and Scene3D::fog(), so the shafts follow a moving sun with no authoring. There is no system
// and nothing is stored between ticks: the component never feeds the simulation or the state hash. kit3d.h is not
// touched.
//
//   SunShafts3D  Authoring (all Save, clamped by the renderer to the Range): `enabled` (1); `intensity` (1, 0..4)
//                the gain on the sun colour; `length` (0.85, 0.1..1) how far toward the sun each pixel's walk goes;
//                `decay` (0.97, 0.8..1) the per-sample weight falloff; `threshold` (1, 0.05..16) the HDR luminance
//                above which a sky sample is a source (the default lets the sun disk through and keeps the sky under
//                it: measured, M-SUNSHAFTS 45c); `cap` (1.5, 0..8) the most the pass adds per channel (a soft clamp);
//                `cloud_follow` (0.5, 0..1) how far the cloud cover dims the shafts on top of the dimming the sky pass
//                already gives the sun disk; `scatter_m` (60, 5..500 m) the view distance over which a geometry pixel
//                gains its full veil.
//
// sun_shafts_visibility(s, cloud_coverage) = clamp(1 - cloud_follow * cloud_coverage, 0, 1); the cover clamps to 0..1
// and NaN counts as 0, a NaN cloud_follow as its default 0.5. Exactly 1.0f for cloud_follow 0 or a clear sky. Rain
// comes in through the cover the weather already raises (no separate rain term).

#include <algorithm>
#include <cmath>

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct SunShafts3D {
    f32 intensity = 1.0f;
    f32 length = 0.85f;
    f32 decay = 0.97f;
    f32 threshold = 1.0f;
    f32 cap = 1.5f;
    f32 cloud_follow = 0.5f;
    f32 scatter_m = 60.0f;
    u8 enabled = 1;
    u8 pad_[3] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(SunShafts3D, (enabled, Save, Range{0, 1}, Help{"0: no sun shafts"}),
              (intensity, Save, Range{0, 4}, Help{"gain on the sun colour"}),
              (length, Save, Range{0.1f, 1},
               Help{"how far toward the sun the walk goes, fraction of the pixel-to-sun distance"}),
              (decay, Save, Range{0.8f, 1}, Help{"per-sample weight falloff: the nearer part of the walk counts more"}),
              (threshold, Save, Range{0.05f, 16}, Help{"HDR luminance above which a sky sample is a source"}),
              (cap, Save, Range{0, 8}, Help{"the most the shafts add per channel (a soft clamp)"}),
              (cloud_follow, Save, Range{0, 1}, Help{"how far cloud cover dims the shafts"}),
              (scatter_m, Save, Range{5, 500}, Help{"view distance over which geometry gains its full veil (metres)"}))

// The caller's dimming for PostFx::shafts.visibility, in [0, 1].
inline f32 sun_shafts_visibility(const SunShafts3D& s, f32 cloud_coverage) {
    const f32 follow = std::isnan(s.cloud_follow) ? 0.5f : std::clamp(s.cloud_follow, 0.0f, 1.0f);
    const f32 cover = std::isnan(cloud_coverage) ? 0.0f : std::clamp(cloud_coverage, 0.0f, 1.0f);
    return std::clamp(1.0f - follow * cover, 0.0f, 1.0f);
}

}  // namespace mce::kit
