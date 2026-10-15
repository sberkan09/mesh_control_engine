#pragma once
// Render quality tiers (M-MOBILE step 8): one switch for the 3D path's cost knobs, so a phone can run
// the same scene as a desktop at a frame budget it holds. `r.quality` picks the tier (auto = Medium
// on Android/iOS, High elsewhere); hosts apply it when they set up Forward3D and each Scene3D.
//
//   tier     MSAA  shadow map  cascades  bloom  AA    render scale  GI volume (M-VISUAL 6)
//   Low      1x    1024        1         off    FXAA  0.67          the bake only (none without one)
//   Medium   2x    1024        2         on     -     0.85          runtime updates
//   High     4x    2048        3         on     TAA   1.0           runtime updates
//   (M-VISUAL 5: TAA in place of the pre-tier FXAA on High)
//
// Render scale shrinks the 3D targets (HDR, depth, bloom chain); the tonemap/FXAA pass upsamples
// into the full-size output, so 2D and UI drawn afterwards stay sharp.

#include "mce/render/forward3d.h"
#include "mce/render/scene3d.h"

namespace mce::render {

enum class QualityTier : u8 { Low, Medium, High };

struct QualitySettings {
    u32 msaa = 4;
    u32 shadow_size = 2048;  // Forward3DDesc::shadow_size and ShadowSettings::resolution
    u32 shadow_cascades = 3;
    bool bloom = true;
    bool fxaa = true;
    f32 render_scale = 1.0f;
    bool taa = false;  // PostFx::taa (replaces FXAA where both are on)
    bool gi_updates = true;  // GiVolume::updates: the irradiance volume is captured at runtime
};

QualitySettings quality_settings(QualityTier tier);
const char* quality_tier_name(QualityTier tier);
// The tier `r.quality` names ("auto", "low", "medium", "high"); auto resolves per platform.
QualityTier quality_tier();

// Forward3D creation knobs: msaa, shadow_size, render_scale.
void apply_quality(const QualitySettings& q, Forward3DDesc& desc);
// Per-scene knobs: cascade count and resolution, bloom, FXAA, TAA. Distances and splits stay the scene's.
void apply_quality(const QualitySettings& q, ShadowSettings& shadows, PostFx& post);
// The irradiance volume's runtime updates (a scene that turned them off keeps them off).
void apply_quality(const QualitySettings& q, GiVolume& gi);

}  // namespace mce::render
