#include "mce/render/quality.h"

#include "mce/convar.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace mce::render {
namespace {

MCE_CVAR(cv_quality, "r.quality", "auto", "3D quality tier: auto, low, medium, high (auto = medium on phones)",
         CVarFlags::RequiresRestart);

}  // namespace

QualitySettings quality_settings(QualityTier tier) {
    QualitySettings q;
    switch (tier) {
        case QualityTier::Low:
            q = QualitySettings{1, 1024, 1, false, true, 0.67f};
            q.gi_updates = false;
            q.volumetrics = false;
            q.dof_motion_blur = false;
            break;
        case QualityTier::Medium:
            q = QualitySettings{2, 1024, 2, true, false, 0.85f};
            q.volumetric_tile = 16;
            q.volumetric_slices = 48;
            break;
        case QualityTier::High:
            q.fxaa = false;
            q.taa = true;
            break;
    }
    return q;
}

const char* quality_tier_name(QualityTier tier) {
    switch (tier) {
        case QualityTier::Low: return "low";
        case QualityTier::Medium: return "medium";
        case QualityTier::High: return "high";
    }
    return "?";
}

QualityTier quality_tier() {
    const StringView v = cv_quality.get_string();
    if (v == "low") return QualityTier::Low;
    if (v == "medium") return QualityTier::Medium;
    if (v == "high") return QualityTier::High;
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    return QualityTier::Medium;
#else
    return QualityTier::High;
#endif
}

void apply_quality(const QualitySettings& q, Forward3DDesc& desc) {
    desc.msaa = q.msaa;
    desc.shadow_size = q.shadow_size;
    desc.render_scale = q.render_scale;
}

void apply_quality(const QualitySettings& q, ShadowSettings& shadows, PostFx& post) {
    shadows.cascades = q.shadow_cascades;
    shadows.resolution = q.shadow_size;
    post.bloom = q.bloom;
    post.fxaa = q.fxaa;
    post.taa = q.taa;
    post.dof.enabled = post.dof.enabled && q.dof_motion_blur;
    post.motion_blur.enabled = post.motion_blur.enabled && q.dof_motion_blur;
}

void apply_quality(const QualitySettings& q, GiVolume& gi) { gi.updates = gi.updates && q.gi_updates; }

void apply_quality(const QualitySettings& q, Volumetrics& v) {
    v.enabled = v.enabled && q.volumetrics;
    v.tile = v.tile > q.volumetric_tile ? v.tile : q.volumetric_tile;
    v.slices = v.slices < q.volumetric_slices ? v.slices : q.volumetric_slices;
}

}  // namespace mce::render
