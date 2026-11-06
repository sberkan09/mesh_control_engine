#pragma once
// Heat haze as data (M-HEATHAZE step 43d). Any number of HeatHaze3D entities; Kit3DExtract reads the first eight in entity id
// order (the render side caps the count by the quality tier again) and fills Scene3D post.haze from their world
// transforms (the render side is 43a to 43c). install_kit3d registers the component next to LensRain3D; no system, no
// runtime state: the component never hashes anything that changes by itself.
//
//   HeatHaze3D  Authoring (all Save, clamped by the renderer to the Range): `enabled` (1); `radius` (1 m, 0.05..50);
//               `strength` (0.5, 0..2); `rise` (0.6, 0..4); `scale` (1, 0.25..4); `softness` (0.5, 0..1); `offset_y` (0,
//               -50..50 m): the sphere's centre sits this far above the entity's world position, so a volume placed on a fire
//               entity floats above it; `fire_follow` (0, 0..1): when above 0 and the entity has a PointLight3D, the
//               strength is scaled by heat_haze_fire_scale (below), so a flickering fire hazes more when bright.
//
// The centre is the entity's world position (WorldTransform, else LocalTransform) plus (0, offset_y, 0), in the same
// render-relative space as the lights and the meshes (a floating-origin rebase moves the entity, and so the centre, with the
// rest of the world; nothing is held in absolute coordinates). Nothing is written back to the world.

#include <algorithm>
#include <cmath>

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct HeatHaze3D {
    f32 radius = 1.0f;
    f32 strength = 0.5f;
    f32 rise = 0.6f;
    f32 scale = 1.0f;
    f32 softness = 0.5f;
    f32 offset_y = 0.0f;
    f32 fire_follow = 0.0f;
    u8 enabled = 1;
    u8 pad_[3] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(HeatHaze3D, (enabled, Save, Range{0, 1}, Help{"0: no haze from this entity"}),
              (radius, Save, Range{0.05f, 50}, Help{"sphere radius in metres"}),
              (strength, Save, Range{0, 2}, Help{"how far the picture behind the sphere is displaced"}),
              (rise, Save, Range{0, 4}, Help{"how fast the shimmer rises"}),
              (scale, Save, Range{0.25f, 4}, Help{"shimmer frequency multiplier"}),
              (softness, Save, Range{0, 1}, Help{"width of the soft rim, a fraction of the radius"}),
              (offset_y, Save, Range{-50, 50}, Help{"the centre sits this far above the entity (metres)"}),
              (fire_follow, Save, Range{0, 1}, Help{"how much the strength follows the entity's point light (a fire)"}))

// The light brightness (intensity per metre of range) at which fire_follow leaves the strength unchanged: PointLight3D's
// defaults (3 over 7 m).
inline constexpr f32 kHeatHazeFireRef = 3.0f / 7.0f;

// The strength multiplier of fire_follow: 1 + follow * (k - 1), k = clamp(intensity / max(radius, 0.1) / kHeatHazeFireRef, 0,
// 2), follow clamped to 0..1 (NaN 0). exactly 1.0f when follow is 0 or the light's numbers are not finite. A dim flicker
// (intensity under the reference) lowers it, a bright one raises it, at most to 1 + follow.
inline f32 heat_haze_fire_scale(f32 follow, f32 light_intensity, f32 light_radius) {
    if (!(follow > 0.0f)) return 1.0f;
    if (!std::isfinite(light_intensity) || !std::isfinite(light_radius)) return 1.0f;
    const f32 f = std::min(follow, 1.0f);
    const f32 k = std::clamp(light_intensity / std::max(light_radius, 0.1f) / kHeatHazeFireRef, 0.0f, 2.0f);
    return 1.0f + f * (k - 1.0f);
}

}  // namespace mce::kit
