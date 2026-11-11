#include "mce/render/caustics.h"

#include <cmath>

#include "unit_clamp.h"

namespace mce::render {

namespace {

// pcg2d (Jarzynski and Olano 2020), both lanes: kCausticsFS's ca_hash.
void ca_hash(u32& x, u32& y) {
    x = x * 1664525u + 1013904223u;
    y = y * 1664525u + 1013904223u;
    x += y * 1664525u;
    y += x * 1664525u;
    x ^= x >> 16u;
    y ^= y >> 16u;
    x += y * 1664525u;
    y += x * 1664525u;
    x ^= x >> 16u;
    y ^= y >> 16u;
}

f32 ca_unit(u32 h) { return f32(h >> 8u) * (1.0f / 16777216.0f); }

// A closed loop in turns without sin: a parabola per half turn (C1, period 1, -1..1); kCausticsFS's ca_loop.
f32 ca_loop(f32 a) {
    a = a - std::floor(a);
    const f32 b = a < 0.5f ? a : a - 0.5f;
    const f32 v = 16.0f * b * (0.5f - b);
    return a < 0.5f ? v : -v;
}

f32 saturate(f32 t) { return t > 0.0f ? (t < 1.0f ? t : 1.0f) : 0.0f; }

// GLSL smoothstep.
f32 smooth_between(f32 lo, f32 hi, f32 x) {
    const f32 t = saturate((x - lo) / (hi - lo));
    return t * t * (3.0f - 2.0f * t);
}

// GLSL mod(x, y) for y > 0.
f32 glsl_mod(f32 x, f32 y) { return x - y * std::floor(x / y); }

bool finite3(math::vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

f32 caustics_scale(f32 m) { return detail::ranged(m, 0.25f, 16.0f, 2.0f); }

}  // namespace

f32 caustics_phase(f32 time, f32 speed) {
    const f64 t = std::isfinite(time) ? f64(time) : 0.0;
    const f64 s = f64(detail::ranged(speed, 0.0f, 4.0f, 1.0f));
    const f64 ticks = std::floor(t * s * f64(kCausticsTicksPerSecond));
    const f64 period = f64(kCausticsPeriodTicks);
    f64 wrapped = ticks - period * std::floor(ticks / period);  // a whole number of ticks, 0..period - 1
    if (!(wrapped >= 0.0) || !(wrapped < period)) wrapped = 0.0;
    return f32(wrapped) / f32(kCausticsPeriodTicks);
}

math::vec2 caustics_offset(f64 world_x, f64 world_z, f32 scale_m) {
    const f64 inv = 1.0 / f64(caustics_scale(scale_m));
    const f64 period = f64(kCausticsCells);
    const auto wrap = [&](f64 w) {
        if (!std::isfinite(w)) return 0.0f;
        const f64 x = w * inv;
        const f32 r = f32(x - period * std::floor(x / period));
        return r < f32(kCausticsCells) ? r : 0.0f;  // a value just under the period may round up to it
    };
    return math::vec2{wrap(world_x), wrap(world_z)};
}

math::vec2 caustics_water_centre(math::vec2 camera_local_xz, f32 half_extent) {
    const f32 he = detail::ranged(half_extent, 1.0f, 20000.0f, 500.0f);
    const f32 cell = 2.0f * he / 16.0f;
    if (!std::isfinite(camera_local_xz.x) || !std::isfinite(camera_local_xz.y)) return math::vec2{0.0f, 0.0f};
    const f32 cx = std::floor(camera_local_xz.x / cell + 0.5f) * cell, cz = std::floor(camera_local_xz.y / cell + 0.5f) * cell;
    return math::vec2{cx - camera_local_xz.x, cz - camera_local_xz.y};
}

f32 caustics_cells(math::vec2 q, u32 m, u32 seed, f32 phase) {
    if (m == 0u) return 0.0f;
    const f32 ix = std::floor(q.x), iy = std::floor(q.y);
    const f32 fx = q.x - ix, fy = q.y - iy;
    const u32 cx = u32(ix > 0.0f ? ix : 0.0f) % m, cy = u32(iy > 0.0f ? iy : 0.0f) % m;  // q may round up to m: wraps to 0
    f32 d1 = 64.0f, d2 = 64.0f;  // squared distances: nearest, second nearest
    for (u32 y = 0; y < 3u; ++y)
        for (u32 x = 0; x < 3u; ++x) {
            u32 hx = (cx + x + (m - 1u)) % m + seed, hy = (cy + y + (m - 1u)) % m;
            ca_hash(hx, hy);
            u32 gx = hx ^ 0x9E3779B9u, gy = hy ^ 0x85EBCA6Bu;
            ca_hash(gx, gy);
            const f32 k = f32(1u + gx % 3u);
            const f32 a = k * phase + ca_unit(gy);
            const f32 px = f32(i32(x) - 1) + (0.2f + 0.6f * ca_unit(hx)) + kCausticsLoopRadius * ca_loop(a + 0.25f);
            const f32 py = f32(i32(y) - 1) + (0.2f + 0.6f * ca_unit(hy)) + kCausticsLoopRadius * ca_loop(a);
            const f32 dx = px - fx, dy = py - fy;
            const f32 dd = dx * dx + dy * dy;
            if (dd < d1) {
                d2 = d1;
                d1 = dd;
            } else if (dd < d2) {
                d2 = dd;
            }
        }
    const f32 e = std::sqrt(d2) - std::sqrt(d1);
    return 1.0f - smooth_between(0.0f, kCausticsLineWidth, e);
}

f32 caustics_pattern(math::vec2 q, f32 phase, u32 octaves) {
    if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(phase) || octaves == 0u) return 0.0f;
    const f32 period = f32(kCausticsCells);
    const math::vec2 w{glsl_mod(q.x, period), glsl_mod(q.y, period)};
    f32 n = caustics_cells(w, kCausticsCells, 0x6C8E9CF5u, phase) * 0.6f;
    f32 sum = 0.6f;
    if (octaves >= 2u) {
        n += caustics_cells(math::vec2{w.x * 2.0f, w.y * 2.0f}, kCausticsCells * 2u, 0x2545F491u, phase) * 0.3f;
        sum += 0.3f;
    }
    if (octaves >= 3u) {
        n += caustics_cells(math::vec2{w.x * 4.0f, w.y * 4.0f}, kCausticsCells * 4u, 0x9E3779B1u, phase) * 0.1f;
        sum += 0.1f;
    }
    return octaves >= 3u ? n : n / sum;
}

f32 caustics_facing(math::vec3 normal, math::vec3 toward_sun) {
    if (!finite3(normal) || !finite3(toward_sun) || !(toward_sun.y >= kCausticsMinSunY)) return 0.0f;
    const f32 n2 = normal.x * normal.x + normal.y * normal.y + normal.z * normal.z;
    if (!(n2 >= 0.25f) || !std::isfinite(n2)) return 0.0f;
    const f32 ndl = (normal.x * toward_sun.x + normal.y * toward_sun.y + normal.z * toward_sun.z) / std::sqrt(n2);
    return smooth_between(kCausticsFacingLo, kCausticsFacingHi, ndl / toward_sun.y);
}

math::vec3 caustics_composite(math::vec3 src, math::vec3 color, math::vec3 term) {
    const f32 lt = 0.2126f * term.x + 0.7152f * term.y + 0.0722f * term.z;
    const f32 ls = 0.2126f * src.x + 0.7152f * src.y + 0.0722f * src.z;
    math::vec3 ch{1.0f, 1.0f, 1.0f};
    if (ls > kCausticsMinLuma && ls < 1.0e30f) {
        const auto chroma = [&](f32 v) {
            const f32 c = v / ls;
            return c > 0.0f ? (c < kCausticsMaxChroma ? c : kCausticsMaxChroma) : 0.0f;  // NaN: 0
        };
        ch = math::vec3{chroma(src.x), chroma(src.y), chroma(src.z)};
    }
    return math::vec3{src.x + color.x * lt * ch.x, src.y + color.y * lt * ch.y, src.z + color.z * lt * ch.z};
}

f32 caustics_sun_fade(f32 sun_y) {
    if (!std::isfinite(sun_y)) return 0.0f;
    return smooth_between(kCausticsMinSunY, kCausticsFullSunY, sun_y);
}

f32 caustics_depth_fade(f32 depth, f32 max_depth) {
    if (!std::isfinite(depth) || !std::isfinite(max_depth) || !(depth > 0.0f) || !(depth < max_depth)) return 0.0f;
    return smooth_between(0.0f, kCausticsSurfaceFade, depth) *
           (1.0f - smooth_between((1.0f - kCausticsDeepFade) * max_depth, max_depth, depth));
}

f32 caustics_view_path(math::vec3 p, f32 plane_h) {
    if (!finite3(p) || !std::isfinite(plane_h) || !(plane_h < 0.0f)) return 0.0f;
    const f32 depth = plane_h - p.y;
    if (!(depth > 0.0f)) return 0.0f;
    const f32 len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    const f32 d = len * depth / -p.y;  // -p.y > depth > 0: the camera is above the plane, the point under it
    return std::isfinite(d) ? d : 0.0f;
}

f32 caustics_absorption(f32 absorption, f32 d_sun, f32 d_view) {
    if (!std::isfinite(d_sun) || !std::isfinite(d_view) || d_sun < 0.0f || d_view < 0.0f) return 0.0f;
    return std::exp(-detail::ranged(absorption, 0.0f, 4.0f, 0.15f) * (d_sun + d_view));
}

bool caustics_plane_point(math::vec3 p, const CausticsParams& c, math::vec2& plane_xz, f32& depth) {
    if (!finite3(p) || !finite3(c.toward_sun) || !std::isfinite(c.plane_h) || !std::isfinite(c.centre.x) ||
        !std::isfinite(c.centre.y) || !std::isfinite(c.half_extent))
        return false;
    if (!(c.toward_sun.y >= kCausticsMinSunY)) return false;
    const f32 d = c.plane_h - p.y;
    if (!(d > 0.0f)) return false;
    const f32 t = d / c.toward_sun.y;
    const f32 x = p.x + c.toward_sun.x * t, z = p.z + c.toward_sun.z * t;
    if (!(std::fabs(x) < 1.0e30f) || !(std::fabs(z) < 1.0e30f)) return false;
    if (!(std::fabs(x - c.centre.x) <= c.half_extent) || !(std::fabs(z - c.centre.y) <= c.half_extent)) return false;
    plane_xz = math::vec2{x, z};
    depth = d;
    return true;
}

math::vec3 caustics_water_sigma(math::vec3 shallow_color, f32 depth_scale_m) {
    const f32 scale = detail::ranged(depth_scale_m, 0.1f, 200.0f, 8.0f);
    const f32 d = scale > 0.5f ? scale : 0.5f;
    const auto sigma = [&](f32 c) {
        const f32 u = detail::unit01(c);  // NaN -> 0 (unit01 of NaN is 0)
        return -std::log(u > 0.02f ? (u < 1.0f ? u : 1.0f) : 0.02f) / d;
    };
    return math::vec3{sigma(shallow_color.x), sigma(shallow_color.y), sigma(shallow_color.z)};
}

math::vec3 caustics_transmission(math::vec3 p, const CausticsParams& c, f32& fog_distance) {
    const math::vec3 one{1.0f, 1.0f, 1.0f};
    const f32 len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    fog_distance = len;
    if (!finite3(p) || !std::isfinite(len) || !std::isfinite(c.plane_h) || !(c.plane_h < 0.0f) || !(p.y < c.plane_h))
        return one;
    const f32 s = c.plane_h / p.y;  // 0..1: the share of the view ray from the camera to where it crosses the plane
    const f32 vx = p.x * s, vz = p.z * s;
    if (!(std::fabs(vx - c.centre.x) <= c.half_extent) || !(std::fabs(vz - c.centre.y) <= c.half_extent)) return one;
    const f32 depth = c.plane_h - p.y;
    const f32 dv = len * depth / -p.y;
    const f32 q = 1.0f - -p.y / len;
    const f32 q2 = q * q;
    const f32 fres = (0.02f + 0.98f * (q2 * q2 * q)) * c.reflectivity;
    const f32 k = 1.0f - fres;
    fog_distance = len - dv;
    const math::vec3 t{k * std::exp(-c.water_sigma.x * dv), k * std::exp(-c.water_sigma.y * dv),
                       k * std::exp(-c.water_sigma.z * dv)};
    return finite3(t) ? t : math::vec3{0.0f, 0.0f, 0.0f};
}

bool caustics_through_water(math::vec3 p, const CausticsParams& c, f32& surface_share) {
    surface_share = 1.0f;
    if (!finite3(p) || !std::isfinite(c.plane_h) || !(c.plane_h < 0.0f) || !(p.y < c.plane_h)) return false;
    const f32 s = c.plane_h / p.y;
    if (!(std::fabs(p.x * s - c.centre.x) <= c.half_extent) || !(std::fabs(p.z * s - c.centre.y) <= c.half_extent))
        return false;
    surface_share = s;
    return true;
}

math::vec3 caustics_term(math::vec3 p, const CausticsParams& c, f32 vol_point, f32 vol_surface, math::vec3 normal) {
    const math::vec3 zero{0.0f, 0.0f, 0.0f};
    math::vec2 xz;
    f32 depth = 0.0f;
    if (!caustics_plane_point(p, c, xz, depth)) return zero;
    if (!(depth < c.max_depth)) return zero;
    const f32 t = depth / c.toward_sun.y;  // the sun ray's length in water
    const f32 pat = caustics_pattern(math::vec2{xz.x * c.inv_scale + c.offset.x, xz.y * c.inv_scale + c.offset.y}, c.phase,
                                     c.octaves < 3u ? c.octaves : 3u);
    f32 k = pat * std::exp(-c.absorption * t) * caustics_depth_fade(depth, c.max_depth) *
            caustics_facing(normal, c.toward_sun);
    f32 fog_distance = 0.0f;
    const math::vec3 tr = caustics_transmission(p, c, fog_distance);
    f32 share = 1.0f;
    const bool through = caustics_through_water(p, c, share);
    if (c.fog_density != 0.0f && std::isfinite(c.fog_density)) {
        const f32 fd = c.fog_density * fog_distance;
        k *= std::exp(-fd * fd);
        if (through) {
            const f32 fl = c.fog_density * std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
            k *= std::exp(-fl * fl);  // the floor's own fog in the copy the water shows
        }
    }
    k *= vol_point;
    if (through) k *= vol_surface;
    const auto clean = [](f32 v) { return std::isfinite(v) ? saturate(v) : 0.0f; };
    return math::vec3{clean(k * tr.x), clean(k * tr.y), clean(k * tr.z)};
}

bool caustics_gate(const CausticsGate& g, math::vec3& sun, math::vec3& color) {
    if (!g.enabled || !g.tier || g.octaves == 0u || !g.cvar || !g.water || !g.see_through || g.render_texture ||
        !g.post_source || !g.perspective)
        return false;
    if (!finite3(g.toward_sun) || !finite3(g.sun_color) || !std::isfinite(g.intensity) || !std::isfinite(g.strength) ||
        !std::isfinite(g.sun_share))
        return false;
    const f32 len2 = g.toward_sun.x * g.toward_sun.x + g.toward_sun.y * g.toward_sun.y + g.toward_sun.z * g.toward_sun.z;
    if (!(len2 >= 1.0e-12f) || !std::isfinite(len2)) return false;
    const f32 inv = 1.0f / std::sqrt(len2);
    const math::vec3 s{g.toward_sun.x * inv, g.toward_sun.y * inv, g.toward_sun.z * inv};
    if (!(s.y >= kCausticsMinSunY)) return false;
    const f32 k = (g.intensity > 0.0f ? g.intensity : 0.0f) * (g.strength > 0.0f ? g.strength : 0.0f) *
                  saturate(g.sun_share) * caustics_sun_fade(s.y);
    const auto pos = [](f32 x) { return x > 0.0f ? x : 0.0f; };
    const math::vec3 col{pos(g.sun_color.x) * k, pos(g.sun_color.y) * k, pos(g.sun_color.z) * k};
    if (!finite3(col) || !(col.x > 0.0f || col.y > 0.0f || col.z > 0.0f)) return false;
    sun = s;
    color = col;
    return true;
}

}  // namespace mce::render
