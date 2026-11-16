#include "mce/render/cloud_shadow.h"

#include <cmath>

#include "unit_clamp.h"

namespace mce::render {

namespace {

// pcg2d (Jarzynski and Olano 2020), one lane: kSkyFS's cloud_hash.
u32 cloud_hash(u32 x, u32 y) {
    x = x * 1664525u + 1013904223u;
    y = y * 1664525u + 1013904223u;
    x += y * 1664525u;
    y += x * 1664525u;
    x ^= x >> 16u;
    y ^= y >> 16u;
    x += y * 1664525u;
    y += x * 1664525u;
    x ^= x >> 16u;
    return x;
}

f32 lattice(u32 cx, u32 cy, u32 seed) { return f32(cloud_hash(cx + seed, cy) >> 8u) * (1.0f / 16777216.0f); }

f32 mixf(f32 a, f32 b, f32 t) { return a * (1.0f - t) + b * t; }

// kSkyFS's cloud_value: g in 0..m cells, m lattice cells per period.
f32 value(f32 gx, f32 gy, u32 m, u32 seed) {
    const f32 ix = std::floor(gx), iy = std::floor(gy);
    f32 fx = gx - ix, fy = gy - iy;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    const u32 x0 = u32(ix > 0.0f ? ix : 0.0f) % m, y0 = u32(iy > 0.0f ? iy : 0.0f) % m;  // g may round up to m: wraps to 0
    const u32 x1 = (x0 + 1u) % m, y1 = (y0 + 1u) % m;
    const f32 a = lattice(x0, y0, seed), b = lattice(x1, y0, seed);
    const f32 c = lattice(x0, y1, seed), e = lattice(x1, y1, seed);
    return mixf(mixf(a, b, fx), mixf(c, e, fx), fy);
}

f32 saturate(f32 t) { return t > 0.0f ? (t < 1.0f ? t : 1.0f) : 0.0f; }

f32 smooth_between(f32 lo, f32 hi, f32 x) {
    const f32 t = saturate((x - lo) / (hi - lo));
    return t * t * (3.0f - 2.0f * t);
}

bool finite3(math::vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// GLSL mod(x, y) for y > 0.
f32 glsl_mod(f32 x, f32 y) { return x - y * std::floor(x / y); }

}  // namespace

bool cloud_shadow_point(math::vec3 world, math::vec3 toward_sun, math::vec3 cam, f32 height_m, math::vec2& plane_xz) {
    if (!finite3(world) || !finite3(toward_sun) || !finite3(cam) || !std::isfinite(height_m) || !(height_m > 0.0f))
        return false;
    const f32 len2 = toward_sun.x * toward_sun.x + toward_sun.y * toward_sun.y + toward_sun.z * toward_sun.z;
    if (!(len2 >= 1.0e-12f) || len2 > 3.0e38f) return false;
    const f32 inv = 1.0f / std::sqrt(len2);
    const math::vec3 d{toward_sun.x * inv, toward_sun.y * inv, toward_sun.z * inv};
    if (!(d.y >= kCloudShadowMinSunY)) return false;
    const f32 rise = (cam.y + height_m) - world.y;  // how far the point is below the plane
    if (!(rise > 0.0f)) return false;
    const f32 t = rise / d.y;
    const f32 x = (world.x + d.x * t) - cam.x, z = (world.z + d.z * t) - cam.z;
    if (!std::isfinite(x) || !std::isfinite(z)) return false;
    plane_xz = math::vec2{x, z};
    return true;
}

math::vec2 cloud_shadow_drift(f32 time, f32 speed, f32 scale_m, math::vec2 wind_dir) {
    if (!std::isfinite(time)) time = 0.0f;
    const f32 sc = detail::cloud_scale(scale_m);
    const math::vec2 dir = detail::unit_wind_dir(wind_dir);
    const f64 cells = f64(time) * f64(detail::cloud_speed(speed)) * 10.0 / f64(sc);
    const auto wrap = [](f64 x) { return f32(x - f64(kCloudShadowPeriod) * std::floor(x / f64(kCloudShadowPeriod))); };
    return math::vec2{wrap(cells * f64(dir.x)), wrap(cells * f64(dir.y))};
}

f32 cloud_noise(math::vec2 plane_xz, const CloudDensityParams& p) {
    if (!std::isfinite(plane_xz.x) || !std::isfinite(plane_xz.y) || !std::isfinite(p.drift_cells.x) ||
        !std::isfinite(p.drift_cells.y) || p.octaves == 0u)
        return 0.0f;
    const f32 inv_cell = 1.0f / detail::cloud_scale(p.scale_m);
    const f32 qx = glsl_mod(plane_xz.x * inv_cell - p.drift_cells.x, kCloudShadowPeriod);
    const f32 qy = glsl_mod(plane_xz.y * inv_cell - p.drift_cells.y, kCloudShadowPeriod);
    if (!std::isfinite(qx) || !std::isfinite(qy)) return 0.0f;
    f32 n = value(qx, qy, 64u, 0x27D4EB2Fu) * 0.55f;
    f32 w = 0.55f;
    if (p.octaves >= 2u) {
        n += value(qx * 2.0f, qy * 2.0f, 128u, 0x165667B1u) * 0.30f;
        w += 0.30f;
    }
    if (p.octaves >= 3u) {
        n += value(qx * 4.0f, qy * 4.0f, 256u, 0xD3A2646Cu) * 0.15f;
        w += 0.15f;
    }
    return p.octaves >= 3u ? n : n / w;
}

f32 cloud_density(math::vec2 plane_xz, const CloudDensityParams& p) {
    const f32 coverage = detail::unit01(p.coverage);
    if (!(coverage > 0.0f)) return 0.0f;
    const f32 n = cloud_noise(plane_xz, p);
    const f32 t = 0.78f - 0.6f * coverage;
    const f32 edge = kCloudShadowEdge + kCloudShadowSoftEdge * detail::unit01(p.softness);
    const f32 mask = smooth_between(t, t + edge, n);
    return mask * (0.55f + 0.45f * detail::unit01(p.density));
}

f32 cloud_shadow_factor(f32 density, f32 strength, f32 sun_share, f32 sun_elevation) {
    if (!std::isfinite(density) || !std::isfinite(strength) || !std::isfinite(sun_share) || !std::isfinite(sun_elevation))
        return 1.0f;
    const f32 fade = smooth_between(kCloudShadowMinSunY, 0.25f, sun_elevation);
    return 1.0f - detail::unit01(strength) * detail::unit01(density) * detail::unit01(sun_share) * fade;
}

f32 cloud_shadow_sun_share(math::vec3 sun_color, f32 intensity, math::vec3 ambient_sky) {
    if (!finite3(sun_color) || !finite3(ambient_sky) || !std::isfinite(intensity)) return 0.0f;
    const auto pos = [](f32 x) { return x > 0.0f ? x : 0.0f; };
    const auto luma = [&](math::vec3 c) { return 0.2126f * pos(c.x) + 0.7152f * pos(c.y) + 0.0722f * pos(c.z); };
    const f32 sun = luma(sun_color) * pos(intensity);
    if (!(sun > 0.0f) || !std::isfinite(sun)) return 0.0f;
    const f32 total = sun + luma(ambient_sky);
    if (!std::isfinite(total)) return 0.0f;
    return detail::unit01(sun / total);
}

bool cloud_shadow_perspective(const math::mat4& proj) {
    if (proj.c[3][3] != 0.0f) return false;
    const f32 p00 = proj.c[0][0], p11 = proj.c[1][1];
    if (!std::isfinite(p00) || !std::isfinite(p11) || !std::isfinite(proj.c[2][0]) || !std::isfinite(proj.c[2][1]))
        return false;
    return std::fabs(p00) > 1.0e-6f && std::fabs(p11) > 1.0e-6f;
}

f32 cloud_shadow_fog_visibility(f32 fog_density, f32 distance) {
    if (!std::isfinite(fog_density) || !std::isfinite(distance) || fog_density == 0.0f) return 1.0f;
    const f32 fd = fog_density * distance;
    return std::exp(-fd * fd);
}

math::vec3 cloud_shadow_shade(math::vec3 pixel, f32 factor, f32 fog_visibility, math::vec3 fog_color) {
    const f32 f = std::isfinite(factor) ? saturate(factor) : 1.0f;
    if (!(fog_visibility < 1.0f)) return math::vec3{pixel.x * f, pixel.y * f, pixel.z * f};
    const f32 k = 1.0f - f, haze = 1.0f - (std::isfinite(fog_visibility) ? saturate(fog_visibility) : 1.0f);
    return math::vec3{pixel.x - k * (pixel.x - haze * fog_color.x), pixel.y - k * (pixel.y - haze * fog_color.y),
                      pixel.z - k * (pixel.z - haze * fog_color.z)};
}

}  // namespace mce::render
