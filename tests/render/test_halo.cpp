// M-HALO 54b: the ice halo's CPU twin (mce/render/halo.h), the helpers test_render3d_halo_gl compares the sky shader's
// halo_term with: the uniforms and their gate, the ring's profile (its peak at 22 degrees, a sharp inner and a soft
// outer edge, the pixel floor of the inner edge, the dispersion), the sun dog centres (22 degrees from the sun on its
// horizon circle, farther apart in azimuth as the sun climbs) and profile, the veil, the gates (no cirrus, the sun
// down, the moon holding the light, a coarse pixel) and the caps.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "mce/render/halo.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;
using math::vec3;

namespace {

Halo on(f32 strength = 1.0f) {
    Halo h;
    h.strength = strength;
    return h;
}

constexpr f32 kDeg = 0.017453292f;
constexpr f32 kNan = std::numeric_limits<f32>::quiet_NaN();

vec3 dir(f32 elevation_deg, f32 azimuth_deg) {
    const f32 e = elevation_deg * kDeg, a = azimuth_deg * kDeg;
    return vec3{std::cos(e) * std::sin(a), std::sin(e), -std::cos(e) * std::cos(a)};
}

f32 dot3(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

f32 angle(vec3 a, vec3 b) {
    const f32 c = dot3(a, b);
    return std::acos(c < 1.0f ? (c > -1.0f ? c : -1.0f) : 1.0f);
}

// The ray `theta` radians from `center` toward `toward` (rotated in their common plane).
vec3 rotate_toward(vec3 center, vec3 toward, f32 theta) {
    const f32 k = dot3(toward, center);
    vec3 t{toward.x - center.x * k, toward.y - center.y * k, toward.z - center.z * k};
    t = math::normalize(t);
    const f32 c = std::cos(theta), s = std::sin(theta);
    return vec3{center.x * c + t.x * s, center.y * c + t.y * s, center.z * c + t.z * s};
}

HaloLight day_light() {
    HaloLight l;
    l.light = vec3{2.0f, 1.92f, 1.8f};
    return l;
}

bool all_zero(const HaloParams& p) {
    return p.strength == 0.0f && p.sun_dogs == 0.0f && p.moon == 0.0f && p.dispersion == 0.0f;
}

}  // namespace

TEST_CASE("halo twin: the gate: strength 0, NaN, disabled and the Low tier give all-zero uniforms") {
    CHECK(all_zero(halo_shader_params(Halo{})));  // the default carrier: strength 0
    CHECK(all_zero(halo_shader_params(on(kNan))));
    CHECK(all_zero(halo_shader_params(on(-1.0f))));
    Halo off = on(1.0f);
    off.enabled = false;
    CHECK(all_zero(halo_shader_params(off)));
    CHECK(all_zero(halo_shader_params(apply_quality(quality_settings(QualityTier::Low), on(1.0f)))));
    const HaloParams p = halo_shader_params(apply_quality(quality_settings(QualityTier::High), on(0.75f)));
    CHECK(p.strength == 0.75f);
    CHECK(p.sun_dogs == 1.0f);
    CHECK(p.moon == 0.5f);
    CHECK(p.dispersion == 1.0f);
    Halo wild = on(7.0f);
    wild.sun_dogs = kNan;
    wild.moon = -3.0f;
    wild.dispersion = 9.0f;
    const HaloParams q = halo_shader_params(wild);  // clamped again without apply_quality
    CHECK(q.strength == 1.0f);
    CHECK(q.sun_dogs == 1.0f);
    CHECK(q.moon == 0.0f);
    CHECK(q.dispersion == 1.0f);
    // Off: nothing anywhere.
    const vec3 sun = dir(12.0f, 0.0f);
    for (f32 t = 0.0f; t < 0.8f; t += 0.01f) {
        const vec3 d = rotate_toward(sun, vec3{0.0f, 1.0f, 0.0f}, t);
        const vec3 h = halo_term(d, sun, dot3(d, sun), 0.5f, 0.001f, HaloParams{}, day_light());
        CHECK((h.x == 0.0f && h.y == 0.0f && h.z == 0.0f));
    }
}

TEST_CASE("halo twin: the ring peaks at 22 degrees, sharp inside and soft outside, its edge at least 1.5 pixels") {
    CHECK(halo_ring(0.0f, 0.0f) == doctest::Approx(1.0f));
    CHECK(halo_ring(-kHaloInnerWidth - 1e-4f, 0.0f) == 0.0f);  // inside the edge: nothing (no darkening either)
    CHECK(halo_ring(-0.2f, 0.0f) == 0.0f);
    f32 prev = 1.0f;
    for (f32 x = 0.002f; x < 0.3f; x += 0.002f) {  // monotone glow outward
        const f32 r = halo_ring(x, 0.0f);
        CHECK(r < prev);
        CHECK(r > 0.0f);
        prev = r;
    }
    // Sharper on the inside: over the same 0.002 rad the inner edge falls far more than the outer glow.
    const f32 in = 1.0f - halo_ring(-0.002f, 0.0f), out = 1.0f - halo_ring(0.002f, 0.0f);
    CHECK(in > 5.0f * out);
    // The pixel floor: at 0.01 rad per pixel the inner edge is 1.5 pixels wide, not 0.004 rad.
    CHECK(halo_ring(-0.006f, 0.0f) == 0.0f);
    CHECK(halo_ring(-0.006f, 0.01f) > 0.1f);
    CHECK(halo_ring(-0.0151f, 0.01f) == 0.0f);
    // The peak per channel: red inside, blue outside with dispersion 1; the same place with 0.
    const auto peak = [](int ch, f32 disp) {
        f32 best = 0.0f, at = 0.0f;
        for (f32 th = 0.35f; th < 0.42f; th += 0.0001f) {
            const vec3 c = halo_ring_rgb(th, 0.0f, disp);
            const f32 v = ch == 0 ? c.x : (ch == 1 ? c.y : c.z);
            if (v > best) {
                best = v;
                at = th;
            }
        }
        return at;
    };
    CHECK(peak(1, 1.0f) == doctest::Approx(kHaloRadius).epsilon(0.002));
    CHECK(peak(0, 1.0f) < peak(1, 1.0f) - 0.003f);
    CHECK(peak(2, 1.0f) > peak(1, 1.0f) + 0.005f);
    CHECK(peak(0, 0.0f) == doctest::Approx(peak(1, 0.0f)).epsilon(0.001));
    CHECK(peak(2, 0.0f) == doctest::Approx(peak(1, 0.0f)).epsilon(0.001));
    const vec3 neutral = halo_ring_rgb(kHaloRadius + 0.02f, 0.0f, 0.0f);  // warm white: weighted, same shape
    CHECK(neutral.y == doctest::Approx(kHaloGreenWeight * neutral.x));
    CHECK(neutral.z == doctest::Approx(kHaloBlueWeight * neutral.x));
}

TEST_CASE("halo twin: the sun dogs sit 22 degrees from the sun on its horizon circle, farther in azimuth when high") {
    f32 prev_az = 0.0f;
    for (f32 el : {0.0f, 5.0f, 15.0f, 30.0f, 45.0f, 60.0f}) {
        const vec3 sun = dir(el, 10.0f);
        for (f32 side : {-1.0f, 1.0f}) {
            const vec3 dog = halo_sun_dog_center(sun, side);
            CHECK(dot3(dog, dog) == doctest::Approx(1.0f).epsilon(1e-5));
            CHECK(angle(dog, sun) == doctest::Approx(kHaloRadius).epsilon(0.003));
            CHECK(dog.y == doctest::Approx(sun.y).epsilon(1e-5));  // the sun's elevation
        }
        const vec3 l = halo_sun_dog_center(sun, -1.0f), r = halo_sun_dog_center(sun, 1.0f);
        const f32 az = std::acos(dot3(math::normalize(vec3{l.x, 0.0f, l.z}), math::normalize(vec3{r.x, 0.0f, r.z})));
        if (el == 0.0f) CHECK(az == doctest::Approx(2.0f * kHaloRadius).epsilon(0.003));
        CHECK(az >= prev_az);
        prev_az = az;
        // Symmetric about the sun's azimuth.
        const f32 sa = std::atan2(sun.x, -sun.z), la = std::atan2(l.x, -l.z), ra = std::atan2(r.x, -r.z);
        CHECK(sa - la == doctest::Approx(ra - sa).epsilon(1e-3));
    }
    const vec3 zenith = halo_sun_dog_center(vec3{0.0f, 1.0f, 0.0f}, 1.0f);  // a degenerate sun: finite
    CHECK(std::isfinite(zenith.x));
    CHECK(std::isfinite(zenith.z));
    // The profile: 1 at the centre, a sharp inside (toward the sun) and a longer tail outside.
    CHECK(halo_dog_profile(0.0f, 0.0f, 0.0f) == doctest::Approx(1.0f));
    CHECK(halo_dog_profile(-0.01f, 0.0f, 0.0f) < 0.5f * halo_dog_profile(0.01f, 0.0f, 0.0f));
    CHECK(halo_dog_profile(0.0f, 0.03f * 0.03f, 0.0f) < 0.2f);
    // The gain: full at a low sun, a quarter at 0.8, none above about 60 degrees.
    CHECK(halo_dog_gain(0.0f) == 1.0f);
    CHECK(halo_dog_gain(0.2f) > halo_dog_gain(0.5f));
    CHECK(halo_dog_gain(0.9f) == 0.0f);
}

TEST_CASE("halo twin: the term: through the cirrus only, capped, the sun's gate, the moon's ring, the pixel fade") {
    const HaloParams p = halo_shader_params(on(1.0f));
    const HaloLight light = day_light();
    const vec3 sun = dir(12.0f, 0.0f);
    const vec3 up{0.0f, 1.0f, 0.0f};
    const auto at = [&](f32 theta, f32 opacity, f32 pix = 0.001f, const HaloLight& l = day_light(), vec3 s = {}) {
        const vec3 c = s.y != 0.0f || s.x != 0.0f || s.z != 0.0f ? s : sun;
        const vec3 d = rotate_toward(c, up, theta);
        return halo_term(d, c, dot3(d, c), opacity, pix, p, l);
    };
    // The veil: none with no cirrus, more on a thick streak than a thin one, full at the layer's cap.
    CHECK(halo_veil(0.0f, 0.5f) == 0.0f);
    CHECK(halo_veil(0.5f, 0.5f) == 1.0f);
    CHECK(halo_veil(0.9f, 0.5f) == 1.0f);
    CHECK(halo_veil(0.25f, 0.5f) == doctest::Approx(0.5f));
    // Near the horizon the streaks' own fade (smoothstep(0.02, 0.2, dy) in the opacity) is undone and a shorter one
    // applies: a streak at the cap 4 degrees up (opacity 0.5 times that fade) still gives most of the veil.
    const f32 dy = std::sin(4.0f * kDeg);
    const f32 ft = (dy - 0.02f) / 0.18f, fade = ft * ft * (3.0f - 2.0f * ft);
    CHECK(halo_veil(0.5f * fade, dy) > 0.5f);
    CHECK(halo_veil(0.5f * fade, dy) < 1.0f);
    CHECK(halo_veil(0.5f, 0.02f) == 0.0f);
    CHECK(at(kHaloRadius, 0.0f).y == 0.0f);
    CHECK(at(kHaloRadius, 0.1f).y < at(kHaloRadius, 0.4f).y);
    // The radial peak is at 22 degrees; nothing inside the ring (the sun's own glow is left alone).
    f32 best = 0.0f, best_t = 0.0f;
    for (f32 t = 0.05f; t < 0.9f; t += 0.0005f) {
        const vec3 h = at(t, 0.5f);
        CHECK(h.x <= kHaloRingCap * light.light.x + 1e-6f);  // the cap (the dogs are on the horizon circle, not here)
        if (h.y > best) {
            best = h.y;
            best_t = t;
        }
        if (t < kHaloRadius - 0.02f) CHECK(h.y == 0.0f);
    }
    CHECK(best_t == doctest::Approx(kHaloRadius).epsilon(0.01));
    CHECK(best > 0.1f);
    // The dogs: brighter than the ring at a low sun, dimmer at a high one; capped.
    const auto dog_at = [&](f32 el) {
        const vec3 s = dir(el, 0.0f);
        const vec3 dog = halo_sun_dog_center(s, 1.0f);
        return halo_term(dog, s, dot3(dog, s), 0.5f, 0.001f, p, light);
    };
    CHECK(dog_at(14.0f).y > 1.5f * best);
    CHECK(dog_at(14.0f).y > dog_at(40.0f).y);
    CHECK(dog_at(14.0f).y <= (kHaloRingCap + kHaloDogCap) * light.light.y);
    // The sun down: none; a coarse pixel: none.
    CHECK(at(kHaloRadius, 0.5f, 0.001f, light, dir(-4.0f, 0.0f)).y == 0.0f);
    CHECK(at(kHaloRadius, 0.5f, kHaloPixFadeHi).y == 0.0f);
    CHECK(at(kHaloRadius, 0.5f, 0.02f).y < at(kHaloRadius, 0.5f, 0.002f).y);
    // The moon: its own ring by night, fainter than the sun's by day; none while the fade is 0 or with no moon.
    HaloLight night;
    night.sun_disk = 0.0f;  // the kit drops the disk while the moon holds the light
    night.light = vec3{0.0275f, 0.0325f, 0.05f};
    night.moon_intensity = 6.0f;
    night.night_fade = 1.0f;
    night.moon_dir = dir(15.0f, 0.0f);
    night.moon_tint = vec3{0.9f, 0.93f, 1.0f};
    const vec3 under = dir(-30.0f, 180.0f);  // the night key from under the horizon behind
    const vec3 moon_ring = at(kHaloRadius, 0.5f, 0.001f, night, night.moon_dir);
    (void)under;
    const vec3 dm = rotate_toward(night.moon_dir, up, kHaloRadius);
    const vec3 m = halo_term(dm, under, dot3(dm, under), 0.5f, 0.001f, p, night);
    CHECK(m.y > 0.0f);
    CHECK(m.y < best);
    CHECK(m.y <= kHaloMoonCap * night.moon_tint.y * night.moon_intensity);
    HaloLight day_fade = night;
    day_fade.night_fade = 0.0f;
    CHECK(halo_term(dm, under, dot3(dm, under), 0.5f, 0.001f, p, day_fade).y == 0.0f);
    // The moon holding the light (the kit's night hand-off): no sun ring and dogs around it, only the moon's ring.
    CHECK(moon_ring.y == doctest::Approx(m.y).epsilon(1e-5));
    HaloParams no_moon = p;
    no_moon.moon = 0.0f;
    CHECK(halo_term(dm, under, dot3(dm, under), 0.5f, 0.001f, no_moon, night).y == 0.0f);
}

namespace {

// The kit's moon hand-off (kit3d_extract.cpp, M-NIGHTSKY 14b) for a sun at height sun_y with intensity sun_i and a moon
// of light intensity moon_i at level `level`: the light the renderer gets (toward it, its colour times intensity) and
// the Sky copy's disk (0 while the moon holds the light, 14e).
struct KitLight {
    vec3 toward;
    HaloLight l;
    bool moon = false;
};
KitLight kit_light(f32 sun_y, f32 sun_i, f32 moon_i, f32 level, vec3 moon_dir) {
    KitLight k;
    const f32 t = std::fmin(std::fmax((sun_y + 0.05f) / 0.15f, 0.0f), 1.0f);
    const f32 sw = t * t * (3.0f - 2.0f * t);
    const f32 mw = (1.0f - sw) * level;
    const f32 c = std::sqrt(std::fmax(1.0f - sun_y * sun_y, 0.0f));
    k.toward = vec3{0.0f, sun_y, -c};
    k.l.light = vec3{1.0f * sun_i, 0.96f * sun_i, 0.9f * sun_i};
    if (mw > 0.0f && sw * sun_i < mw * moon_i) {
        k.toward = moon_dir;
        k.l.light = vec3{0.55f * moon_i * mw, 0.65f * moon_i * mw, 1.0f * moon_i * mw};
        k.l.sun_disk = 0.0f;
        k.moon = true;
    }
    return k;
}

}  // namespace

TEST_CASE("halo twin: no sun ring or dogs round a moon that holds the light, whatever the night sky's uniforms say") {
    const HaloParams p = halo_shader_params(on(1.0f));
    const vec3 moon = dir(20.0f, 30.0f);
    // The four setups where the night sky's uniforms are zero while the kit hands the light to the moon: r.nightsky 0
    // and a Moon without a disk (moon intensity 0), a night fade of 0 (dense fog, a full cloud cover), a sky texture
    // (the kit never fills the NightSky: the default moon direction, intensity 0). The disk is 0 in all of them.
    HaloLight base;
    base.light = vec3{0.165f, 0.195f, 0.3f};  // the moon's light, intensity 0.3
    base.sun_disk = 0.0f;
    HaloLight setups[4] = {base, base, base, base};
    setups[0].moon_dir = moon;  // r.nightsky 0: the direction kept, the intensity zeroed
    setups[1].moon_dir = moon;  // a Moon with disk_intensity 0
    setups[1].moon_tint = vec3{0.9f, 0.93f, 1.0f};
    setups[2].moon_dir = moon;  // night fade 0: night_sky_params leaves p_night zero
    setups[2].moon_tint = vec3{0.9f, 0.93f, 1.0f};
    // setups[3]: the defaults (no NightSky at all)
    u32 probes = 0;
    for (const HaloLight& l : setups)
        for (f32 t = 0.0f; t < 0.8f; t += 0.002f)
            for (f32 side : {-1.0f, 1.0f}) {
                const vec3 d = rotate_toward(moon, vec3{side, 0.3f, 0.0f}, t);
                const vec3 h = halo_term(d, moon, dot3(d, moon), 0.5f, 0.001f, p, l);
                CHECK((h.x == 0.0f && h.y == 0.0f && h.z == 0.0f));
                ++probes;
            }
    CHECK(probes > 3000);
    // The control: the same light with a disk is a sun, and rings.
    HaloLight sun = base;
    sun.sun_disk = 12.0f;
    const vec3 d = rotate_toward(moon, vec3{0.0f, 1.0f, 0.0f}, kHaloRadius);
    CHECK(halo_term(d, moon, dot3(d, moon), 0.5f, 0.001f, p, sun).y > 0.0f);
}

TEST_CASE("halo twin: the sun term is 0 wherever the kit can hand the light to the moon, so it never pops at the switch") {
    const HaloParams p = halo_shader_params(on(1.0f));
    // Anywhere the kit's sun weight is under 1 (sun_y < 0.10) the sun term is exactly 0.
    for (f32 y = -0.2f; y < kHaloSunGateLo; y += 0.001f) {
        const f32 c = std::sqrt(1.0f - y * y);
        const vec3 s{0.0f, y, -c};
        const vec3 dog = halo_sun_dog_center(s, 1.0f);
        const vec3 up = rotate_toward(s, vec3{0.0f, 1.0f, 0.0f}, kHaloRadius);
        CHECK(halo_term(dog, s, dot3(dog, s), 0.5f, 0.001f, p, day_light()).y == 0.0f);
        CHECK(halo_term(up, s, dot3(up, s), 0.5f, 0.001f, p, day_light()).y == 0.0f);
    }
    // A dusk through the hand-off band for the reviewed pairs (sun 3 / moon 0.3, sun 1 / moon 2, the moon's maximum 2
    // against a faint sun 0.2): the halo at the ring's top and at a dog of the sun's natural position, step by step. It
    // is 0 on both sides of the switch, and no step changes it by more than a smooth fade allows.
    const vec3 moon = dir(20.0f, 30.0f);
    const f32 pairs[3][2] = {{3.0f, 0.3f}, {1.0f, 2.0f}, {0.2f, 2.0f}};
    for (const auto& pr : pairs) {
        f32 prev_ring = 0.0f, prev_dog = 0.0f, worst = 0.0f, peak = 0.0f;
        bool switched = false, first = true;
        for (f32 y = 0.3f; y > -0.1f; y -= 0.0025f) {
            const KitLight k = kit_light(y, pr[0], pr[1], 1.0f, moon);
            const f32 c = std::sqrt(1.0f - y * y);
            const vec3 s{0.0f, y, -c};  // the sun's natural direction: where its ring would be
            const vec3 dog = halo_sun_dog_center(s, 1.0f);
            const vec3 up = rotate_toward(s, vec3{0.0f, 1.0f, 0.0f}, kHaloRadius);
            const f32 ring = halo_term(up, k.toward, dot3(up, k.toward), 0.5f, 0.001f, p, k.l).y;
            const f32 dg = halo_term(dog, k.toward, dot3(dog, k.toward), 0.5f, 0.001f, p, k.l).y;
            if (k.moon) {
                switched = true;
                CHECK(prev_ring == 0.0f);  // already 0 the step before the switch
                CHECK(prev_dog == 0.0f);
            }
            if (!first) worst = std::fmax(worst, std::fmax(std::fabs(ring - prev_ring), std::fabs(dg - prev_dog)));
            first = false;
            peak = std::fmax(peak, std::fmax(ring, dg));
            prev_ring = ring;
            prev_dog = dg;
        }
        MESSAGE("sun " << pr[0] << " / moon " << pr[1] << ": peak " << peak << ", largest step " << worst);
        CHECK(switched);
        CHECK(peak > 0.0f);
        CHECK(worst < 0.06f * peak + 1e-6f);  // a 0.0025 step of a fade over 0.1: about 4 % at its steepest
    }
}
