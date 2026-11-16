#pragma once
// M-HALO 54b: the pure CPU twin of the sky's ice-crystal halo (`halo_term` in MCE_GLSL_HALO, added by the cirrus
// layer's cirrus_layer after the streak composite, so kSkyFS's apply_cirrus draws it and kWaterFS's water_sky reflects
// it) and of the upload that feeds it (p_halo, forward3d.cpp's halo_params, which calls halo_shader_params). A 22
// degree ring around the sun (a sharp inner edge, a soft exponential outer one, a faint dispersion: red inside, blue
// outside), two sun dogs on the sun's horizon circle and a fainter ring around the moon, all scaled by the cirrus
// opacity at the pixel (no cirrus, no halo). f32 only, in the shader's evaluation order; no rand, no state, no time.
// Included by halo.cpp, forward3d.cpp and the tests only.
//
// This header must stay narrow: it does not include scene3d.h (a wide header); render::Halo is forward-declared.

#include "mce/math/math.h"
#include "mce/types.h"

namespace mce::render {

struct Halo;

// The ring's radius: 22 degrees (rad) and its cosine.
inline constexpr f32 kHaloRadius = 0.38397244f;
inline constexpr f32 kHaloCosRadius = 0.92718385f;
// The sharp inner edge: a smoothstep over this half-width (rad), floored at kHaloEdgePixels pixels (antialiasing).
inline constexpr f32 kHaloInnerWidth = 0.004f;
inline constexpr f32 kHaloEdgePixels = 1.5f;
// The soft outer edge: exp(-x / kHaloOuterWidth) past the peak (rad; about 2 degrees).
inline constexpr f32 kHaloOuterWidth = 0.035f;
// The dispersion: the red channel's profile sits this far inside the green one, the blue one this far outside (rad,
// times Halo::dispersion); the channels are weighted (1, kHaloGreenWeight, kHaloBlueWeight) so the ring is warm white.
inline constexpr f32 kHaloShiftRed = 0.004f;
inline constexpr f32 kHaloShiftBlue = 0.006f;
inline constexpr f32 kHaloGreenWeight = 0.9f;
inline constexpr f32 kHaloBlueWeight = 0.8f;
// A sun dog: a Gaussian blob, sharp toward the sun (kHaloDogInner rad, floored at kHaloEdgePixels pixels), a longer
// tail away from it (kHaloDogOuter) and kHaloDogAcross rad high across the radial direction.
inline constexpr f32 kHaloDogInner = 0.008f;
inline constexpr f32 kHaloDogOuter = 0.045f;
inline constexpr f32 kHaloDogAcross = 0.022f;
// The gains (the share of the light's colour added at the profile's peak, before the veil and the strength) and the
// caps (the most the term adds, as a share of that light).
inline constexpr f32 kHaloRingGain = 0.4f;
inline constexpr f32 kHaloRingCap = 0.6f;
inline constexpr f32 kHaloDogGain = 2.0f;  // 5 x the ring at a low sun
inline constexpr f32 kHaloDogCap = 2.0f;
inline constexpr f32 kHaloMoonGain = 0.006f;
inline constexpr f32 kHaloMoonCap = 0.08f;
// The cosine under which a ray is too far from the sun or the moon for any term (the shader's acos gate, 53 degrees),
// and the one under which a ray is too far from a dog centre (11.5 degrees).
inline constexpr f32 kHaloMinCos = 0.6f;
inline constexpr f32 kHaloDogMinCos = 0.98f;
// The sun term's elevation gate (sin of the elevation): 0 at or under kHaloSunGateLo, full from kHaloSunGateHi. The kit
// (kit3d_extract.cpp, M-NIGHTSKY 14b) may hand the directional light to the moon anywhere its sun weight
// smoothstep(-0.05, 0.10, elevation) is under 1, so the sun term is 0 over that whole band and cannot pop at the switch;
// a change of the kit's band goes here too.
inline constexpr f32 kHaloSunGateLo = 0.10f;
inline constexpr f32 kHaloSunGateHi = 0.20f;
// The pixel footprint fade: the whole halo fades out between these radians per pixel (a very coarse render shows a
// soft ring or none, never a crawling one).
inline constexpr f32 kHaloPixFadeLo = 0.01f;
inline constexpr f32 kHaloPixFadeHi = 0.03f;

// The halo as the shader sees it: p_halo = (strength, sun_dogs, moon, dispersion). All zero when it is off (the shader
// then skips it).
struct HaloParams {
    f32 strength = 0.0f;    // 0..1; 0 = off
    f32 sun_dogs = 0.0f;    // 0..1
    f32 moon = 0.0f;        // 0..1
    f32 dispersion = 0.0f;  // 0..1
};

// The uniforms for carrier `h`. Off (all zero) for `enabled` false or a strength that is not above 0 (NaN included).
// The rest is clamped again as apply_quality does (a caller may skip it): all to 0..1, NaN to the defaults (sun_dogs 1,
// moon 0.5, dispersion 1).
HaloParams halo_shader_params(const Halo& h);

// The ring's profile at radial offset x = theta - kHaloRadius (rad), `pix` the radians per pixel there:
// smoothstep(-wi, 0, x) * exp(-max(x, 0) / kHaloOuterWidth), wi = max(kHaloInnerWidth, kHaloEdgePixels * pix).
// 1 at the peak (x = 0), 0 well inside.
f32 halo_ring(f32 x, f32 pix);

// The ring per channel at angle `theta` from its centre: (ring(x + kHaloShiftRed d), kHaloGreenWeight ring(x),
// kHaloBlueWeight ring(x - kHaloShiftBlue d)), x = theta - kHaloRadius, d the dispersion (0..1).
math::vec3 halo_ring_rgb(f32 theta, f32 pix, f32 dispersion);

// The sun dog centre on side `side` (+1 or -1) of unit sun direction `toward_sun`: on the sun's horizon circle
// (elevation e, sin e = sun.y) at the azimuth offset dphi from the sun with cos dphi = (cos 22 deg - sin^2 e) / cos^2 e
// (clamped to -1..1), so it is exactly 22 degrees from the sun at any elevation: cos e (cos dphi h + side sin dphi t) +
// sin e up, h the sun's horizontal unit direction (+x for a sun at the zenith), t = (-h.z, 0, h.x).
math::vec3 halo_sun_dog_center(math::vec3 toward_sun, f32 side);

// A dog's profile at offset `along` (rad, positive away from the sun) and squared offset `across2` across it:
// exp(-along^2 / w^2 - across2 / kHaloDogAcross^2), w = max(kHaloDogInner, kHaloEdgePixels * pix) toward the sun
// (along < 0) and kHaloDogOuter away from it.
f32 halo_dog_profile(f32 along, f32 across2, f32 pix);

// The dog centred at `dog` along view ray d per channel: o = d - dog, r the unit direction away from the sun at the
// dog, along = dot(o, r), across2 = max(dot(o, o) - along^2, 0); (profile(along + kHaloShiftRed disp), kHaloGreenWeight
// profile(along), kHaloBlueWeight profile(along - kHaloShiftBlue disp)).
math::vec3 halo_dog_rgb(math::vec3 d, math::vec3 dog, math::vec3 toward_sun, f32 pix, f32 dispersion);

// The dogs' gain for a sun at height sun_y (sin of its elevation): mix(1, 0.25, smoothstep(0, 0.8, sun_y)) * (1 -
// smoothstep(0.7, 0.87, sun_y)): full at a low sun, a quarter when it is high, gone above about 60 degrees.
f32 halo_dog_gain(f32 sun_y);

// The veil along a ray climbing `dy` (sin of its elevation): the cirrus opacity at the pixel without the streaks' own
// horizon fade, scaled to 0..1 (the layer's cap 0.5 gives 1), with a shorter fade of its own: clamp(2 a /
// max(smoothstep(0.02, 0.2, dy), 1e-3), 0, 1) * smoothstep(0.02, 0.1, dy). 0 where there is no cirrus, so a clear sky
// or a gap between the streaks shows no halo; the dogs of a low sun still show.
f32 halo_veil(f32 opacity, f32 dy);

// The frame's light and moon as the shader reads them: p_light_color, p_sun.y (the sun disk's intensity; 0 = no disk,
// which is how the kit marks the moon holding the light), p_night.y (moon intensity, 0 = no moon), p_night.w (night
// fade), p_moon.xyz (unit, toward the moon), p_moon2.rgb (the moon's tint).
struct HaloLight {
    math::vec3 light{0.0f, 0.0f, 0.0f};
    f32 sun_disk = 12.0f;  // Sky::sun_intensity's default
    f32 moon_intensity = 0.0f;
    f32 night_fade = 0.0f;
    math::vec3 moon_dir{0.0f, 1.0f, 0.0f};
    math::vec3 moon_tint{0.0f, 0.0f, 0.0f};
};

// The colour the halo adds along view ray d (the shader's halo_term): `toward_sun` the unit direction toward the light
// (-p_light_dir), cs = dot(d, toward_sun), `opacity` the cirrus opacity at the pixel, `pix` the radians per pixel.
//   amp  = strength * halo_veil(opacity, d.y) * (1 - smoothstep(kHaloPixFadeLo, kHaloPixFadeHi, pix))
//   sun  gate = smoothstep(kHaloSunGateLo, kHaloSunGateHi, sun.y), 0 without a sun disk (sun_disk 0: the moon holds the
//        light);
//        for cs > kHaloMinCos, theta = acos(cs): min(light ring_rgb(theta) kHaloRingGain amp gate smoothstep(0.1, 0.4,
//        theta), kHaloRingCap light) plus, per dog with dot(d, dog) > kHaloDogMinCos, min(light dog_rgb kHaloDogGain
//        sun_dogs halo_dog_gain(sun.y) amp gate, kHaloDogCap light)
//   moon for moon on, night fade > 0, `moon` > 0 and dot(d, moon) > kHaloMinCos: M = tint * intensity, min(M
//        ring_rgb(theta_moon, dispersion / 2) kHaloMoonGain amp moon fade smoothstep(0.1, 0.4, theta_moon),
//        kHaloMoonCap M)
// Zero when the params are off or the opacity is 0.
math::vec3 halo_term(math::vec3 d, math::vec3 toward_sun, f32 cs, f32 opacity, f32 pix, const HaloParams& p,
                     const HaloLight& l);

}  // namespace mce::render
