#pragma once
// Caustics under water as data (M-CAUSTICS step 52c). One optional Caustics3D per scene: Kit3DExtract reads the first
// enabled one by entity index and fills Scene3D post.caustics (the render side is 52a and 52b); install_kit3d registers
// the component next to CloudShadow3D. The water plane, the sun and the clock come from the scene's Water,
// Environment3D (and a DayCycle) and the render time, so the web follows them with no authoring; a scene without an
// enabled Water gets no caustics (the extract leaves post.caustics disabled, and the pass needs a drawn water plane
// anyway). There is no system and nothing is stored between ticks: the component never feeds the simulation or the
// state hash. kit3d.h is not touched.
//
//   Caustics3D  Authoring (all Save, clamped by the renderer to the Range): `enabled` (1); `strength` (1, 0..4) the
//               gain on the sun colour where the web is brightest; `scale` (2, 0.25..16 m) the web's cell size on the
//               water plane; `speed` (1, 0..4) how fast the web moves and changes; `absorption` (0.15, 0..4 per m) the
//               fade along the sun's path through the water; `max_depth` (30, 0.5..200 m) below this depth under the
//               plane nothing is added.

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct Caustics3D {
    f32 strength = 1.0f;
    f32 scale = 2.0f;
    f32 speed = 1.0f;
    f32 absorption = 0.15f;
    f32 max_depth = 30.0f;
    u8 enabled = 1;
    u8 pad_[3] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(Caustics3D, (enabled, Save, Range{0, 1}, Help{"0: no caustics"}),
              (strength, Save, Range{0, 4}, Help{"gain on the sun colour where the web is brightest"}),
              (scale, Save, Range{0.25f, 16}, Help{"the web's cell size on the water plane (metres)"}),
              (speed, Save, Range{0, 4}, Help{"how fast the web moves and changes, 1 = the reference rate"}),
              (absorption, Save, Range{0, 4}, Help{"fade per metre of water along the sun's path"}),
              (max_depth, Save, Range{0.5f, 200}, Help{"below this many metres under the water nothing is added"}))

}  // namespace mce::kit
