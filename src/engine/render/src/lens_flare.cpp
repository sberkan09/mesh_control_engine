#include "mce/render/lens_flare.h"

#include <cmath>

#include "mce/render/scene3d.h"
#include "unit_clamp.h"

namespace mce::render {

// The header declares the ghost array with a literal bound (it cannot include the wide scene3d.h).
static_assert(kLensFlareMaxGhosts == 6, "update LensFlareFrame::ghosts in lens_flare.h");

namespace {

constexpr usize kGhosts = 6;

// Ghost k: position along the sun-to-centre axis (1 = the sun, 0 = the screen centre, negative = the far side),
// radius (fraction of the view height), intensity and tint.
constexpr f32 kGhostT[kGhosts] = {-0.35f, -0.7f, -1.1f, 0.45f, -1.5f, 1.25f};
constexpr f32 kGhostRadius[kGhosts] = {0.05f, 0.09f, 0.04f, 0.07f, 0.12f, 0.06f};
constexpr f32 kGhostIntensity[kGhosts] = {0.30f, 0.20f, 0.25f, 0.15f, 0.10f, 0.12f};
constexpr math::vec3 kGhostTint[kGhosts] = {
    {1.00f, 0.55f, 0.35f}, {0.40f, 0.80f, 1.00f}, {0.60f, 1.00f, 0.50f},
    {1.00f, 0.40f, 0.70f}, {0.50f, 0.60f, 1.00f}, {1.00f, 0.85f, 0.40f},
};

f32 smooth_step(f32 a, f32 b, f32 x) {
    const f32 t = detail::ranged((x - a) / (b - a), 0.0f, 1.0f, 0.0f);
    return t * t * (3.0f - 2.0f * t);
}

bool finite2(math::vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }
bool finite3(math::vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

bool finite_matrix(const math::mat4& m) {
    for (usize c = 0; c < 4; ++c)
        for (usize r = 0; r < 4; ++r)
            if (!std::isfinite(m.c[c][r])) return false;
    return true;
}

}  // namespace

bool lens_flare_project(const math::mat4& view_proj, math::vec3 toward_sun, math::vec2& uv) {
    if (!finite3(toward_sun) || !finite_matrix(view_proj)) return false;
    const math::vec4 clip = view_proj * math::vec4(toward_sun, 0.0f);
    if (!(clip.w > 0.0f)) return false;
    const math::vec2 out{clip.x / clip.w * 0.5f + 0.5f, clip.y / clip.w * 0.5f + 0.5f};
    if (!finite2(out)) return false;
    uv = out;
    return true;
}

f32 lens_flare_horizon_fade(math::vec3 toward_sun) {
    if (!std::isfinite(toward_sun.y)) return 0.0f;
    return smooth_step(-0.02f, 0.04f, toward_sun.y);
}

f32 lens_flare_edge_fade(math::vec2 sun_uv, f32 edge_fade) {
    if (!finite2(sun_uv)) return 0.0f;
    const f32 width = detail::ranged(edge_fade, 0.01f, 0.5f, 0.1f);
    f32 d = sun_uv.x < 1.0f - sun_uv.x ? sun_uv.x : 1.0f - sun_uv.x;
    const f32 dy = sun_uv.y < 1.0f - sun_uv.y ? sun_uv.y : 1.0f - sun_uv.y;
    if (dy < d) d = dy;
    return smooth_step(0.0f, width, d);
}

u32 lens_flare_ghost_layout(const LensFlare& flare, math::vec2 sun_uv, f32 /*aspect*/, LensFlareGhost* out) {
    const u32 count = flare.ghost_count < kLensFlareMaxGhosts ? flare.ghost_count : kLensFlareMaxGhosts;
    if (out == nullptr) return count;
    const f32 spacing = detail::ranged(flare.ghost_spacing, 0.25f, 2.0f, 1.0f);
    const f32 gain = detail::ranged(flare.ghost_intensity, 0.0f, 4.0f, 1.0f);
    const f32 chroma = detail::ranged(flare.ghost_chroma, 0.0f, 1.0f, 0.5f);
    for (u32 k = 0; k < count; ++k) {
        LensFlareGhost& g = out[k];
        const f32 t = kGhostT[k] * spacing;
        g.center = math::vec2{0.5f + (sun_uv.x - 0.5f) * t, 0.5f + (sun_uv.y - 0.5f) * t};
        g.radius = kGhostRadius[k];
        g.intensity = kGhostIntensity[k] * gain;
        const math::vec3 tint = kGhostTint[k];
        g.tint = math::vec3{1.0f + (tint.x - 1.0f) * chroma, 1.0f + (tint.y - 1.0f) * chroma,
                            1.0f + (tint.z - 1.0f) * chroma};
        g.pad_ = 0.0f;
    }
    return count;
}

LensFlareFrame lens_flare_frame(const LensFlare& flare, const math::mat4& view_proj, math::vec3 toward_sun, f32 aspect,
                                bool starburst_allowed) {
    LensFlareFrame frame;
    frame.aspect = std::isfinite(aspect) && aspect > 0.0f ? aspect : 1.0f;
    if (!flare.enabled) return frame;

    // A NaN or infinite intensity or visibility switches the flare off (strength exactly 0); both are clamped.
    const f32 intensity = flare.intensity > 0.0f ? (flare.intensity < 4.0f ? flare.intensity : 4.0f) : 0.0f;
    const f32 visibility = flare.visibility > 0.0f ? (flare.visibility < 1.0f ? flare.visibility : 1.0f) : 0.0f;
    if (!(intensity > 0.0f) || !(visibility > 0.0f)) return frame;

    if (!finite3(toward_sun)) return frame;
    const f32 len2 = toward_sun.x * toward_sun.x + toward_sun.y * toward_sun.y + toward_sun.z * toward_sun.z;
    if (!(len2 >= 1.0e-12f) || !std::isfinite(len2)) return frame;
    const f32 len = std::sqrt(len2);
    const math::vec3 dir{toward_sun.x / len, toward_sun.y / len, toward_sun.z / len};

    math::vec2 uv;
    if (!lens_flare_project(view_proj, dir, uv)) return frame;
    const f32 horizon = lens_flare_horizon_fade(dir);
    const f32 edge = lens_flare_edge_fade(uv, flare.edge_fade);
    const f32 strength = intensity * visibility * horizon * edge;
    if (!(strength > 0.0f) || !std::isfinite(strength)) return frame;

    frame.sun_uv = uv;
    frame.strength = strength;
    frame.halo = detail::ranged(flare.halo, 0.0f, 4.0f, 1.0f);
    frame.halo_radius = detail::ranged(flare.halo_radius, 0.02f, 0.6f, 0.25f);
    frame.starburst = starburst_allowed ? detail::ranged(flare.starburst, 0.0f, 4.0f, 0.0f) : 0.0f;
    frame.rays = flare.starburst_rays < 2u ? 2u : (flare.starburst_rays > 16u ? 16u : flare.starburst_rays);
    frame.occlusion_radius = detail::ranged(flare.occlusion_radius, 0.002f, 0.1f, 0.015f);
    frame.ghost_count = lens_flare_ghost_layout(flare, uv, frame.aspect, frame.ghosts);
    return frame;
}

}  // namespace mce::render
