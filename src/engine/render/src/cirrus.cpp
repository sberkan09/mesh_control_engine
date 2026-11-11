#include "mce/render/cirrus.h"

#include <cmath>
#include <limits>

#include "mce/render/scene3d.h"
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

// kSkyFS's cloud_lattice.
f32 lattice(u32 cx, u32 cy, u32 seed) { return f32(cloud_hash(cx + seed, cy) >> 8u) * (1.0f / 16777216.0f); }

// GLSL mix(a, b, t).
f32 mixf(f32 a, f32 b, f32 t) { return a * (1.0f - t) + b * t; }

// kSkyFS's cirrus_value: g in 0..m cells per axis (mx by my lattice cells per period).
f32 value(f32 gx, f32 gy, u32 mx, u32 my, u32 seed) {
    const f32 ix = std::floor(gx), iy = std::floor(gy);
    f32 fx = gx - ix, fy = gy - iy;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    const u32 x0 = u32(ix > 0.0f ? ix : 0.0f) % mx, y0 = u32(iy > 0.0f ? iy : 0.0f) % my;  // g may round up to m
    const u32 x1 = (x0 + 1u) % mx, y1 = (y0 + 1u) % my;
    const f32 a = lattice(x0, y0, seed), b = lattice(x1, y0, seed);
    const f32 c = lattice(x0, y1, seed), e = lattice(x1, y1, seed);
    return mixf(mixf(a, b, fx), mixf(c, e, fx), fy);
}

f32 saturate(f32 t) { return t > 0.0f ? (t < 1.0f ? t : 1.0f) : 0.0f; }

// GLSL smoothstep(lo, hi, x).
f32 smooth_between(f32 lo, f32 hi, f32 x) {
    const f32 t = saturate((x - lo) / (hi - lo));
    return t * t * (3.0f - 2.0f * t);
}

// GLSL mod(x, y) for y > 0.
f32 glsl_mod(f32 x, f32 y) { return x - y * std::floor(x / y); }

bool finite3(math::vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// The seeds of the two octaves (the low layer's are 0x27D4EB2F, 0x165667B1, 0xD3A2646C).
constexpr u32 kSeedBroad = 0x68E31DA4u;
constexpr u32 kSeedFibre = 0xB5297A4Du;

f32 scale_of(f32 m) { return detail::ranged(m, 200.0f, 40000.0f, 3000.0f); }
f32 stretch_of(f32 s) { return detail::ranged(s, 1.0f, 16.0f, 6.0f); }
f32 speed_of(f32 s) { return detail::ranged(s, 0.0f, 10.0f, 2.0f); }

}  // namespace

f32 cirrus_drift(f32 time, f32 speed, f32 scale_m, f32 stretch) {
    if (!std::isfinite(time)) time = 0.0f;
    const f64 cells =
        f64(time) * f64(speed_of(speed)) * f64(kCirrusDrift) / (f64(scale_of(scale_m)) * f64(stretch_of(stretch)));
    const f64 period = f64(kCirrusPeriod);
    const f64 w = cells - period * std::floor(cells / period);
    return w < period ? f32(w)
                      : 0.0f;  // f32 may round a value just under the period up to it: the shader's mod wraps it
}

CirrusParams cirrus_shader_params(const Cirrus& c, f32 time) {
    CirrusParams p;
    const f32 coverage = detail::unit01(c.coverage);
    if (!c.enabled || !(coverage > 0.0f)) return p;
    const f32 scale = scale_of(c.scale_m);
    const f32 stretch = stretch_of(c.stretch);
    p.coverage = coverage;
    p.density = detail::ranged(c.density, 0.0f, 1.0f, 0.35f);
    p.inv_cell = 1.0f / scale;
    p.height_m = detail::ranged(c.height_m, 1000.0f, 30000.0f, 9000.0f);
    p.wind = detail::unit_wind_dir(c.wind_dir);
    p.stretch = stretch;
    p.drift = cirrus_drift(time, c.speed, scale, stretch);
    return p;
}

bool cirrus_coords(math::vec3 d, const CirrusParams& p, math::vec2& q) {
    if (!finite3(d) || !(p.coverage > 0.0f) || !(d.y > kCirrusMinUp)) return false;
    if (!std::isfinite(p.height_m) || !std::isfinite(p.inv_cell) || !std::isfinite(p.stretch) || !(p.stretch > 0.0f) ||
        !std::isfinite(p.drift) || !std::isfinite(p.wind.x) || !std::isfinite(p.wind.y))
        return false;
    const f32 k = p.height_m / d.y;
    const f32 px = d.x * k * p.inv_cell, pz = d.z * k * p.inv_cell;
    const f32 along = (px * p.wind.x + pz * p.wind.y) / p.stretch - p.drift;
    const f32 across = px * -p.wind.y + pz * p.wind.x;
    const f32 qx = glsl_mod(along, kCirrusPeriod), qy = glsl_mod(across, kCirrusPeriod);
    if (!std::isfinite(qx) || !std::isfinite(qy)) return false;
    q = math::vec2{qx, qy};
    return true;
}

f32 cirrus_noise(math::vec2 q, f32 fine) {
    if (!std::isfinite(q.x) || !std::isfinite(q.y)) return 0.0f;
    const f32 b = value(q.x * 2.0f, q.y, 128u, 64u, kSeedBroad);
    const f32 s = value(q.x, q.y * 6.0f + 3.0f * b, 64u, 384u, kSeedFibre);
    return 0.55f * b + 0.45f * mixf(0.5f, s, saturate(fine));
}

f32 cirrus_fibre_pixels(math::vec3 d, const CirrusParams& p, f32 pix) {
    if (!(pix > 0.0f) || !std::isfinite(pix)) return std::numeric_limits<f32>::infinity();
    const f32 k = d.x * -p.wind.y + d.z * p.wind.x;
    const f32 r = 1.0f - k * k;
    return std::sqrt(r > 0.0f ? r : 0.0f) * d.y / (6.0f * p.height_m * p.inv_cell * pix);
}

f32 cirrus_opacity(math::vec3 d, const CirrusParams& p, f32 pix) {
    math::vec2 q;
    if (!cirrus_coords(d, p, q)) return 0.0f;
    const f32 n = cirrus_noise(q, smooth_between(1.5f, 4.0f, cirrus_fibre_pixels(d, p, pix)));
    const f32 t = 0.8f - 0.6f * p.coverage;
    return smooth_between(t, t + 0.2f, n) * detail::unit01(p.density) * kCirrusMaxOpacity *
           smooth_between(kCirrusMinUp, 0.2f, d.y);
}

math::vec3 cirrus_color(math::vec3 toward_sun, f32 cs, math::vec3 light, math::vec3 ambient_sky) {
    const f32 sun_up = smooth_between(-0.16f, 0.04f, toward_sun.y);
    const f32 warm = sun_up * (1.0f - smooth_between(-0.04f, 0.3f, toward_sun.y));
    const math::vec3 tint{mixf(1.0f, 1.0f, warm), mixf(1.0f, 0.55f, warm), mixf(1.0f, 0.35f, warm)};
    const f32 c = cs > 0.0f ? cs : 0.0f;
    const f32 c2 = c * c, c4 = c2 * c2;
    const f32 silver = c4 * c4;
    const f32 k = 0.5f * sun_up * (1.0f + warm) * (0.7f + 0.3f * cs + 1.5f * silver);
    return math::vec3{ambient_sky.x + light.x * tint.x * k, ambient_sky.y + light.y * tint.y * k,
                      ambient_sky.z + light.z * tint.z * k};
}

math::vec3 cirrus_apply(math::vec3 c, math::vec3 d, math::vec3 toward_sun, math::vec3 light, math::vec3 ambient_sky,
                        const CirrusParams& p, f32 pix) {
    const f32 a = cirrus_opacity(d, p, pix);
    if (!(a > 0.0f)) return c;
    const f32 cs = d.x * toward_sun.x + d.y * toward_sun.y + d.z * toward_sun.z;
    const math::vec3 col = cirrus_color(toward_sun, cs, light, ambient_sky);
    return math::vec3{mixf(c.x, col.x, a), mixf(c.y, col.y, a), mixf(c.z, col.z, a)};
}

}  // namespace mce::render
