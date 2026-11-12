#include "mce/render/halo.h"

#include <cmath>

#include "mce/render/scene3d.h"
#include "unit_clamp.h"

namespace mce::render {

namespace {

f32 saturate(f32 t) { return t > 0.0f ? (t < 1.0f ? t : 1.0f) : 0.0f; }

// GLSL smoothstep(lo, hi, x).
f32 smooth_between(f32 lo, f32 hi, f32 x) {
    const f32 t = saturate((x - lo) / (hi - lo));
    return t * t * (3.0f - 2.0f * t);
}

// GLSL mix(a, b, t).
f32 mixf(f32 a, f32 b, f32 t) { return a * (1.0f - t) + b * t; }

f32 maxf(f32 a, f32 b) { return a > b ? a : b; }
f32 minf(f32 a, f32 b) { return a < b ? a : b; }

f32 dot3(math::vec3 a, math::vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// min(a * k, cap * a) per channel (the shader's min(L * (rgb * k), cap * L)).
math::vec3 capped(math::vec3 a, math::vec3 rgb, f32 k, f32 cap) {
    return math::vec3{minf(a.x * (rgb.x * k), cap * a.x), minf(a.y * (rgb.y * k), cap * a.y),
                      minf(a.z * (rgb.z * k), cap * a.z)};
}

}  // namespace

HaloParams halo_shader_params(const Halo& h) {
    HaloParams p;
    const f32 strength = detail::unit01(h.strength);
    if (!h.enabled || !(strength > 0.0f)) return p;
    p.strength = strength;
    p.sun_dogs = detail::ranged(h.sun_dogs, 0.0f, 1.0f, 1.0f);
    p.moon = detail::ranged(h.moon, 0.0f, 1.0f, 0.5f);
    p.dispersion = detail::ranged(h.dispersion, 0.0f, 1.0f, 1.0f);
    return p;
}

f32 halo_ring(f32 x, f32 pix) {
    const f32 wi = maxf(kHaloInnerWidth, kHaloEdgePixels * pix);
    return smooth_between(-wi, 0.0f, x) * std::exp(-maxf(x, 0.0f) / kHaloOuterWidth);
}

math::vec3 halo_ring_rgb(f32 theta, f32 pix, f32 dispersion) {
    const f32 x = theta - kHaloRadius;
    return math::vec3{halo_ring(x + kHaloShiftRed * dispersion, pix), kHaloGreenWeight * halo_ring(x, pix),
                      kHaloBlueWeight * halo_ring(x - kHaloShiftBlue * dispersion, pix)};
}

math::vec3 halo_sun_dog_center(math::vec3 toward_sun, f32 side) {
    const f32 se = toward_sun.y > -1.0f ? (toward_sun.y < 1.0f ? toward_sun.y : 1.0f) : -1.0f;
    const f32 ce2 = maxf(1.0f - se * se, 1e-6f);
    const f32 cr = (kHaloCosRadius - se * se) / ce2;
    const f32 cp = cr > -1.0f ? (cr < 1.0f ? cr : 1.0f) : -1.0f;
    const f32 sp = std::sqrt(maxf(1.0f - cp * cp, 0.0f));
    const f32 hl = std::sqrt(toward_sun.x * toward_sun.x + toward_sun.z * toward_sun.z);
    const f32 hx = hl > 1e-6f ? toward_sun.x / hl : 1.0f, hz = hl > 1e-6f ? toward_sun.z / hl : 0.0f;
    const f32 ce = std::sqrt(ce2);
    return math::vec3{ce * (cp * hx + side * sp * -hz), se, ce * (cp * hz + side * sp * hx)};
}

f32 halo_dog_profile(f32 along, f32 across2, f32 pix) {
    const f32 wi = maxf(kHaloDogInner, kHaloEdgePixels * pix);
    const f32 w = along < 0.0f ? wi : kHaloDogOuter;
    return std::exp(-(along * along) / (w * w) - across2 / (kHaloDogAcross * kHaloDogAcross));
}

math::vec3 halo_dog_rgb(math::vec3 d, math::vec3 dog, math::vec3 toward_sun, f32 pix, f32 dispersion) {
    const f32 k = dot3(dog, toward_sun);
    math::vec3 r{dog.x - toward_sun.x * k, dog.y - toward_sun.y * k, dog.z - toward_sun.z * k};
    const f32 rl = 1.0f / std::sqrt(maxf(dot3(r, r), 1e-12f));
    r = math::vec3{r.x * rl, r.y * rl, r.z * rl};
    const math::vec3 o{d.x - dog.x, d.y - dog.y, d.z - dog.z};
    const f32 al = dot3(o, r);
    const f32 ac2 = maxf(dot3(o, o) - al * al, 0.0f);
    return math::vec3{halo_dog_profile(al + kHaloShiftRed * dispersion, ac2, pix),
                      kHaloGreenWeight * halo_dog_profile(al, ac2, pix),
                      kHaloBlueWeight * halo_dog_profile(al - kHaloShiftBlue * dispersion, ac2, pix)};
}

f32 halo_dog_gain(f32 sun_y) {
    return mixf(1.0f, 0.25f, smooth_between(0.0f, 0.8f, sun_y)) * (1.0f - smooth_between(0.7f, 0.87f, sun_y));
}

f32 halo_veil(f32 opacity, f32 dy) {
    return saturate(2.0f * opacity / maxf(smooth_between(0.02f, 0.2f, dy), 1e-3f)) * smooth_between(0.02f, 0.1f, dy);
}

math::vec3 halo_term(math::vec3 d, math::vec3 toward_sun, f32 cs, f32 opacity, f32 pix, const HaloParams& p,
                     const HaloLight& l) {
    math::vec3 h{0.0f, 0.0f, 0.0f};
    const f32 amp = p.strength * halo_veil(opacity, d.y) * (1.0f - smooth_between(kHaloPixFadeLo, kHaloPixFadeHi, pix));
    if (!(amp > 0.0f)) return h;
    const bool moon_light = l.moon_intensity > 0.0f && dot3(toward_sun, l.moon_dir) > 0.9999f;
    const f32 gate = moon_light ? 0.0f : smooth_between(-0.05f, 0.02f, toward_sun.y);
    if (gate > 0.0f && cs > kHaloMinCos) {
        const f32 th = std::acos(minf(cs, 1.0f));
        const math::vec3 ring = halo_ring_rgb(th, pix, p.dispersion);
        const math::vec3 add = capped(l.light, ring, kHaloRingGain * amp * gate * smooth_between(0.1f, 0.4f, th),
                                      kHaloRingCap);
        h = math::vec3{h.x + add.x, h.y + add.y, h.z + add.z};
        const f32 g = p.sun_dogs * halo_dog_gain(toward_sun.y);
        if (g > 0.0f) {
            for (const f32 side : {-1.0f, 1.0f}) {
                const math::vec3 dog = halo_sun_dog_center(toward_sun, side);
                if (!(dot3(d, dog) > kHaloDogMinCos)) continue;
                const math::vec3 dr = halo_dog_rgb(d, dog, toward_sun, pix, p.dispersion);
                const math::vec3 da = capped(l.light, dr, kHaloDogGain * g * amp * gate, kHaloDogCap);
                h = math::vec3{h.x + da.x, h.y + da.y, h.z + da.z};
            }
        }
    }
    if (p.moon > 0.0f && l.moon_intensity > 0.0f && l.night_fade > 0.0f) {
        const f32 cm = dot3(d, l.moon_dir);
        if (cm > kHaloMinCos) {
            const f32 th = std::acos(minf(cm, 1.0f));
            const math::vec3 m{l.moon_tint.x * l.moon_intensity, l.moon_tint.y * l.moon_intensity,
                               l.moon_tint.z * l.moon_intensity};
            const math::vec3 ring = halo_ring_rgb(th, pix, 0.5f * p.dispersion);
            const math::vec3 add =
                capped(m, ring, kHaloMoonGain * amp * p.moon * l.night_fade * smooth_between(0.1f, 0.4f, th),
                       kHaloMoonCap);
            h = math::vec3{h.x + add.x, h.y + add.y, h.z + add.z};
        }
    }
    return h;
}

}  // namespace mce::render
