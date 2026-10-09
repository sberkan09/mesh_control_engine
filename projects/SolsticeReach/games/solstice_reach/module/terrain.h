#pragma once
// The island's height field: a line-for-line port of tools/island.py (double precision, the same
// integer hash), so the collider the game builds matches the mesh the generator wrote. Change
// both files together.

#include <cmath>
#include <cstdint>

namespace solstice_reach::terrain {

inline constexpr double kSeaFloor = -7.0;
inline constexpr double kIslandRadius = 92.0;
inline constexpr double kHalf = 100.0;

inline double h01(std::int64_t ix, std::int64_t iz, std::int64_t seed) {
    std::uint32_t n = static_cast<std::uint32_t>(ix) * 374761393u + static_cast<std::uint32_t>(iz) * 668265263u +
                      static_cast<std::uint32_t>(seed) * 2246822519u;
    n = n ^ (n >> 13);
    n = n * 1274126177u;
    n = n ^ (n >> 16);
    return static_cast<double>(n) / 4294967296.0;
}

inline double vnoise(double x, double z, std::int64_t seed) {
    const double fxi = std::floor(x), fzi = std::floor(z);
    const std::int64_t ix = static_cast<std::int64_t>(fxi), iz = static_cast<std::int64_t>(fzi);
    const double fx = x - fxi, fz = z - fzi;
    const double sx = fx * fx * (3.0 - 2.0 * fx);
    const double sz = fz * fz * (3.0 - 2.0 * fz);
    const double a = h01(ix, iz, seed), b = h01(ix + 1, iz, seed);
    const double c = h01(ix, iz + 1, seed), d = h01(ix + 1, iz + 1, seed);
    const double top = a + (b - a) * sx;
    const double bot = c + (d - c) * sx;
    return top + (bot - top) * sz;
}

inline double fbm(double x, double z, std::int64_t seed, int octaves) {
    double v = 0.0, amp = 0.5, f = 1.0;
    for (int o = 0; o < octaves; ++o) {
        v += amp * vnoise(x * f, z * f, seed + o * 131);
        amp *= 0.5;
        f *= 2.03;
    }
    return v;
}

inline double smooth(double t) { return t * t * (3.0 - 2.0 * t); }
inline double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

struct Plateau {
    double x, z, radius, height, blend;
};
struct Ramp {
    double x0, z0, h0, x1, z1, h1, half_width, blend;
};

inline constexpr Plateau kPlateaus[] = {
    {0.0, 74.0, 11.0, 1.6, 8.0},    {-28.0, 40.0, 11.0, 14.0, 7.0}, {10.0, 44.0, 9.0, 9.0, 6.0},
    {54.0, 8.0, 15.0, 14.0, 8.0},   {-56.0, 26.0, 7.0, 12.0, 5.0},  {-66.0, -10.0, 6.0, 28.0, 5.0},
    {-52.0, -42.0, 8.0, 40.0, 5.0}, {-4.0, -62.0, 16.0, 42.0, 9.0}, {48.0, -44.0, 13.0, 30.0, 6.0},
    {-6.0, -20.0, 12.0, 50.0, 8.0}, {-38.0, 6.0, 4.5, 32.0, 3.0},
};
inline constexpr Ramp kRamps[] = {
    {0.0, 65.0, 1.6, 8.0, 50.0, 9.0, 3.0, 4.0},        {4.0, 40.0, 9.0, -20.0, 43.0, 14.0, 3.0, 4.0},
    {16.0, 41.0, 9.0, 44.0, 16.0, 14.0, 3.0, 4.0},     {-36.0, 35.0, 14.0, -52.0, 28.0, 12.0, 3.0, 4.0},
    {-57.0, 21.0, 12.0, -65.0, -6.0, 28.0, 2.5, 3.0},  {-65.0, -13.0, 28.0, -54.0, -37.0, 40.0, 2.5, 3.0},
    {-47.0, -45.0, 40.0, -18.0, -58.0, 42.0, 3.0, 4.0}, {-10.0, -49.0, 42.0, -8.0, -30.0, 50.0, 3.0, 4.0},
    {53.0, -4.0, 14.0, 50.0, -34.0, 30.0, 3.0, 4.0},   {38.0, -49.0, 30.0, 10.0, -62.0, 42.0, 3.0, 4.0},
    {-22.0, 32.0, 14.0, -35.5, 9.5, 32.0, 2.5, 3.5},   {-40.0, 3.0, 32.0, -12.0, -10.0, 50.0, 2.5, 3.5},
};

// A ramp's height profile: mostly linear with eased ends (island.grade).
inline double grade(double t) { return 0.7 * t + 0.3 * smooth(t); }

inline double base_height(double x, double z) {
    const double t = clampd((60.0 - z) / 150.0, 0.0, 1.0);
    double h = 2.0 + 38.0 * smooth(t);
    h += (fbm(x * 0.012 + 7.0, z * 0.012 + 3.0, 11, 5) - 0.5) * 18.0;
    h += (fbm(x * 0.05 + 1.0, z * 0.05 + 9.0, 23, 4) - 0.5) * 4.0;
    const double d = std::sqrt(x * x + z * z);
    double cx = 1.0, cz = 0.0;
    if (d > 0.001) {
        cx = x / d;
        cz = z / d;
    }
    const double radius = kIslandRadius + (fbm(cx * 3.0 + 5.0, cz * 3.0 + 5.0, 41, 3) - 0.5) * 22.0;
    const double fall = smooth(clampd((radius - d) / 24.0, 0.0, 1.0));
    h = h * fall + kSeaFloor * (1.0 - fall);
    const double cliff = smooth(clampd((-z - 80.0) / 12.0, 0.0, 1.0));
    h = h * (1.0 - cliff) + kSeaFloor * cliff;
    return h;
}

inline double height(double x, double z) {
    double h = base_height(x, z);
    // Plateaus first, then the ramps over them (island.height: a ramp's ends sit inside the
    // plateaus it joins, at their height).
    for (const Plateau& p : kPlateaus) {
        const double dist = std::sqrt((x - p.x) * (x - p.x) + (z - p.z) * (z - p.z));
        const double w = smooth(clampd((p.radius + p.blend - dist) / p.blend, 0.0, 1.0));
        h = h * (1.0 - w) + p.height * w;
    }
    for (const Ramp& r : kRamps) {
        const double dx = r.x1 - r.x0, dz = r.z1 - r.z0;
        const double length2 = dx * dx + dz * dz;
        const double t = clampd(((x - r.x0) * dx + (z - r.z0) * dz) / length2, 0.0, 1.0);
        const double px = r.x0 + dx * t, pz = r.z0 + dz * t;
        const double dist = std::sqrt((x - px) * (x - px) + (z - pz) * (z - pz));
        const double w = smooth(clampd((r.half_width + r.blend - dist) / r.blend, 0.0, 1.0));
        const double target = r.h0 + (r.h1 - r.h0) * grade(t);
        h = h * (1.0 - w) + target * w;
    }
    return h;
}

inline float height_f(float x, float z) { return static_cast<float>(height(static_cast<double>(x), static_cast<double>(z))); }

// The ground normal by central differences (0.5 m), as the generator's shading uses.
inline void normal(double x, double z, float out[3]) {
    const double hx = height(x + 0.5, z) - height(x - 0.5, z);
    const double hz = height(x, z + 0.5) - height(x, z - 0.5);
    const double nx = -hx, ny = 1.0, nz = -hz;
    const double l = std::sqrt(nx * nx + ny * ny + nz * nz);
    out[0] = static_cast<float>(nx / l);
    out[1] = static_cast<float>(ny / l);
    out[2] = static_cast<float>(nz / l);
}

}  // namespace solstice_reach::terrain
