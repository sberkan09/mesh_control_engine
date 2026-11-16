#pragma once
// Dynamic resolution as data (M-DYNRES step 46d). One optional DynamicResolution3D per scene: Kit3DExtract reads the
// first enabled one by entity index and fills Scene3D post.dynres through render::apply_quality (the render side is
// 46a to 46c: the controller in Forward3D moves the render scale of the window views to keep the 3D frame's GPU time
// under `target_ms`); install_kit3d registers the component next to LensFlare3D. Opt-in: without one the frame is
// drawn at the tier's fixed render scale, bit for bit as before. The scale is render-side only: there is no system,
// nothing is stored between ticks, nothing is written back, and the component never feeds the simulation or the state
// hash. The controller reads the PostFx of the frame's first 3D view, so a scene puts the component next to its
// primary camera's world like the other post components. kit3d.h is not touched.
//
//   DynamicResolution3D  Authoring (all Save, clamped by apply_quality to the Range, NaN to the default): `enabled` (1);
//                        `target_ms` (14, 4..100) the GPU budget of the 3D frame in milliseconds (`r.dynres.target_ms`
//                        overrides it per player); `min_scale` (0.6, 0.5..1) the floor of the render scale (the tier's
//                        floor is the lower bound: High never goes under 0.65); `max_scale` (1, min_scale..1) an extra
//                        ceiling below the tier's render_scale; `headroom` (0.15, 0.05..0.5) the scale rises only while
//                        the estimate at the next step stays under target_ms * (1 - headroom).
//
// `r.dynres 0` or a tier without dynamic resolution turns it off (apply_quality); `r.dynres.inject` replaces the
// measured GPU time with a fixed schedule (a test and sample hook).

#include "mce/refl/macros.h"
#include "mce/types.h"

namespace mce::kit {

struct DynamicResolution3D {
    f32 target_ms = 14.0f;
    f32 min_scale = 0.6f;
    f32 max_scale = 1.0f;
    f32 headroom = 0.15f;
    u8 enabled = 1;
    u8 pad_[3] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(DynamicResolution3D, (enabled, Save, Range{0, 1}, Help{"0: the fixed render scale of the quality tier"}),
              (target_ms, Save, Range{4, 100}, Help{"GPU budget of the 3D frame in milliseconds"}),
              (min_scale, Save, Range{0.5f, 1}, Help{"lowest render scale the controller may pick"}),
              (max_scale, Save, Range{0.5f, 1}, Help{"highest render scale, under the quality tier's own"}),
              (headroom, Save, Range{0.05f, 0.5f},
               Help{"the scale rises only while the next step's estimate stays this fraction under the budget"}))

}  // namespace mce::kit
