#include "mce/render/underwater.h"

#include <cmath>

#include "mce/render/scene3d.h"
#include "unit_clamp.h"

namespace mce::render {

namespace {

constexpr f32 kDefaultShallow[3] = {0.10f, 0.38f, 0.40f};
constexpr f64 kPeriod = 64.0;  // kWaterPeriod (forward3d.cpp): a wrapped f64 clock repeats exactly
constexpr f64 kTau = 6.283185307179586;
constexpr f64 kCycles[4] = {3.0, 5.0, 7.0, 11.0};

bool finite3(math::vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

bool finite_matrix(const math::mat4& m) {
    for (usize c = 0; c < 4; ++c)
        for (usize r = 0; r < 4; ++r)
            if (!std::isfinite(m.c[c][r])) return false;
    return true;
}

f32 luma(math::vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

f32 smooth01(f32 t) {
    const f32 x = t > 0.0f ? (t < 1.0f ? t : 1.0f) : 0.0f;
    return x * x * (3.0f - 2.0f * x);
}

f32 depth_scale(f32 m) { return detail::ranged(m, 0.5f, 200.0f, 8.0f); }

}  // namespace

f32 underwater_band(f32 amplitude, f32 band_setting) {
    if (band_setting == band_setting && band_setting > 0.0f) return detail::ranged(band_setting, 0.05f, 2.0f, 0.05f);
    const f32 a = amplitude == amplitude ? amplitude : 0.0f;
    return detail::ranged(1.5f * a, 0.15f, 1.0f, 0.15f);
}

f32 underwater_submersion(f32 cam_depth, f32 band) {
    const f32 b = detail::ranged(band, 0.05f, 2.0f, 0.15f);
    const f32 d = cam_depth == cam_depth ? cam_depth : 0.0f;
    return smooth01((d + b) / (2.0f * b));
}

math::vec3 underwater_sigma(math::vec3 shallow, f32 depth_scale_m, f32 fog_scale) {
    const f32 len = depth_scale(depth_scale_m) * detail::ranged(fog_scale, 0.25f, 4.0f, 1.0f);
    const f32 c[3] = {shallow.x, shallow.y, shallow.z};
    f32 s[3];
    for (usize i = 0; i < 3; ++i) {
        const f32 v = -std::log(detail::ranged(c[i], 0.02f, 1.0f, kDefaultShallow[i]));
        s[i] = (v > 0.0f ? v : 0.0f) / len;
    }
    return math::vec3{s[0], s[1], s[2]};
}

math::vec3 underwater_transmittance(math::vec3 sigma, f32 dist) {
    const f32 d = detail::ranged(dist, 0.0f, kUnderwaterFarCap, 0.0f);
    const f32 s[3] = {sigma.x, sigma.y, sigma.z};
    f32 t[3];
    for (usize i = 0; i < 3; ++i) {
        const f32 k = detail::ranged(s[i], 0.0f, 3.0e38f, 0.0f);
        const f32 v = std::exp(-k * d);
        t[i] = v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f;
    }
    return math::vec3{t[0], t[1], t[2]};
}

math::vec3 underwater_scatter(const WaterSurface& water, f32 cam_depth, f32 light) {
    const f32 depth = cam_depth == cam_depth && cam_depth > 0.0f ? cam_depth : 0.0f;
    const f32 t = std::exp(-depth / depth_scale(water.depth_scale_m));
    const f32 l = detail::ranged(light, 0.0f, 1.0f, 0.04f);
    const math::vec3 d = water.deep_color, s = water.shallow_color;
    return math::vec3{(d.x * (1.0f - t) + s.x * t) * l, (d.y * (1.0f - t) + s.y * t) * l, (d.z * (1.0f - t) + s.z * t) * l};
}

f32 underwater_light(const DirectionalLight& sun) {
    f32 sun_term = 0.0f;
    const math::vec3 dir = sun.direction;
    const f32 len2 = dir.x * dir.x + dir.y * dir.y + dir.z * dir.z;
    if (finite3(dir) && finite3(sun.color) && std::isfinite(sun.intensity) && len2 >= 1.0e-12f && len2 <= 3.0e38f) {
        const f32 down = -dir.y / std::sqrt(len2);
        const f32 facing = down > 0.0f ? down : 0.0f;
        sun_term = luma(sun.color) * sun.intensity * facing / 2.0f;
    }
    const f32 ambient = finite3(sun.ambient_sky) ? luma(sun.ambient_sky) : 0.0f;
    return detail::ranged(sun_term + ambient, 0.04f, 1.0f, 0.04f);
}

f32 underwater_window(f32 dir_y, f32 softness) {
    if (!(dir_y == dir_y)) return 0.0f;
    const f32 s = detail::ranged(softness, 0.0001f, 0.3f, kUnderwaterWindowSoftness);
    const f32 lo = kUnderwaterWindowCos - s;
    return smooth01((dir_y - lo) / (2.0f * s));
}

f32 underwater_lid_distance(f32 cam_depth, f32 dir_y) {
    const f32 d = cam_depth == cam_depth ? cam_depth : 0.0f;
    const f32 y = dir_y == dir_y && dir_y > 1.0e-3f ? dir_y : 1.0e-3f;
    return detail::ranged(d / y, 0.0f, kUnderwaterFarCap, 0.0f);
}

bool underwater_frame(const WaterSurface& water, const Underwater& setting, const View3D& view, const f64 origin[3],
                      const DirectionalLight& sun, f32 time_s, f32 aspect, u32 detail_tier, UnderwaterFrame& out) {
    if (!water.enabled || !std::isfinite(water.level_y)) return false;
    if (!setting.enabled) return false;
    const f32 strength = detail::ranged(setting.strength, 0.0f, 1.0f, 1.0f);
    if (!(strength > 0.0f)) return false;
    if (!finite3(view.position) || !finite_matrix(view.view) || !finite_matrix(view.proj)) return false;
    if (!std::isfinite(origin[0]) || !std::isfinite(origin[1]) || !std::isfinite(origin[2])) return false;
    // A perspective projection has proj[2][3] == -1 and proj[3][3] == 0; an orthographic one is unsupported.
    if (view.proj.c[3][3] != 0.0f) return false;
    const f32 px = view.proj.c[0][0], py = view.proj.c[1][1];
    if (!(std::fabs(px) > 1.0e-6f) || !(std::fabs(py) > 1.0e-6f)) return false;

    const f32 level = detail::ranged(water.level_y, -1000.0f, 1000.0f, 0.0f);
    const f32 cam_depth = static_cast<f32>(static_cast<f64>(level) - origin[1]) - view.position.y;
    if (!std::isfinite(cam_depth)) return false;
    const f32 band = underwater_band(water.amplitude, setting.band);
    const f32 s = underwater_submersion(cam_depth, band) * strength;
    if (!(s > 0.0f)) return false;

    UnderwaterFrame f;
    const u32 tier = detail_tier < 2u ? detail_tier : 2u;
    const f32 light = underwater_light(sun);
    f.submersion = s;
    f.aspect = (std::isfinite(aspect) && aspect > 0.0f) ? aspect : 1.0f;
    f.slope_x = 1.0f / px;
    f.slope_y = 1.0f / py;
    const math::mat4& v = view.view;
    f.right = math::vec3{v.c[0][0], v.c[1][0], v.c[2][0]};
    f.up = math::vec3{v.c[0][1], v.c[1][1], v.c[2][1]};
    f.forward = math::vec3{-v.c[0][2], -v.c[1][2], -v.c[2][2]};
    f.cam_depth = cam_depth;
    f.detail = tier;
    const math::vec3 sigma = underwater_sigma(water.shallow_color, water.depth_scale_m, setting.fog_scale);
    f.sigma = tier == 0 ? math::vec3{sigma.x * kUnderwaterFlatDistance, sigma.y * kUnderwaterFlatDistance,
                                     sigma.z * kUnderwaterFlatDistance}
                        : sigma;
    f.scatter = underwater_scatter(water, cam_depth, light);
    const math::vec3 sh = water.shallow_color;
    const f32 lk = 2.0f * light;
    f.lid = math::vec3{detail::ranged(sh.x, 0.0f, 1.0f, kDefaultShallow[0]) * lk,
                       detail::ranged(sh.y, 0.0f, 1.0f, kDefaultShallow[1]) * lk,
                       detail::ranged(sh.z, 0.0f, 1.0f, kDefaultShallow[2]) * lk};
    f.window_softness = kUnderwaterWindowSoftness;
    f.wobble_amp = tier == 0 ? 0.0f : 0.004f * detail::ranged(setting.wobble, 0.0f, 2.0f, 0.5f);

    const f64 t = std::isfinite(time_s) ? static_cast<f64>(time_s) : 0.0;
    const f64 wrapped = t - kPeriod * std::floor(t / kPeriod);
    for (usize i = 0; i < 4; ++i) {
        const f64 cycles = kCycles[i] * wrapped / kPeriod;
        const f64 frac = cycles - std::floor(cycles);
        f.phase[i] = static_cast<f32>(frac * kTau);
    }
    out = f;
    return true;
}

}  // namespace mce::render
