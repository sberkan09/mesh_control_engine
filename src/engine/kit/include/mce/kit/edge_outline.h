#pragma once
// Edge outlines as data (M-OUTLINE step 39d). One optional EdgeOutline3D per scene (the first in ECS order is read);
// install_kit3d registers the component next to LensFlare3D. There is no system and nothing is stored between ticks:
// Kit3DExtract copies the fields into Scene3D post.outline (the render side, 39a to 39c) after running them through
// render::apply_quality, so the outline never feeds the simulation or the state hash. There is no kit-side formula: the
// fog density and the Volumetrics check are the renderer's.
//
//   EdgeOutline3D  Authoring (all Save, clamped to the Range at the extract): `enabled` (1); `color_r`, `color_g`,
//                  `color_b` (0.02, 0.02, 0.025; 0..16, linear HDR, above 1 glows under bloom); `opacity` (1, 0..1);
//                  `thickness` (1, 0.5..4, output pixels); `depth_threshold` (0.05, 0.001..1) the relative view-depth
//                  jump that starts a silhouette; `normal_threshold` (0.4, 0.01..2) the 1 - dot(n, n') that starts a
//                  crease; `depth_weight` and `normal_weight` (1, 0..1; 0 turns that kind off); `fade_start` (30,
//                  0..10000) and `fade_end` (80, 0..10000) the view depth range where lines fade out (fade_end is never
//                  below fade_start); `sky_silhouette` (1) lines between geometry and the sky; `fog_follow` (1) lines
//                  fade in the exp^2 fog (ignored while Volumetrics is on).

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct EdgeOutline3D {
    f32 color_r = 0.02f;
    f32 color_g = 0.02f;
    f32 color_b = 0.025f;
    f32 opacity = 1.0f;
    f32 thickness = 1.0f;
    f32 depth_threshold = 0.05f;
    f32 normal_threshold = 0.4f;
    f32 depth_weight = 1.0f;
    f32 normal_weight = 1.0f;
    f32 fade_start = 30.0f;
    f32 fade_end = 80.0f;
    u8 enabled = 1;
    u8 sky_silhouette = 1;
    u8 fog_follow = 1;
    u8 pad_[1] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(EdgeOutline3D, (enabled, Save, Range{0, 1}, Help{"0: no edge outlines"}),
              (color_r, Save, Range{0, 16}, Help{"line colour, red, linear HDR (above 1 glows under bloom)"}),
              (color_g, Save, Range{0, 16}, Help{"line colour, green, linear HDR"}),
              (color_b, Save, Range{0, 16}, Help{"line colour, blue, linear HDR"}),
              (opacity, Save, Range{0, 1}, Help{"how far a full edge replaces the pixel with the line colour"}),
              (thickness, Save, Range{0.5f, 4}, Help{"tap distance in output pixels (the line is about twice this wide)"}),
              (depth_threshold, Save, Range{0.001f, 1}, Help{"relative view-depth jump that starts a silhouette line"}),
              (normal_threshold, Save, Range{0.01f, 2}, Help{"1 - dot(normal, neighbour normal) that starts a crease line"}),
              (depth_weight, Save, Range{0, 1}, Help{"strength of silhouette lines, 0 = none"}),
              (normal_weight, Save, Range{0, 1}, Help{"strength of crease lines, 0 = none"}),
              (fade_start, Save, Range{0, 10000}, Help{"view depth in metres where lines start to fade"}),
              (fade_end, Save, Range{0, 10000}, Help{"view depth in metres where lines are gone"}),
              (sky_silhouette, Save, Range{0, 1}, Help{"1: geometry against the sky gets a silhouette line"}),
              (fog_follow, Save, Range{0, 1}, Help{"1: lines fade in the exp^2 fog (ignored while Volumetrics is on)"}))

}  // namespace mce::kit
