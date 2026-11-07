#include "mce/render/sun_shafts.h"

#include <cmath>

#include "mce/render/scene3d.h"
#include "unit_clamp.h"

namespace mce::render {

namespace {

u32 mix(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

bool finite3(math::vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

bool finite_matrix(const math::mat4& m) {
    for (usize c = 0; c < 4; ++c)
        for (usize r = 0; r < 4; ++r)
            if (!std::isfinite(m.c[c][r])) return false;
    return true;
}

f32 luma(math::vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

f32 saturate(f32 t) { return t > 0.0f ? (t < 1.0f ? t : 1.0f) : 0.0f; }

f32 smooth_between(f32 lo, f32 hi, f32 x) {
    const f32 t = saturate((x - lo) / (hi - lo));
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

bool sun_shafts_project(const math::mat4& view_proj, math::vec3 toward_sun, math::vec2& uv, f32& facing) {
    if (!finite3(toward_sun) || !finite_matrix(view_proj)) return false;
    const f32 len2 = toward_sun.x * toward_sun.x + toward_sun.y * toward_sun.y + toward_sun.z * toward_sun.z;
    if (!(len2 >= 1.0e-12f) || len2 > 3.0e38f) return false;
    const f32 inv = 1.0f / std::sqrt(len2);
    const math::vec3 d{toward_sun.x * inv, toward_sun.y * inv, toward_sun.z * inv};
    const math::vec4 clip = view_proj * math::vec4(d, 0.0f);  // a direction: w = 0 drops the translation
    if (!(clip.w > 0.0f)) return false;
    const f32 u = clip.x / clip.w * 0.5f + 0.5f;
    const f32 v = clip.y / clip.w * 0.5f + 0.5f;
    if (!(u == u) || !(v == v)) return false;
    uv = math::vec2{detail::ranged(u, -kSunShaftsMaxUv, kSunShaftsMaxUv, 0.5f),
                    detail::ranged(v, -kSunShaftsMaxUv, kSunShaftsMaxUv, 0.5f)};
    facing = clip.w;
    return true;
}

f32 sun_shafts_elevation_fade(math::vec3 toward_sun, f32 high_floor) {
    const f32 y = toward_sun.y;
    if (!std::isfinite(y)) return 0.0f;
    const f32 fl = detail::ranged(high_floor, 0.0f, 1.0f, kSunShaftsHighFloor);
    const f32 hi = smooth_between(0.35f, 0.9f, y);
    return smooth_between(-0.02f, 0.04f, y) * (1.0f + (fl - 1.0f) * hi);
}

f32 sun_shafts_view_fade(math::vec2 sun_uv, f32 facing) {
    if (!std::isfinite(sun_uv.x) || !std::isfinite(sun_uv.y) || !std::isfinite(facing)) return 0.0f;
    const f32 dx = sun_uv.x < 0.0f ? -sun_uv.x : (sun_uv.x > 1.0f ? sun_uv.x - 1.0f : 0.0f);
    const f32 dy = sun_uv.y < 0.0f ? -sun_uv.y : (sun_uv.y > 1.0f ? sun_uv.y - 1.0f : 0.0f);
    const f32 border = std::sqrt(dx * dx + dy * dy);
    return smooth_between(0.0f, 0.3f, facing) * (1.0f - smooth_between(0.0f, 0.75f, border));
}

f32 sun_shafts_medium(const Fog& fog, const Volumetrics&, bool volumetrics_drawn) {
    if (!std::isfinite(fog.density)) return 0.0f;
    const f32 d = (fog.density > 0.0f ? fog.density : 0.0f) * 40.0f;
    const f32 e = d * d;
    const f32 haze = e < 1.0e4f ? 1.0f - std::exp(-e) : 1.0f;
    const f32 m = 0.35f + 0.65f * haze;
    return volumetrics_drawn ? m * kSunShaftsVolumeShare : m;
}

math::vec3 sun_shafts_tint(math::vec3 sun_color, math::vec3 fog_color, f32 fog_amount) {
    if (!finite3(sun_color) || !finite3(fog_color) || !std::isfinite(fog_amount)) return math::vec3{0.0f, 0.0f, 0.0f};
    const f32 l = luma(fog_color);
    const f32 inv = 1.0f / (l > 1.0e-3f ? l : 1.0e-3f);
    const f32 k = 0.5f * saturate(fog_amount);
    return math::vec3{sun_color.x * (1.0f + (fog_color.x * inv - 1.0f) * k),
                      sun_color.y * (1.0f + (fog_color.y * inv - 1.0f) * k),
                      sun_color.z * (1.0f + (fog_color.z * inv - 1.0f) * k)};
}

f32 sun_shafts_soft_clamp(f32 x, f32 cap) {
    if (!(cap > 0.0f) || !(x == x)) return 0.0f;
    const f32 xc = x > 0.0f ? x : 0.0f;
    return cap * (1.0f - std::exp(-xc / cap));
}

u32 sun_shafts_samples(u32 detail) { return detail == 0 ? 0u : (detail == 1 ? 16u : 32u); }

u32 sun_shafts_hash(u32 x, u32 y, u32 seed) {
    u32 h = mix(x);
    h = mix(h ^ (y * 0x85ebca6bu));
    h = mix(h ^ (seed * 0xc2b2ae35u));
    return h;
}

bool sun_shafts_frame(const SunShafts& setting, const DirectionalLight& sun, const Fog& fog, const Volumetrics& volumetrics,
                      bool volumetrics_drawn, const math::mat4& view_proj, f32 aspect, u32 detail, f32 submersion,
                      SunShaftsFrame& out) {
    if (!setting.enabled) return false;
    const f32 intensity = detail::ranged(setting.intensity, 0.0f, 4.0f, 1.0f);
    const f32 visibility = detail::ranged(setting.visibility, 0.0f, 1.0f, 1.0f);
    if (!(intensity > 0.0f) || !(visibility > 0.0f)) return false;
    const u32 samples = sun_shafts_samples(detail);
    if (samples == 0) return false;
    if (!std::isfinite(submersion) || !finite3(sun.direction) || !finite3(sun.color) || !finite3(fog.color) ||
        !std::isfinite(fog.density) || !finite_matrix(view_proj))
        return false;
    const f32 len2 = sun.direction.x * sun.direction.x + sun.direction.y * sun.direction.y + sun.direction.z * sun.direction.z;
    if (!(len2 >= 1.0e-12f) || len2 > 3.0e38f) return false;
    const f32 inv = 1.0f / std::sqrt(len2);
    const math::vec3 toward{-sun.direction.x * inv, -sun.direction.y * inv, -sun.direction.z * inv};

    math::vec2 uv;
    f32 facing = 0.0f;
    if (!sun_shafts_project(view_proj, toward, uv, facing)) return false;
    const f32 elevation = sun_shafts_elevation_fade(toward, kSunShaftsHighFloor);
    const f32 view = sun_shafts_view_fade(uv, facing);
    const f32 medium = sun_shafts_medium(fog, volumetrics, volumetrics_drawn);
    const f32 strength = elevation * view * medium * visibility * (1.0f - saturate(submersion));
    if (!std::isfinite(strength) || !(strength > 0.0f)) return false;

    // How hazy the air is (the medium factor's own term) decides how far the fog's hue reaches into the rays.
    const f32 d = (fog.density > 0.0f ? fog.density : 0.0f) * 40.0f;
    const f32 fog_amount = d * d < 1.0e4f ? 1.0f - std::exp(-d * d) : 1.0f;
    const math::vec3 tint = sun_shafts_tint(sun.color, fog.color, fog_amount);
    const f32 k = intensity * strength;

    SunShaftsFrame f;
    f.sun_uv = uv;
    f.aspect = (std::isfinite(aspect) && aspect > 0.0f) ? aspect : 1.0f;
    f.length = detail::ranged(setting.length, 0.1f, 1.0f, 0.85f);
    f.decay = detail::ranged(setting.decay, 0.8f, 1.0f, 0.97f);
    f.threshold = detail::ranged(setting.threshold, 0.05f, 16.0f, 1.0f);
    f.cap = detail::ranged(setting.cap, 0.0f, 8.0f, 1.5f);
    f.samples = samples;
    f.color = math::vec3{tint.x * k, tint.y * k, tint.z * k};
    f.scatter_m = detail::ranged(setting.scatter_m, 5.0f, 500.0f, 60.0f);
    f.strength = strength;
    out = f;
    return true;
}

}  // namespace mce::render
