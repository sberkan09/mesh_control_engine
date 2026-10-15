#include "mce/render/atmosphere.h"

#include "mce/containers/array.h"

#include <algorithm>
#include <cmath>

namespace mce::render {
namespace {

// Hillaire 2020, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique": per meter.
constexpr double kGround = 6360e3, kTop = 6460e3;
constexpr double kRayleigh[3] = {5.802e-6, 13.558e-6, 33.1e-6};
constexpr double kRayleighHeight = 8000.0;
constexpr double kMieScatter = 3.996e-6, kMieExtinction = 4.40e-6;
constexpr double kMieHeight = 1200.0;
constexpr double kOzone[3] = {0.650e-6, 1.881e-6, 0.085e-6};  // absorption at the layer's peak
constexpr double kPi = 3.14159265358979323846;

struct D3 {
    double x = 0, y = 0, z = 0;
};
D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 operator*(D3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 d3(math::vec3 v) { return {v.x, v.y, v.z}; }

struct Medium {
    double rayleigh[3], mie_scatter, mie_extinction, ozone[3];
    bool operator==(const Medium& o) const {
        for (int c = 0; c < 3; ++c)
            if (rayleigh[c] != o.rayleigh[c] || ozone[c] != o.ozone[c]) return false;
        return mie_scatter == o.mie_scatter && mie_extinction == o.mie_extinction;
    }
};
Medium medium(const Atmosphere& a) {
    Medium m{};
    for (int c = 0; c < 3; ++c) {
        m.rayleigh[c] = kRayleigh[c] * double(a.rayleigh);
        m.ozone[c] = kOzone[c] * double(a.ozone);
    }
    m.mie_scatter = kMieScatter * double(a.mie);
    m.mie_extinction = kMieExtinction * double(a.mie);
    return m;
}

// Extinction per meter at height h (meters above the ground).
void extinction(const Medium& m, double h, double out[3]) {
    const double r = std::exp(-h / kRayleighHeight), mi = std::exp(-h / kMieHeight);
    const double oz = std::fmax(0.0, 1.0 - std::fabs(h - 25000.0) / 15000.0);
    for (int c = 0; c < 3; ++c) out[c] = m.rayleigh[c] * r + m.mie_extinction * mi + m.ozone[c] * oz;
}

// Distance from radius r along cos-zenith mu to the top of the atmosphere, and to the ground (< 0: missed).
double to_top(double r, double mu) {
    const double disc = r * r * (mu * mu - 1.0) + kTop * kTop;
    return std::fmax(-r * mu + std::sqrt(std::fmax(disc, 0.0)), 0.0);
}
double to_ground(double r, double mu) {
    if (mu >= 0.0) return -1.0;
    const double disc = r * r * (mu * mu - 1.0) + kGround * kGround;
    return disc < 0.0 ? -1.0 : -r * mu - std::sqrt(disc);
}

// Transmittance from radius r along mu to space (0 through the ground), integrated directly.
void transmittance_exact(const Medium& m, double r, double mu, double out[3], int steps = 48) {
    if (to_ground(r, mu) >= 0.0) {
        out[0] = out[1] = out[2] = 0.0;
        return;
    }
    const double d = to_top(r, mu);
    double depth[3] = {0, 0, 0};
    for (int i = 0; i < steps; ++i) {
        const double t = (i + 0.5) / steps * d;
        const double h = std::sqrt(t * t + 2.0 * r * mu * t + r * r) - kGround;
        double e[3];
        extinction(m, h, e);
        for (int c = 0; c < 3; ++c) depth[c] += e[c] * d / steps;
    }
    for (int c = 0; c < 3; ++c) out[c] = std::exp(-depth[c]);
}

// Transmittance to space over (height, mu): heights on a square-root scale (dense near the
// ground), mu linear. Built once per Medium (per thread) and reused.
struct TransmittanceLut {
    static constexpr int kH = 32, kMu = 256;
    Medium medium{};
    bool valid = false;
    Array<float> t;  // kH * kMu * 3

    void build(const Medium& m) {
        medium = m;
        t.clear();
        t.resize(usize(kH) * kMu * 3, 0.0f);
        for (int i = 0; i < kH; ++i) {
            const double s = double(i) / (kH - 1), h = s * s * (kTop - kGround);
            for (int j = 0; j < kMu; ++j) {
                const double mu = -1.0 + 2.0 * j / (kMu - 1);
                double o[3];
                transmittance_exact(m, kGround + h, mu, o, 32);
                for (int c = 0; c < 3; ++c) t[(size_t(i) * kMu + j) * 3 + c] = float(o[c]);
            }
        }
        valid = true;
    }
    void sample(double h, double mu, double out[3]) const {
        const double fh = std::sqrt(std::fmin(std::fmax(h / (kTop - kGround), 0.0), 1.0)) * (kH - 1);
        const double fm = std::fmin(std::fmax((mu + 1.0) * 0.5, 0.0), 1.0) * (kMu - 1);
        const int i0 = std::min(int(fh), kH - 2), j0 = std::min(int(fm), kMu - 2);
        const double a = fh - i0, b = fm - j0;
        for (int c = 0; c < 3; ++c) {
            const auto at = [&](int i, int j) { return double(t[(size_t(i) * kMu + j) * 3 + c]); };
            out[c] = (at(i0, j0) * (1 - b) + at(i0, j0 + 1) * b) * (1 - a) + (at(i0 + 1, j0) * (1 - b) + at(i0 + 1, j0 + 1) * b) * a;
        }
    }
};

const TransmittanceLut& lut_for(const Medium& m) {
    thread_local TransmittanceLut lut;
    if (!lut.valid || !(lut.medium == m)) lut.build(m);
    return lut;
}

// Single scattering along `view` from the viewer, per unit solar irradiance (sun = unit, toward it).
void sky(const Atmosphere& a, const Medium& m, const TransmittanceLut& lut, D3 sun, D3 view, double out[3]) {
    constexpr int kSteps = 24;
    const double r0 = kGround + std::fmax(double(a.altitude), 0.5);
    const double mu = view.y;
    const double ground = to_ground(r0, mu);
    const double d = ground >= 0.0 ? ground : to_top(r0, mu);
    const double c = dot(view, sun);
    const double g = double(a.mie_anisotropy);
    const double phase_r = 3.0 / (16.0 * kPi) * (1.0 + c * c);
    const double phase_m = 3.0 / (8.0 * kPi) * (1.0 - g * g) * (1.0 + c * c) /
                           ((2.0 + g * g) * std::pow(std::fmax(1.0 + g * g - 2.0 * g * c, 1e-6), 1.5));
    const D3 origin{0.0, r0, 0.0};
    double sum[3] = {0, 0, 0}, trans[3] = {1, 1, 1};
    for (int i = 0; i < kSteps; ++i) {
        // Quadratic spacing: short steps near the viewer, where the air is densest along low rays.
        const double s0 = double(i) / kSteps, s1 = double(i + 1) / kSteps;
        const double t0 = s0 * s0 * d, t1 = s1 * s1 * d, ds = t1 - t0;
        const D3 p = origin + view * (0.5 * (t0 + t1));
        const double r = std::sqrt(dot(p, p)), h = r - kGround;
        const double rs = std::exp(-h / kRayleighHeight), mi = std::exp(-h / kMieHeight);
        const double oz = std::fmax(0.0, 1.0 - std::fabs(h - 25000.0) / 15000.0);
        double ts[3];
        lut.sample(h, dot(p, sun) / r, ts);
        for (int k = 0; k < 3; ++k) {
            // This step's in-scattering, attenuated by the air to its middle.
            const double step_t = std::exp(-(m.rayleigh[k] * rs + m.mie_extinction * mi + m.ozone[k] * oz) * ds);
            const double mid = trans[k] * std::sqrt(step_t);
            sum[k] += mid * ts[k] * (m.rayleigh[k] * rs * phase_r + m.mie_scatter * mi * phase_m) * ds;
            trans[k] *= step_t;
        }
    }
    if (ground >= 0.0) {  // the ground: Lambert, lit by the sun through the air above it
        const D3 p = origin + view * ground;
        const double r = std::sqrt(dot(p, p)), mu_s = dot(p, sun) / r;
        double ts[3];
        lut.sample(0.0, mu_s, ts);
        const double albedo[3] = {a.ground_albedo.x, a.ground_albedo.y, a.ground_albedo.z};
        for (int k = 0; k < 3; ++k) sum[k] += trans[k] * ts[k] * std::fmax(mu_s, 0.0) * albedo[k] / kPi;
    }
    for (int k = 0; k < 3; ++k) out[k] = sum[k];
}

D3 unit(math::vec3 v) {
    const math::vec3 n = math::normalize(v);
    return math::length_sq(n) > 0.0f ? d3(n) : D3{0.0, 1.0, 0.0};
}

}  // namespace

math::vec3 atmosphere_transmittance(const Atmosphere& a, f32 altitude, math::vec3 dir) {
    const D3 d = unit(dir);
    double t[3];
    transmittance_exact(medium(a), kGround + std::fmax(double(altitude), 0.0), d.y, t);
    return {f32(t[0]), f32(t[1]), f32(t[2])};
}

math::vec3 atmosphere_sky(const Atmosphere& a, math::vec3 sun, math::vec3 view) {
    const Medium m = medium(a);
    double l[3];
    sky(a, m, lut_for(m), unit(sun), unit(view), l);
    return {f32(l[0]), f32(l[1]), f32(l[2])};
}

void bake_atmosphere_sky(const Atmosphere& a, math::vec3 sun_dir, u32 width, u32 height, Span<f32> out) {
    if (width == 0 || height == 0 || out.size() < usize(width) * height * 4) return;
    const Medium m = medium(a);
    const TransmittanceLut& lut = lut_for(m);
    const D3 sun = unit(sun_dir);
    // The sky is symmetric about the sun's vertical plane: march a sky-view table of kRows
    // elevations (square-root spaced around the horizon, where the sky changes fastest) by kAz
    // azimuths from the sun (0..π), then look every texel up in it.
    constexpr int kRows = 64, kAz = 25;
    const double sh = std::sqrt(sun.x * sun.x + sun.z * sun.z);
    const double sx = sh > 1e-6 ? sun.x / sh : 1.0, sz = sh > 1e-6 ? sun.z / sh : 0.0;
    Array<double> table;
    table.resize(usize(kRows) * kAz * 3, 0.0);
    for (int i = 0; i < kRows; ++i) {
        const double x = -1.0 + 2.0 * i / (kRows - 1), el = (x < 0.0 ? -x * x : x * x) * 0.5 * kPi;
        const double cy = std::sin(el), st = std::cos(el);
        for (int k = 0; k < kAz; ++k) {
            const double phi = double(k) / (kAz - 1) * kPi;
            const D3 v{st * (std::cos(phi) * sx - std::sin(phi) * sz), cy, st * (std::cos(phi) * sz + std::sin(phi) * sx)};
            sky(a, m, lut, sun, v, &table[(size_t(i) * kAz + k) * 3]);
        }
    }
    for (u32 y = 0; y < height; ++y) {
        const double el = 0.5 * kPi - (y + 0.5) / height * kPi;  // row 0 looks up
        const double x = (el < 0.0 ? -std::sqrt(-el / (0.5 * kPi)) : std::sqrt(el / (0.5 * kPi)));
        const double fr = (x * 0.5 + 0.5) * (kRows - 1);
        const int r0 = std::min(int(fr), kRows - 2);
        const double fy = fr - r0;
        for (u32 x_ = 0; x_ < width; ++x_) {
            // equirect_uv: u = atan(z, x) / 2π + 0.5.
            const double phi = ((x_ + 0.5) / width - 0.5) * 2.0 * kPi;
            const double dx = std::cos(phi), dz = std::sin(phi);
            const double rel = std::acos(std::fmin(std::fmax(dx * sx + dz * sz, -1.0), 1.0)) / kPi * (kAz - 1);
            const int k0 = std::min(int(rel), kAz - 2);
            const double f = rel - k0;
            f32* o = &out[(usize(y) * width + x_) * 4];
            for (int c = 0; c < 3; ++c) {
                const auto at = [&](int i, int k) { return table[(size_t(i) * kAz + k) * 3 + c]; };
                const double v = (at(r0, k0) * (1.0 - f) + at(r0, k0 + 1) * f) * (1.0 - fy) +
                                 (at(r0 + 1, k0) * (1.0 - f) + at(r0 + 1, k0 + 1) * f) * fy;
                o[c] = f32(v * double(a.intensity));
            }
            o[3] = 1.0f;
        }
    }
}

math::vec3 atmosphere_sky_ambient(const Atmosphere& a, math::vec3 sun_dir) {
    // Scene3D::build asks every frame: the last answer is kept per thread.
    struct Key {
        f32 v[13];
        bool operator==(const Key& o) const {
            for (int i = 0; i < 13; ++i)
                if (v[i] != o.v[i]) return false;
            return true;
        }
    };
    const Key key{{a.intensity, a.rayleigh, a.mie, a.mie_anisotropy, a.ozone, a.ground_albedo.x, a.ground_albedo.y,
                   a.ground_albedo.z, a.altitude, sun_dir.x, sun_dir.y, sun_dir.z, 0.0f}};
    thread_local Key last{};
    thread_local math::vec3 last_value;
    thread_local bool has_last = false;
    if (has_last && last == key) return last_value;
    const Medium m = medium(a);
    const TransmittanceLut& lut = lut_for(m);
    const D3 sun = unit(sun_dir);
    // (1/π) ∫ L cosθ dω over the upper hemisphere, midpoint rule in (cos²θ, φ): uniform in cosine
    // weight, so each sample carries the same share.
    constexpr int kRings = 8, kAz = 16;
    double sum[3] = {0, 0, 0};
    for (int i = 0; i < kRings; ++i) {
        const double c2 = (i + 0.5) / kRings, ct = std::sqrt(c2), st = std::sqrt(1.0 - c2);
        for (int j = 0; j < kAz; ++j) {
            const double phi = (j + 0.5) / kAz * 2.0 * kPi;
            double l[3];
            sky(a, m, lut, sun, D3{st * std::cos(phi), ct, st * std::sin(phi)}, l);
            for (int c = 0; c < 3; ++c) sum[c] += l[c];
        }
    }
    // ∫ L cosθ dω = π · mean(L) under cosine-weighted sampling; divided by π: the mean.
    const double s = double(a.intensity) / (kRings * kAz);
    last = key;
    last_value = {f32(sum[0] * s), f32(sum[1] * s), f32(sum[2] * s)};
    has_last = true;
    return last_value;
}

math::vec3 atmosphere_quantized_sun(const Atmosphere& a, math::vec3 sun) {
    const math::vec3 n = math::normalize(sun);
    if (!(math::length_sq(n) > 0.0f) || !(a.rebake_degrees > 0.0f)) return math::length_sq(n) > 0.0f ? n : math::vec3{0, 1, 0};
    const f32 step = math::radians(a.rebake_degrees);
    const f32 el = std::round(std::asin(math::clamp(n.y, -1.0f, 1.0f)) / step) * step;
    const f32 az = std::round(std::atan2(n.z, n.x) / step) * step;
    return {std::cos(el) * std::cos(az), std::sin(el), std::cos(el) * std::sin(az)};
}

math::vec3 sun_direction_at(f32 hours, f32 noon_elevation_deg, f32 azimuth_deg) {
    // The sun turns about the east-west axis tilted toward the south by (90° - noon elevation).
    const f32 angle = (hours - 6.0f) / 12.0f * math::kPi;  // 0 at sunrise, π at sunset
    const f32 tilt = math::radians(90.0f - noon_elevation_deg);
    math::vec3 to_sun{std::cos(angle), std::sin(angle) * std::cos(tilt), -std::sin(angle) * std::sin(tilt)};
    const f32 az = math::radians(azimuth_deg), ca = std::cos(az), sa = std::sin(az);
    to_sun = {to_sun.x * ca + to_sun.z * sa, to_sun.y, -to_sun.x * sa + to_sun.z * ca};
    return -to_sun;
}

}  // namespace mce::render
