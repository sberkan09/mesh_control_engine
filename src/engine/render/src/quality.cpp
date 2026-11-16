#include "mce/render/quality.h"

#include <cmath>

#include "mce/convar.h"
#include "mce/render/translucency.h"
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
MCE_CVAR(cv_cirrus, "r.cirrus", true,
         "High cirrus streaks above the cloud layer (Cirrus; off = the frame without them)", CVarFlags::Saved);
MCE_CVAR(cv_halo, "r.halo", true,
         "Ice halo and sun dogs seen through the cirrus layer (Halo; off = the frame without them)", CVarFlags::Saved);
MCE_CVAR(cv_nightsky, "r.nightsky", true,
         "Moon and stars in the night sky (NightSky; off = the moonless, starless frame)", CVarFlags::Saved);
MCE_CVAR(cv_water, "r.water", true,
         "Water plane from the Water component (WaterSurface; off = the frame without a plane)", CVarFlags::Saved);
MCE_CVAR(cv_aurora, "r.aurora", true,
         "Aurora curtains in the night sky (AuroraSky; off = the frame without them)", CVarFlags::Saved);
MCE_CVAR(cv_rainbow, "r.rainbow", true,
         "Rainbow in the sunlit sky (RainbowSky; off = the frame without it)", CVarFlags::Saved);
MCE_CVAR(cv_meteor, "r.meteor", true,
         "Meteors in the night sky (MeteorSky; off = the frame without them)", CVarFlags::Saved);
MCE_CVAR(cv_lens_flare, "r.lens_flare", true,
         "Sun lens flare (PostFx::flare; off = the frame without it)", CVarFlags::Saved);
MCE_CVAR(cv_lens_rain, "r.lens_rain", true,
         "Rain on the camera lens (PostFx::rain; off = the frame without it)", CVarFlags::Saved);
MCE_CVAR(cv_heat_haze, "r.heat_haze", true,
         "Heat haze (PostFx::haze; off = the frame without it)", CVarFlags::Saved);
MCE_CVAR(cv_underwater, "r.underwater", true,
         "Camera under water look (PostFx::underwater; off = the frame without it)", CVarFlags::Saved);
MCE_CVAR(cv_sun_shafts, "r.sun_shafts", true,
         "Sun light shafts (PostFx::shafts; off = the frame without it)", CVarFlags::Saved);
MCE_CVAR(cv_cloud_shadows, "r.cloud_shadows", true,
         "Cloud shadows on the ground (PostFx::cloud_shadow; off = the frame without them)", CVarFlags::Saved);
MCE_CVAR(cv_caustics, "r.caustics", true,
         "Caustics under water (PostFx::caustics; off = the frame without them)", CVarFlags::Saved);
MCE_CVAR(cv_dynres, "r.dynres", true,
         "Dynamic resolution scaling (PostFx::dynres; off = the fixed render scale)", CVarFlags::Saved);
MCE_CVAR(cv_dynres_target_ms, "r.dynres.target_ms", 0.0f,
         "Dynamic resolution GPU budget in ms, 4..100 (0 = the setting's own)", CVarFlags::Saved);
MCE_CVAR(cv_dynres_inject, "r.dynres.inject", "",
         "Test hook: a fake frame cost schedule 'ms@frame,...' (e.g. 24@0,8@240) replaces the measured GPU time; empty = measured",
         CVarFlags::None);
MCE_CVAR(cv_edge_outline, "r.edge_outline", true,
         "Edge outlines (PostFx::outline; off = the frame without them)", CVarFlags::Saved);
MCE_CVAR(cv_light_cookies, "r.light_cookies", true,
         "Light cookies (a texture projected by spot lights and the sun; off = plain lights)", CVarFlags::Saved);
MCE_CVAR(cv_translucency, "r.translucency", true,
         "Thin-surface translucency and wrap lighting (material translucency / wrap; off = plain diffuse)",
         CVarFlags::Saved);
MCE_CVAR(cv_particles, "r.particles", 1.0f,
         "Share (0..1) of the weather particles kept, on top of the tier's particle_scale (0 = none)", CVarFlags::Saved);

}  // namespace

bool translucency_cvar_on() { return cv_translucency.get_bool(); }

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
            q.cirrus = false;
            q.halo = false;
            q.night_sky = false;
            q.water = false;
            q.aurora = false;
            q.rainbow = false;
            q.meteor = false;
            q.lens_flare = false;
            q.lens_flare_starburst = false;
            q.lens_rain = false;
            q.lens_rain_layers = 0;
            q.heat_haze = false;
            q.heat_haze_volumes = 0;
            q.underwater_detail = 0;
            q.sun_shafts = false;
            q.sun_shafts_samples = 0;
            q.cloud_shadows = false;
            q.cloud_shadow_octaves = 0;
            q.caustics = false;
            q.caustics_octaves = 0;
            q.dynres_floor = 0.5f;
            q.edge_outline_wide = false;
            q.light_cookie_size = 128;
            q.light_cookie_layers = 4;
            q.light_cookie_lights = 4;
            q.translucency_ambient = false;
            q.translucency_scatter = false;
            q.particle_scale = 0.5f;
            break;
        case QualityTier::Medium:
            q = QualitySettings{2, 1024, 2, true, false, 0.85f};
            q.volumetric_tile = 16;
            q.volumetric_slices = 48;
            q.terrain_pixel_error = 2.0f;
            q.terrain_triplanar = false;
            q.parallax_distance = 15.0f;
            q.parallax_max_steps = 16;
            q.lens_flare_starburst = false;
            q.lens_rain_layers = 1;
            q.heat_haze_volumes = 4;
            q.underwater_detail = 1;
            q.sun_shafts_samples = 16;
            q.cloud_shadow_octaves = 2;
            q.caustics_octaves = 2;
            q.dynres_floor = 0.5f;
            q.light_cookie_size = 256;
            q.light_cookie_layers = 8;
            q.light_cookie_lights = 8;
            break;
        case QualityTier::High:
            q.fxaa = false;
            q.taa = true;
            break;
    }
    // M-PARTICLEUSER 29a: r.particles (0..1, NaN reads 1) scales the tier's share; the default 1 changes nothing.
    const f32 user = cv_particles.get_float();
    q.particle_scale *= user == user ? unit01(user) : 1.0f;
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
    desc.dynres_floor = q.dynres_floor;
}

void apply_quality(const QualitySettings& q, ShadowSettings& shadows, PostFx& post) {
    shadows.cascades = q.shadow_cascades;
    shadows.resolution = q.shadow_size;
    post.bloom = q.bloom;
    post.fxaa = q.fxaa;
    post.taa = q.taa;
    post.dof.enabled = post.dof.enabled && q.dof_motion_blur;
    post.motion_blur.enabled = post.motion_blur.enabled && q.dof_motion_blur;
    // M-LENSFLARE 38b: the sun lens flare. Off for the tier or r.lens_flare 0; the starburst follows the tier.
    LensFlare& fl = post.flare;
    fl.enabled = fl.enabled && q.lens_flare && cv_lens_flare.get_bool();
    fl.intensity = detail::ranged(fl.intensity, 0.0f, 4.0f, 1.0f);
    fl.visibility = detail::ranged(fl.visibility, 0.0f, 1.0f, 1.0f);
    fl.halo = detail::ranged(fl.halo, 0.0f, 4.0f, 1.0f);
    fl.halo_radius = detail::ranged(fl.halo_radius, 0.02f, 0.6f, 0.25f);
    fl.ghost_count = fl.ghost_count < kLensFlareMaxGhosts ? fl.ghost_count : kLensFlareMaxGhosts;
    fl.ghost_intensity = detail::ranged(fl.ghost_intensity, 0.0f, 4.0f, 1.0f);
    fl.ghost_spacing = detail::ranged(fl.ghost_spacing, 0.25f, 2.0f, 1.0f);
    fl.ghost_chroma = detail::ranged(fl.ghost_chroma, 0.0f, 1.0f, 0.5f);
    fl.starburst = q.lens_flare_starburst ? detail::ranged(fl.starburst, 0.0f, 4.0f, 0.0f) : 0.0f;
    fl.starburst_rays = fl.starburst_rays < 2u ? 2u : (fl.starburst_rays > 16u ? 16u : fl.starburst_rays);
    fl.occlusion_radius = detail::ranged(fl.occlusion_radius, 0.002f, 0.1f, 0.015f);
    fl.edge_fade = detail::ranged(fl.edge_fade, 0.01f, 0.5f, 0.1f);
    // M-LENSRAIN 42b: rain on the camera lens. Off for the tier or r.lens_rain 0; the ranges clamp, NaN -> the default.
    LensRain& lr = post.rain;
    lr.enabled = lr.enabled && q.lens_rain && cv_lens_rain.get_bool();
    lr.amount = detail::ranged(lr.amount, 0.0f, 1.0f, 0.0f);
    lr.density = detail::ranged(lr.density, 0.0f, 4.0f, 1.0f);
    lr.size = detail::ranged(lr.size, 0.25f, 4.0f, 1.0f);
    lr.refraction = detail::ranged(lr.refraction, 0.0f, 2.0f, 1.0f);
    lr.blur = detail::ranged(lr.blur, 0.0f, 1.0f, 0.35f);
    lr.trail = detail::ranged(lr.trail, 0.0f, 1.0f, 0.5f);
    lr.fall_speed = detail::ranged(lr.fall_speed, 0.0f, 2.0f, 0.25f);
    // M-HEATHAZE 43b: heat haze. Off for the tier or r.heat_haze 0; the count is trimmed to the tier's cap, a volume with a
    // non-finite centre is dropped (the rest close up), the ranges clamp, NaN -> the default.
    HeatHaze& hh = post.haze;
    {
        const u32 cap = (q.heat_haze && cv_heat_haze.get_bool())
                            ? (q.heat_haze_volumes < kHeatHazeMaxVolumes ? q.heat_haze_volumes : kHeatHazeMaxVolumes)
                            : 0u;
        const u32 in = hh.count < kHeatHazeMaxVolumes ? hh.count : kHeatHazeMaxVolumes;
        u32 out = 0;
        for (u32 i = 0; i < in && out < cap; ++i) {
            HeatHazeVolume v = hh.volumes[i];
            if (!std::isfinite(v.center.x) || !std::isfinite(v.center.y) || !std::isfinite(v.center.z)) continue;
            v.radius = detail::ranged(v.radius, 0.05f, 50.0f, 1.0f);
            v.strength = detail::ranged(v.strength, 0.0f, 2.0f, 0.5f);
            v.rise = detail::ranged(v.rise, 0.0f, 4.0f, 0.6f);
            v.scale = detail::ranged(v.scale, 0.25f, 4.0f, 1.0f);
            v.softness = detail::ranged(v.softness, 0.0f, 1.0f, 0.5f);
            hh.volumes[out++] = v;
        }
        for (u32 i = out; i < kHeatHazeMaxVolumes; ++i) hh.volumes[i] = HeatHazeVolume{};
        hh.count = out;
    }
    // M-UNDERWATER 44b: the camera under water look. Off for the tier or r.underwater 0; the ranges clamp, NaN -> the default
    // (band: 0 = automatic, else 0.05..2).
    Underwater& uw = post.underwater;
    uw.enabled = uw.enabled && q.underwater && cv_underwater.get_bool();
    uw.strength = detail::ranged(uw.strength, 0.0f, 1.0f, 1.0f);
    uw.wobble = detail::ranged(uw.wobble, 0.0f, 2.0f, 0.5f);
    uw.fog_scale = detail::ranged(uw.fog_scale, 0.25f, 4.0f, 1.0f);
    uw.band = uw.band == uw.band ? (uw.band > 0.0f ? detail::ranged(uw.band, 0.05f, 2.0f, 0.0f) : 0.0f) : 0.0f;
    // M-SUNSHAFTS 45b: the sun light shafts. Off for the tier or r.sun_shafts 0; the ranges clamp, NaN -> the default.
    SunShafts& ss = post.shafts;
    ss.enabled = ss.enabled && q.sun_shafts && cv_sun_shafts.get_bool();
    ss.intensity = detail::ranged(ss.intensity, 0.0f, 4.0f, 1.0f);
    ss.length = detail::ranged(ss.length, 0.1f, 1.0f, 0.85f);
    ss.decay = detail::ranged(ss.decay, 0.8f, 1.0f, 0.97f);
    ss.threshold = detail::ranged(ss.threshold, 0.05f, 16.0f, 1.0f);
    ss.cap = detail::ranged(ss.cap, 0.0f, 8.0f, 1.5f);
    ss.visibility = detail::ranged(ss.visibility, 0.0f, 1.0f, 1.0f);
    ss.scatter_m = detail::ranged(ss.scatter_m, 5.0f, 500.0f, 60.0f);
    // M-CLOUDSHADOW 51b: the cloud shadows. Off for the tier or r.cloud_shadows 0; the ranges clamp, NaN -> the default.
    CloudShadow& cs = post.cloud_shadow;
    cs.enabled = cs.enabled && q.cloud_shadows && cv_cloud_shadows.get_bool();
    cs.strength = detail::ranged(cs.strength, 0.0f, 1.0f, 0.6f);
    cs.softness = detail::ranged(cs.softness, 0.0f, 1.0f, 0.3f);
    // M-CAUSTICS 52a: the caustics. Off for the tier or r.caustics 0; the ranges clamp, NaN -> the default.
    Caustics& ca = post.caustics;
    ca.enabled = ca.enabled && q.caustics && cv_caustics.get_bool();
    ca.strength = detail::ranged(ca.strength, 0.0f, 4.0f, 1.0f);
    ca.scale = detail::ranged(ca.scale, 0.25f, 16.0f, 2.0f);
    ca.speed = detail::ranged(ca.speed, 0.0f, 4.0f, 1.0f);
    ca.absorption = detail::ranged(ca.absorption, 0.0f, 4.0f, 0.15f);
    ca.max_depth = detail::ranged(ca.max_depth, 0.5f, 200.0f, 30.0f);
    // M-DYNRES 46b: dynamic resolution. Off for the tier or r.dynres 0; the ranges clamp, NaN -> the default.
    DynamicResolution& dr = post.dynres;
    dr.enabled = dr.enabled && q.dynres && cv_dynres.get_bool();
    dr.target_ms = detail::ranged(dr.target_ms, 4.0f, 100.0f, 14.0f);
    dr.min_scale = detail::ranged(dr.min_scale, 0.5f, 1.0f, 0.6f);
    dr.max_scale = detail::ranged(dr.max_scale, dr.min_scale, 1.0f, 1.0f);
    dr.headroom = detail::ranged(dr.headroom, 0.05f, 0.5f, 0.15f);
    // M-OUTLINE 39b: the edge outline. Off for the tier or r.edge_outline 0; the ranges clamp, NaN -> the default.
    EdgeOutline& eo = post.outline;
    eo.enabled = eo.enabled && q.edge_outline && cv_edge_outline.get_bool();
    eo.color.x = detail::ranged(eo.color.x, 0.0f, 16.0f, 0.02f);
    eo.color.y = detail::ranged(eo.color.y, 0.0f, 16.0f, 0.02f);
    eo.color.z = detail::ranged(eo.color.z, 0.0f, 16.0f, 0.025f);
    eo.opacity = detail::ranged(eo.opacity, 0.0f, 1.0f, 1.0f);
    eo.thickness = detail::ranged(eo.thickness, 0.5f, 4.0f, 1.0f);
    eo.depth_threshold = detail::ranged(eo.depth_threshold, 0.001f, 1.0f, 0.05f);
    eo.normal_threshold = detail::ranged(eo.normal_threshold, 0.01f, 2.0f, 0.4f);
    eo.depth_weight = detail::ranged(eo.depth_weight, 0.0f, 1.0f, 1.0f);
    eo.normal_weight = detail::ranged(eo.normal_weight, 0.0f, 1.0f, 1.0f);
    eo.fade_start = detail::ranged(eo.fade_start, 0.0f, 10000.0f, 30.0f);
    eo.fade_end = detail::ranged(eo.fade_end, 0.0f, 10000.0f, 80.0f);
    if (eo.fade_end < eo.fade_start) eo.fade_end = eo.fade_start;
}

namespace {

constexpr f32 kPi = 3.14159265359f;

// Wraps an angle to [-pi, pi]; NaN or infinite -> 0.
f32 wrap_angle(f32 a) {
    if (!std::isfinite(a)) return 0.0f;
    f32 r = std::fmod(a, 2.0f * kPi);
    if (r > kPi) r -= 2.0f * kPi;
    if (r < -kPi) r += 2.0f * kPi;
    return r;
}

f32 finite_or_zero(f32 x) { return std::isfinite(x) ? x : 0.0f; }

}  // namespace

void apply_quality(const QualitySettings& q, PointLight& light) {
    if (!q.light_cookies || !cv_light_cookies.get_bool()) light.cookie = ~0u;
    light.cookie_strength = detail::ranged(light.cookie_strength, 0.0f, 1.0f, 1.0f);
    light.cookie_edge = detail::ranged(light.cookie_edge, 0.0f, 1.0f, 0.0f);
    light.cookie_roll = wrap_angle(light.cookie_roll);
}

void apply_quality(const QualitySettings& q, SunCookie& cookie) {
    if (!q.light_cookies || !cv_light_cookies.get_bool()) cookie.texture = ~0u;
    cookie.strength = detail::ranged(cookie.strength, 0.0f, 1.0f, 1.0f);
    cookie.size = detail::ranged(cookie.size, 0.5f, 1000.0f, 20.0f);
    cookie.offset.x = finite_or_zero(cookie.offset.x);
    cookie.offset.y = finite_or_zero(cookie.offset.y);
    cookie.roll = wrap_angle(cookie.roll);
}

void apply_quality(const QualitySettings& q, GiVolume& gi) {
    gi.updates = gi.updates && q.gi_updates;
    gi.max_offset = detail::ranged(gi.max_offset, 0.0f, 0.45f, 0.4f);
    gi.follow = gi.follow && gi.updates;
}

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

Cirrus apply_quality(const QualitySettings& q, const Cirrus& cirrus) {
    Cirrus out = cirrus;
    out.coverage = q.cirrus && cv_cirrus.get_bool() ? detail::ranged(cirrus.coverage, 0.0f, 1.0f, 0.0f) : 0.0f;
    out.density = detail::ranged(cirrus.density, 0.0f, 1.0f, 0.35f);
    out.scale_m = detail::ranged(cirrus.scale_m, 200.0f, 40000.0f, 3000.0f);
    out.stretch = detail::ranged(cirrus.stretch, 1.0f, 16.0f, 6.0f);
    out.speed = detail::ranged(cirrus.speed, 0.0f, 10.0f, 2.0f);
    out.wind_dir = detail::unit_wind_dir(cirrus.wind_dir);
    out.height_m = detail::ranged(cirrus.height_m, 1000.0f, 30000.0f, 9000.0f);
    return out;
}

Halo apply_quality(const QualitySettings& q, const Halo& halo) {
    Halo out = halo;
    out.strength = q.halo && cv_halo.get_bool() ? detail::ranged(halo.strength, 0.0f, 1.0f, 0.0f) : 0.0f;
    out.sun_dogs = detail::ranged(halo.sun_dogs, 0.0f, 1.0f, 1.0f);
    out.moon = detail::ranged(halo.moon, 0.0f, 1.0f, 0.5f);
    out.dispersion = detail::ranged(halo.dispersion, 0.0f, 1.0f, 1.0f);
    return out;
}

NightSky apply_quality(const QualitySettings& q, const NightSky& sky) {
    NightSky out = sky;
    const bool on = cv_nightsky.get_bool();
    out.moon_dir = detail::unit_dir3(sky.moon_dir);
    out.moon_cos_size = detail::moon_cos_size(sky.moon_cos_size);
    out.moon_intensity = on ? detail::scaled(sky.moon_intensity, 8.0f, 0.0f) : 0.0f;
    out.moon_color = math::vec3{detail::scaled(sky.moon_color.x, 1.0f, 0.0f), detail::scaled(sky.moon_color.y, 1.0f, 0.0f),
                                detail::scaled(sky.moon_color.z, 1.0f, 0.0f)};
    out.moon_phase = unit01(sky.moon_phase);
    out.star_density = on && q.night_sky ? unit01(sky.star_density) : 0.0f;
    out.star_brightness = detail::scaled(sky.star_brightness, 4.0f, 1.0f);
    out.star_twinkle = q.night_sky ? unit01(sky.star_twinkle) : 0.0f;
    out.star_time = std::isfinite(sky.star_time) ? sky.star_time : 0.0f;
    out.night_fade = unit01(sky.night_fade);
    return out;
}

WaterSurface apply_quality(const QualitySettings& q, const WaterSurface& water) {
    WaterSurface out = water;
    const auto unit = [](f32 x, f32 nan_to) { return detail::ranged(x, 0.0f, 1.0f, nan_to); };
    out.enabled = water.enabled && cv_water.get_bool();
    out.level_y = detail::ranged(water.level_y, -1000.0f, 1000.0f, 0.0f);
    out.half_extent = detail::ranged(water.half_extent, 1.0f, 20000.0f, 500.0f);
    const f32 count = detail::ranged(static_cast<f32>(water.wave_count), 1.0f, 8.0f, 4.0f);
    out.wave_count = static_cast<i32>(count);
    out.amplitude = q.water ? detail::ranged(water.amplitude, 0.0f, 10.0f, 0.0f) : 0.0f;
    out.wavelength = detail::ranged(water.wavelength, 1.0f, 500.0f, 24.0f);
    out.steepness = unit(water.steepness, 0.0f);
    out.wind_dir = detail::unit_wind_dir(water.wind_dir);
    out.time = std::isfinite(water.time) ? water.time : 0.0f;
    out.speed_scale = detail::ranged(water.speed_scale, 0.0f, 4.0f, 1.0f);
    out.shallow_color = math::vec3{unit(water.shallow_color.x, 0.0f), unit(water.shallow_color.y, 0.0f),
                                   unit(water.shallow_color.z, 0.0f)};
    out.deep_color = math::vec3{unit(water.deep_color.x, 0.0f), unit(water.deep_color.y, 0.0f),
                                unit(water.deep_color.z, 0.0f)};
    out.depth_scale_m = detail::ranged(water.depth_scale_m, 0.1f, 200.0f, 8.0f);
    out.reflectivity = unit(water.reflectivity, 1.0f);
    out.roughness = unit(water.roughness, 0.08f);
    out.foam_amount = q.water ? unit(water.foam_amount, 0.0f) : 0.0f;
    out.foam_width_m = detail::ranged(water.foam_width_m, 0.05f, 20.0f, 1.5f);
    out.flow = math::vec2{detail::ranged(water.flow.x, -5.0f, 5.0f, 0.0f), detail::ranged(water.flow.y, -5.0f, 5.0f, 0.0f)};
    return out;
}

AuroraSky apply_quality(const QualitySettings& q, const AuroraSky& aurora) {
    AuroraSky out = aurora;
    const auto unit = [](f32 x) { return detail::ranged(x, 0.0f, 1.0f, 0.0f); };
    out.level = q.aurora && cv_aurora.get_bool() ? detail::ranged(aurora.level, 0.0f, 4.0f, 0.0f) : 0.0f;
    out.base_height = detail::ranged(aurora.base_height, 0.02f, 0.6f, 0.18f);
    out.height_span = detail::ranged(aurora.height_span, 0.05f, 0.8f, 0.35f);
    out.azimuth = detail::unit_dir2_z(aurora.azimuth);
    out.half_width_cos = detail::ranged(aurora.half_width_cos, -1.0f, 1.0f, 0.8192f);
    const f32 count = detail::ranged(static_cast<f32>(aurora.curtain_count), 1.0f, 6.0f, 3.0f);
    out.curtain_count = static_cast<i32>(count);
    out.time = std::isfinite(aurora.time) ? aurora.time : 0.0f;
    out.speed = detail::ranged(aurora.speed, 0.0f, 4.0f, 1.0f);
    out.low_color = math::vec3{unit(aurora.low_color.x), unit(aurora.low_color.y), unit(aurora.low_color.z)};
    out.high_color = math::vec3{unit(aurora.high_color.x), unit(aurora.high_color.y), unit(aurora.high_color.z)};
    return out;
}

RainbowSky apply_quality(const QualitySettings& q, const RainbowSky& rainbow) {
    RainbowSky out = rainbow;
    out.level = q.rainbow && cv_rainbow.get_bool() ? detail::ranged(rainbow.level, 0.0f, 4.0f, 0.0f) : 0.0f;
    out.band_width = detail::ranged(rainbow.band_width, 0.5f, 2.0f, 1.0f);
    out.secondary = detail::ranged(rainbow.secondary, 0.0f, 1.0f, 0.0f);
    out.anti_solar = detail::unit_dir3_or(rainbow.anti_solar, math::vec3{0.0f, 0.0f, 1.0f});
    return out;
}

MeteorSky apply_quality(const QualitySettings& q, const MeteorSky& meteor) {
    MeteorSky out = meteor;
    out.level = q.meteor && cv_meteor.get_bool() ? detail::ranged(meteor.level, 0.0f, 4.0f, 0.0f) : 0.0f;
    out.rate = detail::ranged(meteor.rate, 0.0f, 30.0f, 4.0f);
    out.brightness = detail::ranged(meteor.brightness, 0.0f, 4.0f, 1.0f);
    out.length = detail::ranged(meteor.length, 0.5f, 2.0f, 1.0f);
    out.speed = detail::ranged(meteor.speed, 0.5f, 2.0f, 1.0f);
    out.time = std::isfinite(meteor.time) ? detail::ranged(meteor.time, 0.0f, 60.0f, 0.0f) : 0.0f;
    out.seed = meteor.seed < 65535u ? meteor.seed : 65535u;
    return out;
}

void apply_quality(const QualitySettings& q, Volumetrics& v) {
    v.enabled = v.enabled && q.volumetrics;
    v.tile = v.tile > q.volumetric_tile ? v.tile : q.volumetric_tile;
    v.slices = v.slices < q.volumetric_slices ? v.slices : q.volumetric_slices;
}

}  // namespace mce::render
