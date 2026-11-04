#pragma once
// Sun lens flare as data (M-LENSFLARE step 38d). One optional LensFlare3D per scene (the first in ECS order is read);
// install_kit3d registers the component next to Environment3D. There is no system and nothing is stored between ticks:
// Kit3DExtract copies the fields into Scene3D post.flare (the render side, 38a to 38c), so the flare never feeds the
// simulation or the state hash.
//
//   LensFlare3D  Authoring (all Save, clamped by the renderer to the Range): `enabled` (1); `intensity` (1, 0..4) overall gain;
//                `halo` (1, 0..4) and `halo_radius` (0.25, 0.02..0.6) the soft glare; `ghost_count` (4, 0..6),
//                `ghost_intensity` (1, 0..4), `ghost_spacing` (1, 0.25..2) and `ghost_chroma` (0.5, 0..1) the ghosts along
//                the sun-to-centre axis; `starburst` (0, 0..4) and `starburst_rays` (6, 2..16) the optional rays;
//                `occlusion_radius` (0.015, 0.002..0.1) the depth sample disk; `edge_fade` (0.1, 0.01..0.5) the fade at a
//                border; `cloud_dim` (1, 0..1) how far cloud cover and rain dim the flare.
//
// lens_flare_visibility(f, cloud_coverage, rain_intensity) = clamp(1 - cloud_dim * max(cloud_coverage, rain_intensity), 0, 1);
// both inputs clamp to 0..1 and NaN counts as 0, a NaN cloud_dim as its default 1. Exactly 1.0f for cloud_dim 0 or clear air.

#include <algorithm>
#include <cmath>

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct LensFlare3D {
    f32 intensity = 1.0f;
    f32 halo = 1.0f;
    f32 halo_radius = 0.25f;
    u32 ghost_count = 4;
    f32 ghost_intensity = 1.0f;
    f32 ghost_spacing = 1.0f;
    f32 ghost_chroma = 0.5f;
    f32 starburst = 0.0f;
    u32 starburst_rays = 6;
    f32 occlusion_radius = 0.015f;
    f32 edge_fade = 0.1f;
    f32 cloud_dim = 1.0f;
    u8 enabled = 1;
    u8 pad_[3] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(LensFlare3D, (enabled, Save, Range{0, 1}, Help{"0: no lens flare"}),
              (intensity, Save, Range{0, 4}, Help{"overall gain on the sun's light"}),
              (halo, Save, Range{0, 4}, Help{"strength of the soft glare around the sun"}),
              (halo_radius, Save, Range{0.02f, 0.6f}, Help{"halo falloff radius, fraction of the view height"}),
              (ghost_count, Save, Range{0, 6}, Help{"ghosts drawn along the sun-to-centre axis"}),
              (ghost_intensity, Save, Range{0, 4}, Help{"gain on the ghosts"}),
              (ghost_spacing, Save, Range{0.25f, 2}, Help{"scales each ghost's position along the axis"}),
              (ghost_chroma, Save, Range{0, 1}, Help{"0 = white ghosts, 1 = fully tinted"}),
              (starburst, Save, Range{0, 4}, Help{"ray strength; 0 = no starburst"}),
              (starburst_rays, Save, Range{2, 16}, Help{"number of starburst rays"}),
              (occlusion_radius, Save, Range{0.002f, 0.1f}, Help{"radius of the depth sample disk around the sun, fraction of the view height"}),
              (edge_fade, Save, Range{0.01f, 0.5f}, Help{"fraction of the screen over which the flare fades at a border"}),
              (cloud_dim, Save, Range{0, 1}, Help{"how far cloud cover and rain dim the flare"}))

// The caller's dimming for PostFx::flare.visibility, in [0, 1].
inline f32 lens_flare_visibility(const LensFlare3D& f, f32 cloud_coverage, f32 rain_intensity) {
    const f32 dim = std::isnan(f.cloud_dim) ? 1.0f : std::clamp(f.cloud_dim, 0.0f, 1.0f);
    const f32 cover = std::isnan(cloud_coverage) ? 0.0f : std::clamp(cloud_coverage, 0.0f, 1.0f);
    const f32 rain = std::isnan(rain_intensity) ? 0.0f : std::clamp(rain_intensity, 0.0f, 1.0f);
    return std::clamp(1.0f - dim * std::max(cover, rain), 0.0f, 1.0f);
}

}  // namespace mce::kit
