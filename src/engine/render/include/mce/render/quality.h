#pragma once
// Render quality tiers (M-MOBILE step 8): one switch for the 3D path's cost knobs, so a phone can run
// the same scene as a desktop at a frame budget it holds. `r.quality` picks the tier (auto = Medium
// on Android/iOS, High elsewhere); hosts apply it when they set up Forward3D and each Scene3D.
//
//   tier     MSAA  shadow map  cascades  bloom  AA    render scale  GI volume (M-VISUAL 6)                volumetric fog (7)
//   Low      1x    1024        1         off    FXAA  0.67          the bake only (none without one)     off (exp² fog)
//   Medium   2x    1024        2         on     -     0.85          runtime updates                      16 px, 48 slices
//   High     4x    2048        3         on     TAA   1.0           runtime updates                      8 px, 64 slices
//   terrain LOD error (M-TERRAIN): 4 px on Low, 2 px on Medium, 1 px on High; splat layers: weight blend on
//   Low, height blend on Medium, height blend and triplanar on High
//   (M-VISUAL 5: TAA in place of the pre-tier FXAA on High; M-VISUAL 8: Low drops depth of field and
//   motion blur, every tier keeps auto exposure and the color grade; M-VISUAL2 1: Low drops
//   screen-space reflections; M-PARTICLEQ 28a: particle_scale, the share of a weather emitter's rate and
//   max_particles kept, is 0.5 on Low and 1 on Medium and High)
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
    bool volumetrics = true;     // Volumetrics::enabled is honored (off: the exp² fog)
    u32 volumetric_tile = 8;     // Volumetrics::tile, at least
    u32 volumetric_slices = 64;  // Volumetrics::slices, at most
    bool dof_motion_blur = true; // PostFx::dof and PostFx::motion_blur are honored
    bool ssr = true;             // ScreenSpaceReflections::enabled is honored (M-VISUAL2 1)
    bool contact_shadows = true; // ContactShadows::enabled is honored (M-VISUAL2 3)
    // M-WETSURFACE 5a: WetSurfaces is honored (off: wetness 0) and, separately, its puddles. Low keeps the
    // darkening and the lower roughness but drops puddles. wet_puddles has no effect while wet_surfaces is off
    // (wetness 0 draws the dry frame, puddles included).
    bool wet_surfaces = true;
    bool wet_puddles = true;
    // M-SNOWCOVER 6a: SnowCover is honored (off: cover 0) and, separately, its noise breakup. Low keeps the
    // whitening and drops the noise. snow_noise has no effect while snow_cover is off (cover 0 draws the
    // snowless frame, noise included).
    bool snow_cover = true;
    bool snow_noise = true;
    // M-RAINFX 7a: Ripples are honored (off: strength 0, the ripple-less frame). Low turns them off.
    bool rain_ripples = true;
    // M-CLOUDS 9a: Clouds are honored (off: coverage 0, the cloudless frame). Low turns them off.
    bool clouds = true;
    // M-NIGHTSKY 14c: the night sky is honored (off: no moon, no stars). Low keeps the moon disk but drops the
    // stars and the twinkle.
    bool night_sky = true;
    // M-WATER 15b: the water plane is honored (off: no plane). Low keeps a flat tinted plane with the sky
    // reflection: no animated normals, no foam.
    bool water = true;
    // M-AURORA 16b: the aurora curtains are honored (off: level 0, the frame without them). Low turns them off.
    bool aurora = true;
    // M-RAINBOW 17b: the rainbow is honored (off: level 0, the frame without it). Low turns it off.
    bool rainbow = true;
    // M-METEOR 18b: meteors are honored (off: level 0, the frame without them). Low turns them off.
    bool meteor = true;
    // M-PARTICLEQ 28a: the share of rate and max_particles a render-side weather emitter keeps (0..1); Low 0.5,
    // Medium and High 1. No CVar. The weather particles read it in a later step (28b).
    f32 particle_scale = 1.0f;
    // M-TERRAIN: the screen error, in pixels, a terrain LOD node may show before a finer one replaces
    // it (TerrainSelectParams::pixel_error; a Terrain3D's own pixel_error wins when set).
    f32 terrain_pixel_error = 1.0f;
    // M-TERRAIN 2: triplanar terrain layers also project along x and z on steep ground (three fetches
    // per map), and splat layers meet by height rather than by weight alone.
    bool terrain_triplanar = true;
    bool terrain_height_blend = true;
    // M-VISUAL3 2: parallax occlusion mapping fades out at this view depth (meters; 0 = off) and a
    // material's march steps are clamped to `parallax_max_steps`. Low: off. Medium: 15 m, 16 steps.
    f32 parallax_distance = 30.0f;
    u32 parallax_max_steps = 32;
};

QualitySettings quality_settings(QualityTier tier);
const char* quality_tier_name(QualityTier tier);
// The tier `r.quality` names ("auto", "low", "medium", "high"); auto resolves per platform.
QualityTier quality_tier();

// Forward3D creation knobs: msaa, shadow_size, render_scale.
void apply_quality(const QualitySettings& q, Forward3DDesc& desc);
// Per-scene knobs: cascade count and resolution, bloom, FXAA, TAA, depth of field and motion blur (off
// on Low). Distances and splits stay the scene's.
void apply_quality(const QualitySettings& q, ShadowSettings& shadows, PostFx& post);
// The irradiance volume's runtime updates (a scene that turned them off keeps them off).
void apply_quality(const QualitySettings& q, GiVolume& gi);
// Volumetric fog: off on Low; the froxel grid no finer than the tier's (a scene's coarser grid stays).
void apply_quality(const QualitySettings& q, Volumetrics& v);
// Screen-space reflections: off on Low (the probes and the sky remain).
void apply_quality(const QualitySettings& q, ScreenSpaceReflections& ssr);
// Contact shadows: off on Low (the cascades remain).
void apply_quality(const QualitySettings& q, ContactShadows& c);
// Parallax occlusion mapping: its distance and step cap (off on Low).
void apply_quality(const QualitySettings& q, ParallaxSettings& p);
// Wet surfaces: the weather's wetness, zeroed when the tier or `r.wet_surfaces 0` switches them off;
// puddles_enabled follows the tier (and means nothing at wetness 0). wetness, puddles, darken and roughness are
// clamped to 0..1 (NaN to 0) and puddle_scale to 0.25..256 m. Unlike the in-place overloads above it returns a
// copy: the extract builds the value from kit data and hands it straight to Scene3D::set_wet_surfaces, which
// exposes no mutable reference to apply it to.
WetSurfaces apply_quality(const QualitySettings& q, const WetSurfaces& wet);
// Snow cover: the weather's cover, zeroed when the tier or `r.snow_cover 0` switches it off; noise_enabled
// follows the tier (and means nothing at cover 0). cover, coverage and wind_bias are clamped to 0..1 (NaN to 0)
// and noise_scale to 0.25..256 m. Returns a copy for the same reason as the WetSurfaces overload: the extract
// builds the value from kit data and hands it straight to Scene3D::set_snow_cover, which exposes no mutable
// reference to apply it to.
SnowCover apply_quality(const QualitySettings& q, const SnowCover& snow);
// Rain ripples: the weather's ripples, strength zeroed when the tier or `r.rain_ripples 0` switches them off.
// strength, density and ring_width are clamped to 0..1 (NaN to 0), speed to 0.1..10 (NaN to 1, the nominal
// rate); `enabled` passes through. Returns a copy for the same reason as the SnowCover overload.
Ripples apply_quality(const QualitySettings& q, const Ripples& ripples);
// Clouds: the weather's cloud layer, coverage zeroed when the tier or `r.clouds 0` switches it off. coverage and
// density are clamped to 0..1 (NaN to 0), scale_m to 50..20000 (NaN to 900), speed to 0..10 (NaN to 1),
// height_m to 100..20000 (NaN to 2000) and wind_dir is normalized (NaN / zero to +x); `enabled` passes through.
// Returns a copy for the same reason as the SnowCover overload.
Clouds apply_quality(const QualitySettings& q, const Clouds& clouds);
// NightSky: the moon and stars. `r.nightsky 0` zeroes moon_intensity and star_density (the frame without it);
// the Low tier (night_sky false) keeps the moon disk but zeroes star_density and star_twinkle. moon_dir is
// normalized (NaN / zero to +y), moon_cos_size is clamped to 0.9..1 (NaN to 0.9995), moon_intensity to 0..8,
// moon_color per channel, moon_phase, star_density, star_twinkle and night_fade to 0..1, star_brightness to 0..4
// (all NaN to 0, except star_brightness NaN to 1); a non-finite star_time becomes 0; `enabled` passes through.
// Returns a copy for the same reason as the SnowCover overload.
NightSky apply_quality(const QualitySettings& q, const NightSky& sky);
// WaterSurface: `r.water 0` turns `enabled` off (no plane, the frame without it); the Low tier (water false) zeroes
// amplitude and foam_amount (flat plane, no foam). Clamps: level_y -1000..1000 (NaN 0), half_extent 1..20000 (NaN
// 500), wave_count 1..8, amplitude 0..10 (NaN 0), wavelength 1..500 (NaN 24), steepness / reflectivity / roughness /
// foam_amount 0..1 (NaN 0, roughness NaN 0.08, reflectivity NaN 1), speed_scale 0..4 (NaN 1), colors 0..1 per
// channel (NaN 0), depth_scale_m 0.1..200 (NaN 8), foam_width_m 0.05..20 (NaN 1.5); wind_dir is normalized (NaN /
// zero -> +x) and a non-finite time becomes 0. Returns a copy for the same reason as the SnowCover overload.
WaterSurface apply_quality(const QualitySettings& q, const WaterSurface& water);
// AuroraSky: `r.aurora 0` and the Low tier (aurora false) zero `level` (the frame without it). Clamps: level 0..4,
// base_height 0.02..0.6, height_span 0.05..0.8, half_width_cos -1..1, curtain_count 1..6, speed 0..4, colors 0..1 per
// channel (NaN to 0, except base_height 0.18, height_span 0.35, half_width_cos 0.8192, curtain_count 3, speed 1); azimuth
// is normalized (NaN / zero -> (0, 1)) and a non-finite time becomes 0; `enabled` passes through. Returns a copy for
// the same reason as the SnowCover overload.
AuroraSky apply_quality(const QualitySettings& q, const AuroraSky& aurora);
// RainbowSky: `r.rainbow 0` and the Low tier (rainbow false) zero `level` (the frame without it). Clamps: level 0..4
// (NaN 0), band_width 0.5..2 (NaN 1), secondary 0..1 (NaN 0); anti_solar is normalized (NaN / zero -> (0, 0, 1));
// `enabled` passes through. Returns a copy for the same reason as the SnowCover overload.
RainbowSky apply_quality(const QualitySettings& q, const RainbowSky& rainbow);
// MeteorSky: `r.meteor 0` and the Low tier (meteor false) zero `level` (the frame without it). Clamps: level 0..4
// (NaN 0), rate 0..30 (NaN 4), brightness 0..4 (NaN 1), length 0.5..2 (NaN 1), speed 0.5..2 (NaN 1), time 0..60
// (non-finite 0), seed 0..65535; `enabled` passes through. Returns a copy for the same reason as the SnowCover overload.
MeteorSky apply_quality(const QualitySettings& q, const MeteorSky& meteor);

}  // namespace mce::render
