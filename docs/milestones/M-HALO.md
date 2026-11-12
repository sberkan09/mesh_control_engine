# M-HALO — Ice-crystal halo and sun dogs seen through the cirrus layer (milestone record)

**Status:** in progress (started 2026-11-12), steps 54a to 54d. 54a (this plan and the `render::Halo` seam) and 54b
(the sky shader and its CPU twin) are done; 54c (kit, extract, Luau) and 54d (sample, guide, close) are planned. **Owner:
routine 2 thread (the M-VISUAL line).** Follow-up of M-CIRRUS (whose "not in scope" list names "halos and sun dogs, an
ice-crystal optics follow-up"), M-RAINBOW (a sky optic that is a pure function of the angle between the view ray and a
reference direction) and the M-AURORA / M-RAINBOW / M-METEOR / M-CIRRUS split (plan and seam, shader, kit, sample and
close), which it copies. No Linear issues (free issue limit); this record is the tracker. The shader step is
Opus-written and merged after an independent Opus review plus a green `--task` (as M-CLOUDS 9b, M-AURORA 16c, M-RAINBOW
17c and M-CIRRUS 53b).

**Naming:** no type, file, cvar or doc line called `Halo`, `M-HALO` or `r.halo` existed before 54a (`src/`, `docs/`,
`tests/`, `samples/`, `scripts/` checked 2026-11-12 with a case-insensitive grep for `halo`). The lowercase word does
occur, and none of it collides: `LensFlare::halo` / `halo_radius` (the screen-space glare of M-LENSFLARE, in
`scene3d.h`, `lens_flare.h`, `kit/lens_flare.h`, `quality.cpp`, `forward3d.cpp`, `shaders3d.h`), the `-halo` token of
the OBJ material importer, and prose in M-HEATHAZE, M-NIGHTSKY, M-SUNSHAFTS, `docs/research` and `docs/tech`. Those are
members or words, not a type, so `render::Halo` (the carrier, 54a), `kit::Halo` (the component, 54c; a different
namespace, as `render::Cirrus` / `kit::Cirrus`), `Scene3D::set_halo()` and the cvar `r.halo` do not clash; the docs and
the Luau API say "ice halo" where the lens-flare glare could be meant, and the extract names the carrier
`render::Halo` explicitly.

**Goal.** Thin ice cloud does more than streak: its hexagonal crystals refract the sun's light into a ring 22 degrees
from the sun (sharp on the inside, a soft glow outside, with a faint colour fringe, red inside and blue outside) and
into two bright patches on the sun's horizon line at the same 22 degrees (sun dogs, parhelia), brighter and sharper
when the sun is low. The moon makes the same ring, much fainter. M-HALO adds exactly these, and nothing else, as an
extra term of the cirrus layer: a 22 degree ring and two sun dogs around the sun and a fainter ring around the moon,
visible only through the cirrus (the strength is scaled by the cirrus coverage and by the local streak opacity, so a
clear sky, the Low tier, `r.cirrus 0` or `r.halo 0` shows none), computed in the sky pass from the angle between the
view ray and the sun or moon direction with a cheap analytic profile, composited inside `apply_cirrus` so the low cloud
layer still covers it and the fog still hides it. Opt-in and deterministic; a scene without it renders exactly as today.

**Not in scope:** circumscribed and upper tangent arcs, the 46 degree halo, light pillars, parry arcs and every other
crystal-orientation effect (one ring and two dogs only), a real ray-traced crystal model (the profile is analytic and
fixed), volumetric clouds, a halo in the IBL bake, reflection probes or GI captures (`sky_radiance()` carries neither
cloud layer nor this, see M-CIRRUS), a halo under an equirect `sky_texture` (the texture wins), a halo through the low
cloud layer where it is thick (the low layer composites over it), several suns, changing `Weather`, `Cirrus`,
`CloudLayer`, `WorldClock`, `Moon`, `NightSky` or `Environment3D` bytes, and Metal / WebGPU pixels in this milestone
(the shader text goes through the existing translation tests only).

## Design

- **Carrier (seam, 54a).** `render::Halo {strength, sun_dogs, moon, dispersion, enabled, pad_[3]}` in `scene3d.h` right
  after `render::Cirrus`, `Scene3D::set_halo()` / `halo()`, `QualitySettings::halo` (Low off, Medium and High on), the
  saved cvar `r.halo` and a by-value `apply_quality(q, halo)` overload, exactly the shape of M-CIRRUS 53a. Defaults:
  `strength` 0 (off), `sun_dogs` 1, `moon` 0.5, `dispersion` 1, `enabled` true. Clamps (NaN to the default): all four
  to 0..1 (strength NaN 0); `enabled` passes through; `strength` is zeroed by the Low tier and `r.halo 0`. In-range
  values pass bit for bit, so the default carrier is bit-identical before and after `apply_quality` (tested). 20 bytes,
  no implicit padding (`static_assert` in the test). The carrier holds no direction: the sun direction and the moon
  direction (`p_moon.xyz`) are already in the frame's uniforms, and the cirrus coverage is already in `p_cirrus`.
  `scene3d.h` and `quality.h` are wide headers (`include_fanout.py` on 2026-11-12: `scene3d.h` at most 241 of 1075
  TUs, `quality.h` 156; both over the 150 warning, and both already so before this change at 235 / 151), which is why
  the carrier ships alone, before any user, as this seam: 19 and 5 added lines.
- **Sky shader (Opus, 54b).** The halo is a term inside the shared cirrus piece (`cirrus_layer`, after the streak
  composite), behind `p_cirrus.x > 0.0` (the cirrus off-check) and `p_halo.x > 0.0`, so a frame with either at 0 is
  the frame without it bit for bit. Notation: `s` the unit sun direction, `d` the unit view ray, `theta = acos(dot(d,
  s))` (clamped dot; `acos` only near the ring, using `dot` thresholds away from it), `pix` the pixel's angle as
  `cirrus_layer` already takes it, `e` the sun's elevation (`s.y = sin e`).
  - Veil factor (as built in 54b; the plan's `p_cirrus.x * (0.5 + 0.5 * a / amax)` kept half the ring in the gaps):
    `veil = clamp(2 a / smoothstep(0.02, 0.2, d.y), 0, 1) * smoothstep(0.02, 0.1, d.y)`, `a` the streak opacity the
    layer just computed (cap 0.5), so the halo shows only through the streaks (none in a gap, none with coverage 0) and
    the streaks' own long horizon fade is swapped for a shorter one, so the dogs of a sun a few degrees up still show.
  - Ring: `x = theta - t0`, `t0 = 22 deg = 0.3840 rad`; the profile `ring(x) = smoothstep(-wi, 0, x) * exp(-x / wo)` for
    the radial offset `x` (zero well inside, a sharp inner edge of half-width `wi = max(0.004, 1.5 * pix)` rad, an
    exponential soft outer edge `wo = 0.035` rad, about 2 degrees). Dispersion: the profile is evaluated per colour
    channel with `x_c = theta - t0 - k_c * dispersion`, `k = (-0.004, 0, +0.006)` rad for (r, g, b), so red peaks about
    0.3 degrees inside green and blue 0.35 degrees outside; the three channels are weighted `(1, 0.9, 0.8)` so the ring
    reads warm white at the edge with dispersion 0 and tinted at 1. There is no darkening inside the ring: the term is purely additive, so it cannot go negative or double-count, and the
    glow outside already makes the inside read darker.
  - Sun dogs: they sit on the sun's horizon circle (`elevation(d) = e`) at azimuth offset `dphi` from the sun with
    `cos dphi = (cos 22 deg - sin^2 e) / cos^2 e` (so `dphi = 22 deg` at the horizon and growing as the sun climbs; for
    `e` above about 60 degrees where the right side exceeds 1 it is clamped to 1 and the dogs sit on the ring). With
    `sh = normalize(vec3(s.x, 0, s.z))` and `t = vec3(-sh.z, 0, sh.x)` the two centers are `dog = cos e * (cos dphi * sh
    +- sin dphi * t) + sin e * up`. Each is a blob in `psi = acos(dot(d, dog))`: `exp(-(psi / 0.025)^2)` stretched
    1.6x away from the sun (a short tail on the outside, a sharp inside edge), the same per-channel dispersion shift
    along the sun direction (red toward the sun), gain `sun_dogs * mix(1.0, 0.25, smoothstep(0.0, 0.8, s.y))`: full at
    a low sun, a quarter when it is high. Sun dogs are 3 times the ring's peak at a low sun (they are the brightest
    part of a real display), before the global cap below. As built (54b): the blob is `exp(-along^2 / w^2 - across^2 /
    0.022^2)` in the radial (`along`, away from the sun) and cross directions at the dog, `w = max(0.008, 1.5 pix)`
    toward the sun and 0.045 away; the right side of the `cos dphi` formula never exceeds 1 (it falls as the sun
    climbs; it passes -1 only above 79 degrees), so it is clamped to -1..1 and the dogs fade out instead with `1 -
    smoothstep(0.7, 0.87, s.y)` (none above about 60 degrees, as real parhelia); the dogs' gain is 5 times the ring's
    (2.0 against 0.4) to read through the tonemap at a low sun.
  - Sun gating and level: `sun_gate = smoothstep(-0.05, 0.02, s.y)` (nothing once the sun is below the horizon),
    the sun's light colour from `p_light_color` times that gate, `strength * veil * sun_gate` as the amplitude. Added
    to `c` (emission, over the sky and the streaks, under the low layer and the fog), the amplitude scaled by
    `smoothstep(0.1, 0.4, theta)` (a ramp away from the sun's own glow), so the Mie glow near the sun
    is not added twice (risk below), and the added colour is capped at `0.25 * sun_light` per channel for the ring and
    `0.6 * sun_light` for a dog, so the halo never outshines the disk. As built (54b, tuned on 800 x 450 llvmpipe
    frames): the ring adds `0.4` of the light at its peak (cap 0.6), a dog `2.0` (cap 2.0). Review fix (54b follow-up):
    the moon holding the light is read from the disk, not from the night sky's uniforms: the kit drops the disk
    (`Sky::sun_intensity` 0, M-NIGHTSKY 14e) exactly when it hands the light to the moon, while `p_night` can be zero
    then (`r.nightsky 0`, a Moon without a disk, a night fade of 0, a sky texture), so `p_sun.y == 0` gates the sun
    term off (a sky authored without a disk therefore shows no sun halo either). And the kit may hand off anywhere its
    sun weight `smoothstep(-0.05, 0.10, elevation)` is under 1, at a point that depends on the moon's intensity, which
    the renderer does not know; so `sun_gate = smoothstep(0.10, 0.20, s.y)` (`kHaloSunGateLo` / `Hi`): the sun term is
    0 over the whole hand-off band and cannot pop at the switch (it used to vanish in one frame), at the cost of no sun
    halo for a sun under 5.7 degrees.
  - Moon ring: the same ring profile (no sun dogs) around `p_moon.xyz`, 22 degrees, with `moon * strength * veil *
    p_night.y (the moon intensity) * night_fade` as the amplitude (so no moon, a day sky or `NightSky` off draws none)
    and a cap at 0.08 of the moon's tint `p_moon2.rgb`; dispersion is halved (the moonlight is too faint to show colour).
    As built (54b): `M = p_moon2.rgb * p_night.y`, the term `0.006 * M * ring` times the amplitude, capped at `0.08 M`.
  - Fades: up-going rays only (`d.y > 0.02`, as the layer), `smoothstep(0.02, 0.2, d.y)` into the horizon haze like the
    streaks, and the ring and the dogs fade out where `pix` would make the sharp edge thinner than a pixel (`wi` is the
    floor, and the amplitude is multiplied by `1 - smoothstep(0.004, 0.012, pix)` below the ring's own width so a very
    coarse render shows a soft ring, not a crawling one). As built (54b): `1 - smoothstep(0.01, 0.03, pix)` (the
    plan's range would have dimmed the 160 x 120 test frames, 0.0063 rad per pixel, which the 1.5-pixel edge floor
    already keeps from crawling).
  - Order: unchanged from M-CIRRUS: `kSkyFS` applies `apply_cirrus` after the meteors and right before `apply_clouds`,
    so the low layer composites over the halo and the fog over both; the sun disk is under the cirrus and the halo
    adds around it. `water_sky` reaches it through the same `apply_cirrus` line, so the water reflects the halo with no
    second copy. `sky_radiance()` (the IBL) does not see it.
- **PassU budget (54b checks it first).** `Forward3DPassU` is 1536 bytes (96 `vec4`) with exactly **1** free
  (`free_[1]`: M-CIRRUS 53b took two of the earlier three). The halo fits in it: `p_halo` (x
  `strength` after `apply_quality`, y `sun_dogs`, z `moon`, w `dispersion`), the four carrier floats, nothing more; the
  directions, the cirrus coverage, the light colour and the moon's tint and intensity are existing uniforms. That
  takes the **last** free `vec4`: the size stays 1536 and the ring cost per upload is unchanged, but PassU then has 0
  free, and any later sky feature needs a growth seam (one more 256-byte slot, as M-AURORA 16c did) first. 54b updates
  the places that must agree in one PR: `MCE_GLSL_PASS_U`, `Forward3DPassU` (`free_[1]` goes away), the offset chain
  `static_assert` and its message, the size comment and the "free" count in the size `static_assert`'s message. If a
  fifth scalar turns out to be needed (a width scale), 54b stops and splits the growth seam instead of squeezing it
  into the pad of another field.
- **CPU twin (54b).** `mce/render/halo.h` (as `cirrus.h`): pure f32 helpers `halo_ring(f32 x, f32 pix, f32 channel_shift)`,
  `halo_sun_dog_center(vec3 sun, f32 sign) -> vec3`, `halo_dog_profile(f32 psi, f32 along)` and `halo_veil(f32
  coverage, f32 opacity)`, the same constants as the shader in a commented block; the upload does not need them, the
  tests do: `tests/render/test_halo.cpp` compares them against the shader at probe angles (the GL pixel test reads
  back a ray through the ring, a dog and a gap), and checks the formulas' properties below on the CPU with no GPU.
- **Kit (54c).** Opt-in component `Halo` (`mce/kit/halo.h`, one per scene, the first in ECS order is read, registered by
  `install_weather` next to `Cirrus`), all `Save` with a `Range`: `strength` 1 (0..1), `sun_dogs` 1 (0..1), `moon` 0.5
  (0..1), `dispersion` 1 (0..1). A pure helper `halo_level(const Halo&, f32 cirrus_coverage, f32 sun_elevation_deg) ->
  f32` (strength times a smoothstep that is 0 under 0.1 coverage and full from 0.5, so only a real veil of ice shows
  a ring, and exactly 0 for strength 0 or no cirrus; NaN falls back to the defaults). `Kit3DExtract::apply_environment`
  fills `render::Halo` next to the cirrus feed (`.cpp` only) from the helper with the carrier's `cirrus.coverage`,
  then `apply_quality`; nothing is set with no `Halo`, no `Cirrus`, no `Weather` or an equirect `sky_texture`; the
  carrier is written every frame (default when off) so a stale one never survives. Luau: `weather.set_halo{strength=,
  sun_dogs=, moon=, dispersion=}`, the authoring getters and a read-only `weather.halo_level()` from the same helper;
  named errors, clamped to the Range, nothing written on error, a missing `Halo` raises; typed in `definitions.cpp`,
  same rules as `weather.set_cirrus`.
- **Sample and goldens (54b, 54d).** `samples/12_weather` (it already has a `Cirrus` on its Weather entity, a clock and
  a front) gets a default `Halo` on the same entity (entity count unchanged) and its script logs `halo:` every 150
  ticks; only that sample's scene, script and test expectations move. Goldens (54b, GL and SDL GPU): `halo_sun`
  (day, a thick cirrus deck, a sun about 35 degrees up: the ring and the dogs), `halo_lowsun` (the sun 5 degrees up:
  the dogs at their brightest and widest apart in the frame) and `halo_moon` (night, the moon's ring); every
  existing golden (`cirrus_*`, `cloud_sky`, `night_sky`, the aurora, rainbow, meteor and `water_*` sets) stays
  byte-identical on both backends, because the halo is off at strength 0.
- **Determinism.** A scene without `Halo` is byte-identical. The halo has no time term (it is a pure function of the
  view ray, the sun and moon directions, the cirrus mask and four constants), so there is nothing to wrap and two
  runs agree; the cirrus drift moves the streaks, and with them the veil, deterministically. The extract is not part of
  the state hash.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 54a. Plan and seam | This record, the ROADMAP row, `render::Halo` in `scene3d.h` after `Cirrus`, `Scene3D::set_halo()` / `halo()`, `QualitySettings::halo` (Low off), `r.halo` (Saved), the by-value `apply_quality` overload with the clamps above; `tests/render/test_halo_quality.cpp` (defaults, a fresh `Scene3D` holds the default bit for bit, the defaults pass `apply_quality` bit for bit on every tier, no implicit padding (`static_assert`), set/get round trip, Low and `r.halo 0` zero the strength and leave the cirrus alone, `enabled` passes through, clamps and NaN); the changelog fragment. No shader change | render, tests/render, docs | Sonnet; the seam PR for `scene3d.h` and `quality.h` |
| 54b. Sky shader | Check the PassU budget first (the one free `vec4`, the size unchanged, 0 free after); `p_halo` in `Forward3DPassU` / `MCE_GLSL_PASS_U`, filled next to the cirrus params in `forward3d.cpp`; the halo term in `cirrus_layer` (inside `MCE_GLSL_CIRRUS`) behind the cirrus off-check and `p_halo.x > 0.0`, reflected by the water through `apply_cirrus`; CPU twin `mce/render/halo.h`; new goldens `halo_sun`, `halo_lowsun`, `halo_moon` on GL and SDL GPU, all existing goldens byte-identical on both backends; `test_render3d_halo_gl` pixel tests (strength 0, `enabled` false, coverage 0, the Low tier and `r.halo 0` equal the plain frame; only sky pixels above the horizon change; the ring's radial peak is at 22 degrees from the sun, sharper on the inside than the outside (the gradient inside is steeper), red inside and blue outside with dispersion 1 and neutral with 0; the dogs sit on the sun's horizon line, 22 degrees away at a low sun and farther at a high one, and are brighter at the low sun; no halo with cirrus coverage 0 and less in a gap between streaks than on one; the low layer in front hides it; the sun below the horizon draws none; the moon ring is fainter than the sun's and absent by day; the water reflects it and the water without a halo is unchanged; an origin shift leaves the sky unchanged; a one-degree yaw stays within a frame-difference bound, the crawl test, at 160 x 120 and 640 x 360; GLSL vs twin at probe angles) and `tests/render/test_halo.cpp`; `perf_run` at 1080p recorded where a GPU exists | render, tests/render, tests/render_gl | **Opus writes**, independent Opus review |
| 54c. Kit, extract, Luau | `Halo` component and `halo_level` in `mce/kit/halo.h` (registration in `install_weather`, zeroed pad, the `test_padding` entry, the `engine_components.cpp` includes), the `Kit3DExtract` feed (`.cpp` only) with the quality tiers through `apply_quality`, `weather.set_halo`, the getters and `weather.halo_level()` typed in `definitions.cpp`; tests in `tests/kit/test_halo.cpp` (helper range, 0 under thin cirrus, rises with coverage, strength 0 gives exactly 0, NaN fallbacks, save/load round trip, no `Halo` gives a bit-identical `Scene3D`, the extract's carrier equals the helper, Low tier zeroes it, `Weather` / `Cirrus` / `CloudLayer` bytes unchanged) and `tests/script/test_script_halo.cpp` (getters, clamping, named errors, nothing written on error, a missing `Halo` raises). Split into a kit PR and a script PR if the diff is large | kit, script, tests | Sonnet |
| 54d. Sample, guide, close | `samples/12_weather` carries a `Halo` on its Weather entity and logs `halo:` every 150 ticks; only that sample's goldens and log expectations move; the user guide's sky section (what the halo is, its defaults, the Low tier, why it needs cirrus, why no shadow and no IBL), the 27.3 and 08-3d sky rows, the changelog, this record's step status and the ROADMAP row to done; `capabilities.md` and `sample_coverage.allow` are the integration runner's to regenerate | samples, docs | Sonnet |

54b is the only step that writes a shader, so it is Opus-written with an independent Opus review (the author does not
review it). The review checklist: the PassU field agrees in every place and the size is 1536, both off-checks, the order
against the low layer and the fog, the `water_sky` path, the per-channel ring and the dog center formulas against the
twin, no `acos` of an unclamped dot, the additive caps, the pixel-footprint floor. Steps stack in order (54a, then 54b
and 54c in parallel, then 54d).

## Gates

- With strength 0 (the default, no `Halo`, `enabled` false, the Low tier or `r.halo 0`), or with cirrus coverage 0, every
  frame is bit-identical to the frame without the halo; every existing golden under `tests/render_gl/*` stays
  byte-identical through 54a and 54b on both backends, and every sample hash until 54d moves `12_weather` only.
- `Weather`, `Cirrus`, `CloudLayer`, `WorldClock`, `Moon`, `NightSky` and `Environment3D` are never written by halo code.
- PassU: the size stays 1536 bytes with 0 `vec4` free after 54b (the budget decision above), or 54b ships a growth seam
  first; no other pass's block changes.
- Include fan-out: the only wide headers touched are `scene3d.h` and `quality.h`, in this seam (54a); `halo.h` (kit) is
  included only by the extract, the install, the script binding and the tests; `render/halo.h` only by `forward3d.cpp`
  and the tests; `kit3d.h`, `weather.h`, `cirrus.h` and `cloud_layer.h` are not edited.
- Slow tests: a halo GPU test over 60 s ships a smoke variant and the long one is `mce_mark_slow`.

## Risks

- **Aliasing of the sharp ring.** The inner edge is the whole look and it is thinner than a pixel at low resolution
  and at 22 degrees on a wide field of view (about 1 pixel per 0.03 degrees at 4K and 90 degrees is not the problem;
  160 x 120 is). The edge half-width is floored at 1.5 `pix` and the amplitude fades when the pixel exceeds the ring's
  own width, as `rainbow_layer` band-limits its violet edge; the crawl test (a one-degree yaw, frame difference
  bound) at two resolutions is the gate, and MSAA or TAA are not assumed. The three dispersion channels are 0.3 to
  0.35 degrees apart, below a pixel at 1080p, so the fringe is a subpixel colour tilt and may vanish at low
  resolution (accepted: it fades with `pix` like the edge).
- **Double-counting with the sun glow.** The sky pass already adds a Mie-like glow and the disk around the sun, and
  the cirrus layer a forward-scatter silver term (`pow(max(cs, 0), 8)`). The ring is at 22 degrees where those are
  small, but a bright glow plus the ring could stack to a halo that looks painted on. Mitigations: purely additive and
  capped relative to the sun light (not relative to the pixel), scaled down near the glow, the sun disk untouched, and
  the golden review at a low sun, where the glow is widest, is the check; the strength default stays 0 (opt-in) so
  nothing else moves.
- **The veil factor reads the streak mask.** A thin cirrus deck breaks the ring into arcs along the streaks, which is
  right for cirrus and wrong for the cirrostratus veil real halos need. Accepted: the factor keeps 0.5 of its strength
  where there is any cirrus, so the ring stays mostly continuous at high coverage; a smooth veil mode is a follow-up.
- **The last free `vec4`.** See the PassU budget: the next sky feature pays for a growth seam.
- **The sun below the cirrus's own horizon.** The cirrus layer lights a bit longer after sunset (53b); the halo gate
  uses the sun's elevation only, so there is no halo in the afterglow (a real one is also gone by then).

## Limits (planned; 54b and 54d confirm or extend the list)

- Ring and dogs only: no 46 degree halo, no arcs, no pillars, no sub-suns.
- The halo is a function of the angle in the sky plane, not of the cirrus plane: it does not parallax with the streaks
  and does not follow the layer's height (correct: it is at infinity).
- One sun and one moon; the dogs follow the sun's elevation only (no crystal tilt statistics).
- No sun halo for a sun under 5.7 degrees (sin 0.10, the kit's moon hand-off band; full from 11.5 degrees), and none
  for a sky without a sun disk (the disk is the kit's "the moon holds the light" signal).
- No shadow, no ambient or IBL change.
- Desktop GL and SDL GPU pixels only; Metal and WebGPU see the shader through the translation tests.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 54a. Plan and seam | done | TBD | `render::Halo` (20 bytes, explicit `pad_[3]`) after `render::Cirrus` in `scene3d.h`, `Scene3D::set_halo()` / `halo()`, `QualitySettings::halo` (Low off), the saved cvar `r.halo`, `apply_quality(q, halo)` (clamps as in Design; in-range values bit for bit); `tests/render/test_halo_quality.cpp`. `include_fanout.py` 2026-11-12: `scene3d.h` at most 241 of 1075 TUs, `quality.h` 156 (both over the 150 warning: this is the seam PR; 235 / 151 before this step). PassU budget checked: 1 `vec4` free, the halo needs exactly 1 (`p_halo`), so the size stays 1536 and 0 are left. No shader change |
| 54b. Sky shader | done | TBD | `p_halo` (strength, sun dogs, moon, dispersion) takes PassU's last free `vec4` (`free_[1]` gone; 1536 bytes, 0 free) in `MCE_GLSL_PASS_U`, `Forward3DPassU`, the offset `static_assert` and its message, the size comment and message. `MCE_GLSL_HALO` (`halo_term`, added by `cirrus_layer` behind `p_halo.x > 0` after the streak composite, so `kSkyFS`'s `apply_cirrus` draws it and `water_sky` reflects it; the low layer and the fog cover it; `sky_radiance()` / IBL unchanged; no `sin()`). CPU twin `mce/render/halo.h` / `halo.cpp` (narrow, `render::Halo` forward-declared; `halo_shader_params` feeds the upload). Review follow-up: the sun term is gated by the disk (`p_sun.y > 0`, the kit's hand-off signal) instead of `p_night` (zero in four night setups, which drew a full sun halo round the moon) and by `smoothstep(0.10, 0.20, s.y)` so it is 0 over the kit's whole hand-off band (no pop at the switch); tests for the four setups and a dusk sweep through the hand-off (CPU and GL); the dog test moved to 14 / 25 degrees. Deviations from the plan, each in Design: the veil multiplies by the streak opacity (no halo in a gap) with a shorter horizon fade, the dogs fade out above 60 degrees instead of a clamp, the gains (ring 0.4, dogs 2.0, moon 0.006) and the pixel fade (0.01..0.03 rad) were tuned on 800 x 450 llvmpipe frames, and the goldens are `halo_sky` (sun 12 degrees up, thick deck) and `halo_night` (the moon's ring) instead of `halo_sun` / `halo_lowsun` / `halo_moon`, on linux-gl and linux-sdlgpu. Every existing golden byte-identical on both backends. Tests: `tests/render/test_halo.cpp` (gate, ring profile and edge floor, dispersion, dog centres 22 degrees on the horizon circle, gain, veil, caps, sun gate, moon hand-off, pixel fade) and `test_render3d_halo_gl` on GL and SDL GPU (strength 0 / disabled / NaN / Low / `r.halo 0` / cirrus coverage 0 / sun under the horizon equal the plain frame; only sky above the horizon changes; ring peak at 22.25 deg, inner slope 1.9x the outer; blue-minus-red centroid 0.45 deg with dispersion 1, -0.02 with 0; dogs on the sun's elevation 22 deg away, 45.4 deg apart at 14 deg and 48.8 at 25 deg, dimmer when higher; no halo in streak gaps; hidden behind an opaque low deck; the moon ring 0.012 against the sun's 0.56 linear, absent by day; the sea reflects it; origin shift unchanged; a quarter-pixel yaw changes it by 4.5 % (160 x 120) and 2.4 % (640 x 360) of its peak at the 99th percentile; GLSL vs twin over 6652 probes, worst 0.004). The cirrus twin test's PassU gains the halo's uniforms (zero). `kSkyFS` 46536 bytes, `kWaterFS` 56063 (under 65536). SPIR-V entry compiles here; the Metal-cross entry is macOS-only and unverified on Linux. `perf_run` not recorded (no GPU here) |
| 54c. Kit, extract, Luau | planned | | |
| 54d. Sample, guide, close | planned | | |
