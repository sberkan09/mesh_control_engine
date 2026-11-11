# M-CIRRUS — High cirrus streaks above the cloud layer (milestone record)

**Status:** in progress (started 2026-11-11), steps 53a to 53d. 53a (this plan and the `render::Cirrus` seam) and 53b (the
sky shader) are done; 53c and 53d are not started. **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-CLOUDS (the one cloud
layer in the sky pass, `apply_clouds`), M-WATER (the water reflects the sky layers through `water_sky`) and the
M-AURORA / M-RAINBOW / M-METEOR line, whose split (plan and seam, shader, kit, sample and close) it copies. No Linear
issues (free issue limit); this record is the tracker. The shader step is Opus-written and merged after an independent
Opus review plus a green `--task` (as M-CLOUDS 9b, M-AURORA 16c, M-RAINBOW 17c and M-METEOR 18c).

**Naming:** no type, file, cvar or doc line called `Cirrus`, `cirrus` or `M-CIRRUS` existed before 53a (`src/`,
`docs/`, `tests/`, `samples/`, `scripts/` checked 2026-11-11). `render::Cirrus` (the carrier, 53a) and `kit::Cirrus`
(the component, 53c) live in different namespaces, as `render::Caustics` and `kit::Caustics3D` do; the kit extract
names the carrier `render::Cirrus` explicitly.

**Goal.** Today the sky has one cloud layer: integer-hash value noise on a plane `height_m` (default 2000 m) above the
camera, drifting with the wind (`p_cloud`, `p_cloud2`). Real skies often carry a second, much higher deck of thin ice
cloud: cirrus, long fibrous streaks combed out along the upper wind, silver near the sun, and the last cloud to stay
pink and gold after sunset while the low deck below has already gone grey. M-CIRRUS adds that layer: a plane about
9000 m up, noise stretched along the wind (6:1 by default) so it reads as streaks, low opacity (0 to about 0.5, soft
edges), lit by the frame's sun and sky light, its own coverage, a faster drift than the low layer, composited behind the
low layer. Opt-in and deterministic; a scene without it renders exactly as today.

**Not in scope:** volumetric clouds (both layers stay noise on a plane), cloud shadows from cirrus (M-CLOUDSHADOW reads
the low layer only, and cirrus is too thin to shade; `kCloudShadowFS` is not touched), cirrus in the IBL bake,
reflection probes or GI captures (`sky_radiance()` carries neither cloud layer today, see Design), cirrus under an
equirect `sky_texture` (the texture wins, as for the low layer), halos and sun dogs (an ice-crystal optics follow-up),
contrails, more than one cirrus deck, changing `Weather`, `CloudLayer`, `WindDirection`, `WorldClock` or
`Environment3D` bytes, and Metal / WebGPU pixels in this milestone (the shader text goes through the existing
translation tests only).

## Design

- **Carrier (seam, 53a).** `render::Cirrus {coverage, density, scale_m, stretch, speed, wind_dir, height_m, enabled,
  pad_[3]}` in `scene3d.h` right after `render::Clouds` (where `Clouds`, `NightSky`, `AuroraSky`, `RainbowSky` and
  `MeteorSky` live), `Scene3D::set_cirrus()` / `cirrus()`, `QualitySettings::cirrus` (Low off, Medium and High on),
  the saved cvar `r.cirrus` and a by-value `apply_quality(q, cirrus)` overload, exactly the shape of M-AURORA 16b.
  Defaults: `coverage` 0 (off), `density` 0.35, `scale_m` 3000, `stretch` 6, `speed` 2, `wind_dir` +x, `height_m`
  9000, `enabled` true. Clamps (NaN to the default): coverage 0..1 (NaN 0), density 0..1, scale_m 200..40000, stretch
  1..16, speed 0..10, height_m 1000..30000; `wind_dir` normalized (NaN / zero / infinite to +x); `enabled` passes
  through; coverage is zeroed by the Low tier and `r.cirrus 0`. In-range values pass bit for bit, so the default
  carrier is bit-identical before and after `apply_quality` (tested). 36 bytes, no implicit padding (`static_assert`
  in the test). The carrier has its own `wind_dir` rather than reading `Clouds::wind_dir`, so the cirrus streaks keep
  their direction when the low layer is off (coverage 0); the extract fills both from the same `WindDirection`.
  `scene3d.h` is a wide header (`include_fanout.py`: at most 235 of 1067 TUs on 2026-11-11, over the 150 warning;
  `quality.h` 151), which is why the carrier ships alone, before any user, as this seam.
- **Sky shader (Opus, 53b).** A new `apply_cirrus(vec3 c, vec3 d, vec3 sun, float cs)` in the shared
  `MCE_GLSL_SKY_LAYERS` string next to `apply_clouds`, behind `p_cirrus.x > 0.0` (coverage; 0 = the frame without it
  bit for bit, the same off-check as the low layer):
  - Up-going rays only (`d.y > 0.02`); the ray meets a plane `p_cirrus.w` meters above the camera, camera-relative
    (the camera's xz and the floating origin never enter).
  - Anisotropic coordinates: with `w` the wind's xz unit vector and `w' = (-w.y, w.x)`, `p = d.xz * h / d.y`,
    `q = (dot(p, w) / (cell * stretch) - drift, dot(p, w') / cell)`, wrapped to the 64-cell period. The drift is one
    scalar along the wind in noise cells (`time * speed * wind_speed / (cell * stretch)`), wrapped on the CPU in f64
    to the period, as `p_cloud2` is; a cell is `stretch` times longer along the wind than across it.
  - Two octaves of the existing `cloud_value` (integer pcg2d lattice hashes, `cloud_hash`; exact wrap: each octave is
    a whole lattice per period, the `% m` wrap of the low layer; no `sin()`), with their own seeds so the streaks do
    not repeat the low layer's pattern; a second, cross-wind octave at 2x keeps the fibres from looking like smeared
    blobs.
  - Thin and soft: `t = 0.8 - 0.6 coverage`, `mask = smoothstep(t, t + 0.2, n)` (a wider edge than the low layer's
    0.12), opacity `a = mask * density * 0.5 * smoothstep(0.02, 0.2, d.y)`, so it never exceeds 0.5 and fades into the
    horizon haze.
  - Lighting from the same frame uniforms as `apply_clouds` (`p_light_color`, `p_amb_sky`, the sun direction): a
    forward-scatter silver term near the sun (`pow(max(cs, 0), 8)`, wider than the low layer's lining because ice
    scatters forward strongly), little self-shadowing (thin), and a later sunset: the sun term uses
    `smoothstep(-0.16, 0.04, sun.y)` instead of the low layer's `smoothstep(-0.1, 0.1, sun.y)` (at 9 km the sun
    stays above the local horizon about 3 degrees longer, so the streaks still catch light after the low deck has
    gone dark), with a warm tint toward the horizon as the sun sets. 53b first checks what `p_light_color` holds just
    below the horizon (the day cycle may already have faded it or swapped in the moon); if it is already dark there,
    the afterglow term takes the warm colour from `p_sky_horizon` instead, and the step records which.
  - Order: `kSkyFS` applies it after the night sky, the aurora, the rainbow and the meteors and right before
    `apply_clouds`, so the low layer composites over it (the cirrus is behind it) and the fog over both. A rainbow
    seen through cirrus is therefore dimmed by up to the cirrus opacity, although the bow is physically nearer; 53b
    may move the call before `apply_rainbow` if the review asks (records the choice). A cirrus deck in front of the
    sun dims the disk by its opacity, as the low layer does.
  - Water reflection: not free. `water_sky` (`kWaterFS`) calls each layer explicitly, so 53b adds one
    `if (p_cirrus.x > 0.0) c = apply_cirrus(c, r, sun, dot(r, sun));` before its `apply_clouds` line; the function
    itself is shared through `MCE_GLSL_SKY_LAYERS`, so there is no second copy. With coverage 0 the water is
    unchanged by construction.
  - IBL: not free either, and not in scope. `sky_radiance()` (the IBL prefilter, the irradiance and SH bakes, the
    ambient specular fallback) is the gradient or the sky texture only; the low cloud layer is not in it today, so
    the cirrus is not either. Recorded here so nobody expects the ambient light to change with cirrus.
  - Low tier: off (coverage 0 from `apply_quality`), no octave count field: two octaves are cheap next to the low
    layer's three. If 53b's measurement says Medium needs one octave, it adds a tier field in a separate seam (it
    would touch `quality.h` again).
- **PassU budget (53b checks it first).** `Forward3DPassU` is 1536 bytes (96 `vec4`) with 3 free (`free_[3]`; M-WATER2
  49c took the last before). The cirrus needs two: `p_cirrus` (x coverage, y density, z 1 / cell (1/m), w plane
  height (m)) and `p_cirrus2` (xy the wind's xz unit vector, z stretch, w the drift along the wind in cells mod 64),
  leaving 1 free; the size does not change and the ring cost per upload stays the same. 53b updates the places that
  must agree in one PR: `MCE_GLSL_PASS_U`, `Forward3DPassU`, the offset chain `static_assert` and its message, the
  size comment and the "free" count in the size `static_assert`'s message. If a third `vec4` turns out to be needed,
  53b stops and splits a PassU growth seam (one more 256-byte slot, as M-AURORA 16c did) instead of squeezing.
- **Kit (53c).** Opt-in component `Cirrus` (`mce/kit/cirrus.h`, one per scene, the first in ECS order is read,
  registered by `install_weather` next to `CloudLayer`), all `Save` with a `Range`: `strength` 1 (0..1),
  `fair_coverage` 0.45 (0..1, cover in clear weather), `front_coverage` 0.7 (0..1, cover in the hours before a change
  of weather), `density` 0.35, `scale_m` 3000, `stretch` 6, `speed_scale` 2 (x `Weather.wind`), `height_m` 9000.
  A pure helper `cirrus_coverage(const Cirrus&, const Weather&, f32 day_fraction) -> f32`: blends `fair_coverage`
  toward `front_coverage` by the weather's change progress (cirrus runs ahead of a front), lowers it under heavy
  overcast (the low deck hides it anyway; no wasted pixels), and adds a slow deterministic variation over the
  `WorldClock` day from an integer hash of the day and a smoothstep blend, no `sin()`; NaN and inverted ranges fall
  back to the defaults; strength 0 returns exactly 0. `Kit3DExtract::apply_environment` fills `render::Cirrus` next
  to the cloud feed (`.cpp` only): coverage from the helper, `wind_dir` from the first `WindDirection` (+x without),
  `speed` from `Weather.wind * speed_scale`, the rest copied, then `apply_quality`. Nothing is set with no `Cirrus`,
  no `Weather` or an equirect `sky_texture`. Luau: `weather.set_cirrus{strength=, fair_coverage=, front_coverage=,
  density=, scale=, stretch=, speed_scale=, height=}`, the authoring getters and a read-only
  `weather.cirrus_coverage()` from the same helper; named errors, clamped to the Range, nothing written on error, a
  missing `Cirrus` raises; typed in `definitions.cpp`, same rules as `weather.set_clouds`.
- **Determinism.** A scene without `Cirrus` is byte-identical (one empty query in the extract; the extract is not part
  of the state hash). The coverage is a pure function of authoring, the weather and the clock; the drift is the scene
  time times rates, wrapped on the CPU in f64 to a period every octave divides, so the GPU pattern is seamless and
  precision-stable over long runs.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 53a. Plan and seam | This record, the ROADMAP row, `render::Cirrus` in `scene3d.h` after `Clouds`, `Scene3D::set_cirrus()` / `cirrus()`, `QualitySettings::cirrus` (Low off), `r.cirrus` (Saved), the by-value `apply_quality` overload with the clamps above; `tests/render/test_cirrus_quality.cpp` (defaults, a fresh `Scene3D` holds the default bit for bit, the defaults pass `apply_quality` bit for bit on every tier, no implicit padding (`static_assert`), set/get round trip, Low and `r.cirrus 0` zero the coverage and leave the low layer alone, `enabled` passes through, clamps and NaN, `wind_dir` normalization); the changelog fragment. No shader change | render, tests/render, docs | Sonnet; the seam PR for `scene3d.h` and `quality.h` |
| 53b. Sky shader | Check the PassU budget first (two of the three free `vec4`, the size unchanged); `p_cirrus` / `p_cirrus2` in `Forward3DPassU` / `MCE_GLSL_PASS_U`, filled next to the cloud params in `forward3d.cpp` (the drift wrapped in f64); `apply_cirrus` in `MCE_GLSL_SKY_LAYERS`, applied in `kSkyFS` before `apply_clouds` behind the off-check and called from `water_sky`; new goldens `cirrus_sky` (day, default streaks) and `cirrus_dusk` (the sun just under the horizon: the streaks lit, the low layer dark), on GL and SDL GPU, all existing goldens (including `cloud_sky`, `night_sky`, the aurora, rainbow, meteor and `water_*` sets) byte-identical on both backends; `test_render3d_cirrus_gl` pixel tests (coverage 0 and `enabled` false equal the plain frame; only sky pixels above the horizon change; the opacity stays at or under 0.5; streaks are longer along the wind than across it (an autocorrelation along and across); the low layer in front hides them where it is thick; brighter near the sun; lit at a sun elevation where the low layer is already dark; the water reflects them and the water without cirrus is unchanged; Low tier equals the plain frame; an origin shift leaves the sky unchanged; bit-identical across the drift wrap); `perf_run` at 1080p recorded | render, tests/render_gl | **Opus writes**, independent Opus review |
| 53c. Kit, extract, Luau | `Cirrus` component and `cirrus_coverage` in `mce/kit/cirrus.h` (registration in `install_weather`, zeroed pad, the `test_padding` entry, the `engine_components.cpp` includes), the `Kit3DExtract` feed (`.cpp` only) with the quality tiers through `apply_quality`, `weather.set_cirrus`, the getters and `weather.cirrus_coverage()` typed in `definitions.cpp`; tests in `tests/kit/test_cirrus.cpp` (helper range, fair vs front vs overcast, strength 0 gives exactly 0, the day variation is a pure function of the clock, NaN fallbacks, save/load round trip, no `Cirrus` gives a bit-identical `Scene3D`, the extract's carrier equals the helper's coverage and the `WindDirection`, Low tier zeroes it, `Weather` / `CloudLayer` / `WindDirection` bytes unchanged) and `tests/script/test_script_cirrus.cpp` (getters, clamping, named errors, nothing written on error, a missing `Cirrus` raises). Split into a kit PR and a script PR if the diff is large, so no PR touches more than two modules | kit, script, tests | Sonnet |
| 53d. Sample, guide, close | `samples/12_weather` carries a `Cirrus` on its Weather entity (entity count unchanged) and its script logs `cirrus:` every 150 ticks; only that sample's goldens and log expectations move; the user guide's sky section (what the layer is, its defaults, the Low tier, why it has no shadow and no IBL), the 27.3 and 08-3d sky rows, the changelog, this record's step status and the ROADMAP row to done; `capabilities.md` and `sample_coverage.allow` are the integration runner's to regenerate | samples, docs | Sonnet |

53b is the only step that writes a shader, so it is Opus-written with an independent Opus review (the author does not
review it). The review checklist: the PassU fields agree in every place, the off-check, the order against the low
layer and the fog, the `water_sky` call, the exact wrap of the drift and the lattice, no `sin()`, and the opacity cap.
Steps stack in order (53a, then 53b and 53c in parallel, then 53d).

## Gates

- With coverage 0 (the default, no `Cirrus`, `enabled` false, the Low tier or `r.cirrus 0`) every frame is
  bit-identical to the frame without the layer; every existing golden under `tests/render_gl/*` stays byte-identical
  through 53a and 53b on both backends, and every sample hash until 53d moves `12_weather` only.
- `Weather`, `CloudLayer`, `WindDirection`, `WorldClock` and `Environment3D` are never written by cirrus code.
- PassU: the size stays 1536 bytes with 1 `vec4` free after 53b, or 53b ships a growth seam first; no other pass's
  block changes.
- Include fan-out: the only wide headers touched are `scene3d.h` and `quality.h`, in this seam (53a); `cirrus.h` is
  included only by the extract, the install, the script binding and the tests; `kit3d.h`, `weather.h` and
  `cloud_layer.h` are not edited.
- Slow tests: a cirrus GPU test over 60 s ships a smoke variant and the long one is `mce_mark_slow`.

## Limits (planned; 53b and 53d confirm or extend the list)

- A plane, not a volume: no parallax between fibres, no thickness, no self-shadowing beyond a constant thinness term.
- No shadow on the ground and no change to the ambient or IBL light.
- The streak direction follows the scene's one `WindDirection`; there is no separate upper-level wind.
- The look near the horizon is a fade into the haze, not a curved-earth perspective.
- Desktop GL and SDL GPU pixels only; Metal and WebGPU see the shader through the translation tests.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 53a. Plan and seam | done | TBD | `render::Cirrus` (36 bytes, explicit `pad_[3]`) after `render::Clouds` in `scene3d.h`, `Scene3D::set_cirrus()` / `cirrus()`, `QualitySettings::cirrus` (Low off), the saved cvar `r.cirrus`, `apply_quality(q, cirrus)` (clamps as in Design; in-range values bit for bit); `tests/render/test_cirrus_quality.cpp`. `include_fanout.py` 2026-11-11: `scene3d.h` at most 235 of 1067 TUs, `quality.h` 151 (both over the 150 warning: this is the seam PR). PassU budget checked: 3 `vec4` free, the cirrus needs 2, the size stays 1536. Water reflection needs one explicit `water_sky` line in 53b; the IBL does not see either cloud layer. No shader change |
| 53b. Sky shader | done | TBD | PassU: `p_cirrus` / `p_cirrus2` took two of the three free `vec4` (1 left, size 1536 unchanged). `apply_cirrus` in its own `MCE_GLSL_CIRRUS` piece of `MCE_GLSL_SKY_LAYERS`, applied in `kSkyFS` right before `apply_clouds` (after the meteors: a rainbow seen through cirrus is dimmed by it, the review may move it) and in `water_sky`; the IBL (`sky_radiance()`), probes and cloud shadows do not see it (checked). Noise as planned (pcg2d lattice, exact wrap, no `sin()`) with the octaves tuned on headless renders: broad `v(2 q.x, q.y)` (128 x 64: the streaks break up along the wind) and fibres `v(q.x, 6 q.y + 3 b)` (64 x 384, bent by the broad octave so they curve), `n = 0.55 b + 0.45 mix(0.5, s, fine)`, the fibres faded by their pixel footprint (`fine` = smoothstep(1.5, 4, pixels a fibre cell spans across the wind)), so `kSkyFS` calls a shared `cirrus_layer(c, d, sun, cs, pix)` through `apply_cirrus(c, d, sun, cs)` (the pixel's angle as `apply_aurora` takes it) and `water_sky` passes its own `pix`: no moire toward the horizon at 160 x 120. Afterglow: `p_light_color` is not dark just under the horizon (a DayCycle's 18:00 key fades over hours), so the warm colour comes from it, tinted toward (1, 0.55, 0.35) and lifted by 1 + warm, not from `p_sky_horizon`. The `render::Cirrus` defaults are unchanged. CPU twin `mce/render/cirrus.h` (the upload calls it); `test_render3d_cirrus_gl` (off paths bit for bit, sky only, cap 0.5, along vs across, hidden by a full low deck, brighter toward the sun, lit and warm at dusk, the sea, origin shift, drift wrap, GLSL vs twin: worst 3e-6), `tests/render/test_cirrus.cpp`; goldens `cirrus_sky`, `cirrus_dusk` (GL, SDL GPU). `kSkyFS` 43.4 KB (was 40.9), `kWaterFS` 53.0 KB, `kMeshFS` / `kTerrainFS` +204 bytes (the PassU lines). `perf_run` at 1080p not recorded (no GPU here) |
| 53c. Kit, extract, Luau | not started | | |
| 53d. Sample, guide, close | not started | | |
