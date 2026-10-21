#include "mce/render/quality.h"

#include "mce/convar.h"
#include "unit_clamp.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace mce::render {

using detail::noise_cell;
using detail::unit01;

namespace {

MCE_CVAR(cv_quality, "r.quality", "auto", "3D quality tier: auto, low, medium, high (auto = medium on phones)",
         CVarFlags::RequiresRestart | CVarFlags::Saved);
MCE_CVAR(cv_wet_surfaces, "r.wet_surfaces", true,
         "Wet surfaces from the weather (WetSurfaces; off = the dry frame)", CVarFlags::Saved);
MCE_CVAR(cv_snow_cover, "r.snow_cover", true,
         "Snow cover from the weather (SnowCover; off = the snowless frame)", CVarFlags::Saved);
MCE_CVAR(cv_rain_ripples, "r.rain_ripples", true,
         "Rain ripples on puddles from the weather (Ripples; off = the ripple-less frame)", CVarFlags::Saved);
MCE_CVAR(cv_clouds, "r.clouds", true,
         "Cloud layer in the sky from the weather (Clouds; off = the cloudless frame)", CVarFlags::Saved);

}  // namespace

QualitySettings quality_settings(QualityTier tier) {
    QualitySettings q;
    switch (tier) {
        case QualityTier::Low:
            q = QualitySettings{1, 1024, 1, false, true, 0.67f};
            q.gi_updates = false;
            q.volumetrics = false;
            q.dof_motion_blur = false;
            q.ssr = false;
            q.contact_shadows = false;
            q.terrain_pixel_error = 4.0f;
            q.terrain_triplanar = false;
            q.terrain_height_blend = false;
            q.parallax_distance = 0.0f;
            q.parallax_max_steps = 0;
            q.wet_puddles = false;
            q.snow_noise = false;
            q.rain_ripples = false;
            q.clouds = false;
            break;
        case QualityTier::Medium:
            q = QualitySettings{2, 1024, 2, true, false, 0.85f};
            q.volumetric_tile = 16;
            q.volumetric_slices = 48;
            q.terrain_pixel_error = 2.0f;
            q.terrain_triplanar = false;
            q.parallax_distance = 15.0f;
            q.parallax_max_steps = 16;
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

void apply_quality(const QualitySettings& q, ScreenSpaceReflections& ssr) { ssr.enabled = ssr.enabled && q.ssr; }
void apply_quality(const QualitySettings& q, ContactShadows& c) { c.enabled = c.enabled && q.contact_shadows; }

void apply_quality(const QualitySettings& q, ParallaxSettings& p) {
    p.distance = q.parallax_distance > 0.0f ? (p.distance < q.parallax_distance ? p.distance : q.parallax_distance) : 0.0f;
    p.max_steps = p.max_steps < q.parallax_max_steps ? p.max_steps : q.parallax_max_steps;
}

WetSurfaces apply_quality(const QualitySettings& q, const WetSurfaces& wet) {
    WetSurfaces out = wet;
    out.wetness = q.wet_surfaces && cv_wet_surfaces.get_bool() ? unit01(wet.wetness) : 0.0f;
    out.darken = unit01(wet.darken);
    out.roughness = unit01(wet.roughness);
    out.puddles = unit01(wet.puddles);
    out.puddle_scale = noise_cell(wet.puddle_scale);
    // Off with wet_surfaces (wetness 0 skips the whole shader branch, puddles included) whatever this says.
    out.puddles_enabled = wet.puddles_enabled && q.wet_puddles;
    return out;
}

SnowCover apply_quality(const QualitySettings& q, const SnowCover& snow) {
    SnowCover out = snow;
    out.cover = q.snow_cover && cv_snow_cover.get_bool() ? unit01(snow.cover) : 0.0f;
    out.coverage = unit01(snow.coverage);
    out.wind_bias = unit01(snow.wind_bias);
    out.noise_scale = noise_cell(snow.noise_scale);
    out.wind_dir = detail::unit_wind_dir(snow.wind_dir);
    // Meaningless with snow_cover off (cover 0 skips the whole shader branch, noise included).
    out.noise_enabled = snow.noise_enabled && q.snow_noise;
    return out;
}

Ripples apply_quality(const QualitySettings& q, const Ripples& ripples) {
    Ripples out = ripples;
    out.strength = q.rain_ripples && cv_rain_ripples.get_bool() ? unit01(ripples.strength) : 0.0f;
    out.density = unit01(ripples.density);
    out.ring_width = unit01(ripples.ring_width);
    out.speed = detail::ripple_speed(ripples.speed);  // NaN -> the nominal rate 1
    return out;
}

Clouds apply_quality(const QualitySettings& q, const Clouds& clouds) {
    Clouds out = clouds;
    out.coverage = q.clouds && cv_clouds.get_bool() ? unit01(clouds.coverage) : 0.0f;
    out.density = unit01(clouds.density);
    out.scale_m = detail::cloud_scale(clouds.scale_m);
    out.speed = detail::cloud_speed(clouds.speed);
    out.wind_dir = detail::unit_wind_dir(clouds.wind_dir);
    out.height_m = detail::cloud_height(clouds.height_m);
    return out;
}

void apply_quality(const QualitySettings& q, Volumetrics& v) {
    v.enabled = v.enabled && q.volumetrics;
    v.tile = v.tile > q.volumetric_tile ? v.tile : q.volumetric_tile;
    v.slices = v.slices < q.volumetric_slices ? v.slices : q.volumetric_slices;
}

}  // namespace mce::render
