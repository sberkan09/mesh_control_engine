# M-CLOUDSHADOW — Cloud shadows on the ground (milestone record)

**Status:** done (2026-11-10; 51a data seam, 51b pure helpers and quality tiers, 51c the post pass, 51d the kit component, 51e Luau and sample, 51f docs). **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-CLOUDS ("cloud shadows on the ground" was not in scope), M-SUNSHAFTS and the open-world look. No Linear issues (free issue limit); this record is the tracker.

**Naming:** no type, file or cvar called `CloudShadow`, `cloud_shadow` or `M-CLOUDSHADOW` exists (`src/`, `docs/`, `tests/`, `samples/` checked 2026-11-10; the only hits are the M-CLOUDS "not in scope" line).

**Goal.** Drifting patches of shade on the terrain, buildings and water that match the clouds in the sky: where the sky layer has a thick cloud between a point and the sun, the point loses part of the sun. The patches move with the same wind and clock as the clouds, the shade deepens with the weather's coverage, and a scene without the component renders exactly as today.

**Not in scope:** shadows of clouds on other clouds, shadows in the IBL bake, GI captures and reflection probes (they stay cloudless, as with the sky layer), shadows from a moon, a physically based sun term (the pass scales the frame, it does not know which light a pixel received), cloud shadows through the volumetric fog or under water.

## Design

- **Why a post pass.** The lit shaders `kMeshFS` (62954 bytes) and `kTerrainFS` (62976) sit under the 65536-byte string literal limit, and all 16 sampler banks of the lit pass are taken (M-LIGHTCOOKIE). A per-fragment cloud lookup there would need the sky's noise copied into both and a texture or a large splice. A post pass on the HDR frame has neither limit and needs no change to the lit shaders.
- **The pass.** Runs on the HDR frame after the exposure histogram and before the sun shafts, the flare and bloom. For each non-sky pixel the world position is rebuilt from the prepass depth; the point is moved along the sun direction to the cloud plane (`height_m` above the camera, same plane as the sky layer, same camera-relative xz); the sky's periodic integer-hash noise (`p_cloud`, `p_cloud2` drift) is read there and the same coverage threshold gives a cloud density `c` in 0..1. The pixel is multiplied by `1 - strength * c * sun_share`, where `sun_share` (0..1) is the sun's share of the frame's light, taken from the sun and ambient intensities in `Scene3D` (so a dim overcast or dusk frame darkens less than a bright noon). Sky pixels are untouched. The edge is soft (a smoothstep over the cloud's own edge) and the effect fades with the sun's elevation (`light.y`) toward the horizon, where the projected point leaves the layer.
- **Render carrier (seam).** `render::CloudShadow {strength, softness, enabled, pad_}` in `PostFx` (`scene3d.h`, wide header: its own seam PR), `QualitySettings::cloud_shadows`, cvar `r.cloud_shadows`; Low off, Medium a half-size noise (2 octaves), High 3 octaves. `apply_quality` clamps (NaN to 0).
- **Pure helpers** in `mce/render/cloud_shadow.h`: `cloud_shadow_point(world, sun_dir, cam, height)`, `cloud_density(p, coverage, ...)` (the CPU twin of the sky's noise, shared with the sky test), `cloud_shadow_factor(...)`. The pass shader is tested against them.
- **Kit.** Opt-in `CloudShadow3D` (`mce/kit/cloud_shadow.h`: `strength` 0.6, `softness` 0.3; no state, the clouds come from `CloudLayer` and the weather, `kit3d.h` untouched). A scene without a `CloudLayer` gets no shadow. `Kit3DExtract` feeds `PostFx::cloud_shadow` and nothing is hashed.
- **Luau.** `weather.set_cloud_shadow{strength=, softness=}` and getters.
- **Sample.** The `tour_cloudshadow` scene in `kit3d_tour` (tuned in CLOUDSHADOW-4: a 600 m `field_wide` ground in light sand, 60 m cells, a strong sun over a dim ambient, so the patches read at a glance) and `samples/12_weather`.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 51a. Seam | `render::CloudShadow` in `PostFx`, `QualitySettings::cloud_shadows`, `r.cloud_shadows` | render, tests/render | Sonnet; seam PR for `scene3d.h` |
| 51b. Helpers | `cloud_shadow.h` (point, density, factor), `apply_quality` overload, tests | render, tests/render | Sonnet |
| 51c. Pass | the post pass, quality tiers, stats, `cloud_shadow` golden and pixel tests | render, tests/render_gl | **Opus writes**, independent Opus review |
| 51d. Kit | `CloudShadow3D`, extract feed, padding audit | kit, tests/kit | Sonnet |
| 51e. Luau, sample | `weather.set_cloud_shadow`, `tour_cloudshadow`, `samples/12_weather` | script, samples | Sonnet |
| 51f. Docs, close | user guide, `capabilities.md`, record | docs | Sonnet |

## Gates
- A scene without `CloudShadow3D` (or with `r.cloud_shadows 0`, or on Low) is bit-identical to today; every existing golden stays byte-identical.
- New golden `cloud_shadow` with pixel tests: only non-sky pixels change; more coverage shades more of the ground; the patches move with time and a 180 degree wind turn moves them the other way; a rebase of the floating origin is bit-identical; the sun near the horizon fades the effect to nothing; the sky stays as it is.
- The shader agrees with the CPU helpers on a grid of sample points.
- GL validation clean; GPU cost of the pass recorded per tier.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 51a. Seam | done | TBD | `render::CloudShadow` and `PostFx::cloud_shadow`, `Forward3DStats::cloud_shadow` / `cloud_shadow_strength`, `cloud_shadow_pipe_`, `record_cloud_shadow` (declared only); defaults test |
| 51b. Helpers | done | TBD | `cloud_shadow.h` (point, drift, noise, density, factor), `QualitySettings::cloud_shadows` / `cloud_shadow_octaves`, `r.cloud_shadows`, `apply_quality` clamps; `test_cloud_shadow.cpp` |
| 51c. Pass | done | TBD | `kCloudShadowFS` (its own shader; the sky's noise shared through `MCE_GLSL_CLOUD_NOISE`, split out of `MCE_GLSL_SKY_LAYERS` with the sky and water text byte for byte unchanged; `kMeshFS` / `kTerrainFS` untouched), its own 7-vec4 `CloudShadowU` at bank kPass (no `PassU` change), the pipeline made at startup where the prepass exists, `record_cloud_shadow` after the exposure histogram and before the shafts. Cloud values from `cloud_params` (the sky's `p_cloud` / `p_cloud2`), octaves from the tier, `cloud_shadow_sun_share` (new: S / (S + A), luma of sun colour * intensity against the sky ambient), strength * (1 - underwater submersion). Off paths record nothing (setting, Low, `r.cloud_shadows 0`, coverage 0 / layer off, sun under 0.02 / NaN / zero, ortho, render-to-texture). Tests: 11 GL / SDL GPU cases (plus the opt-in cost case; 14 after the review fixes) in `test_render3d_cloud_shadow_gl.cpp` (off paths bit for bit, only ground pixels darken (0 sky pixels changed on llvmpipe), coverage 0.25 / 0.5 / 0.8 shade 1062 / 5964 / 10858 px, 1 s of wind moves the shade +7 px and the turned wind -7 px, rebase bit for bit, the horizon fade, Medium vs High, the exp² fog scaling the shade down, TAA history clean, a window view after a render-texture view, shader vs CPU twin on 384 points (one case with fog): worst 2e-6), golden `cloud_shadow`, SPIR-V and MSL cases, sun share unit tests; every existing golden unchanged. GPU cost (opt-in case, `MCE_CLOUD_SHADOW_COST=1`, 1280 x 720, llvmpipe, a CPU rasterizer): 9.1 to 10.6 ms Medium, 10.6 to 10.9 ms High over two runs (run-to-run noise is as large as the octave difference); unmeasured on a real GPU. Review fixes (follow-up commit): the exp² fog composite is exact (`cloud_shadow_shade`: only the surface part s of src = s T + fog (1 - T) is shaded; the fog colour rides `CloudShadowU`, now 8 vec4; a direct test with fog 4.0 over s 0.3 at T 0.35..0.65 matches the exact formula within the half-float source's rounding), a prepass depth of 6e4 m or more (half-float overflow) counts as sky and a non-finite plane point stays unshaded, `Sky::enabled` false records nothing (no clouds drawn), a negative fog density fogs as the main pass's squared term does, the view gate is the tested `cloud_shadow_perspective` (Scene3D builds perspective views only, so the orthographic off path is a unit test), and a scene test checks the uniform fill: for two other camera poses every pixel whose 3 x 3 ground points the CPU puts in clear sky is byte for byte the frame without the pass and every one it puts under a thick cloud is darker. Not modelled: the volumetric fog (with it on, no fog term is applied), transparent surfaces and water (they take the prepass point behind them, or none), a silhouette pixel takes its half-resolution block's sample. Accepted limits: `sun_share` reads `DirectionalLight::ambient_sky` only, not the IBL or the probes' ambient, so under a bright IBL sky the shade is deeper than the light split warrants; the half-resolution prepass depth is point sampled (no depth-aware upsample) and the pass runs after depth of field and motion blur, so on blurred pixels the shade follows the sharp prepass, not the blurred image. |
| 51d. Kit | done | TBD | `CloudShadow3D` (`mce/kit/cloud_shadow.h`: `enabled`, `strength` 0.6, `softness` 0.3, Range 0..1, explicit pad, no state), registered next to `SunShafts3D` in `install_kit3d` and in the three `engine_components.cpp` files, padding audit entry, `Kit3DExtract` feed (first enabled by entity index through `apply_quality`, so the clamps, Low and `r.cloud_shadows 0` apply; a scene without the component keeps the disabled default every frame); `kit3d.h` untouched; `test_cloud_shadow_extract.cpp` (9 cases: defaults, no component, enabled 0, values, clamps and NaN, first by index, tier and cvar, the sky layer and `Clouds` identical with and without the component and no `CloudLayer` = coverage 0, state hash unmoved) |
| 51e. Luau, sample | done | TBD | `weather.set_cloud_shadow{strength=, softness=}`, `weather.cloud_shadow_strength()` / `cloud_shadow_softness()` in `api_weather.cpp` (named errors, clamped to 0..1, nothing written on an error, `enabled` not settable) and typed in `definitions.cpp`; 2 cases in `test_script_weather.cpp`; `tour_cloudshadow` scene in `kit3d_tour` (`gen_cloudshadow.py`, 14 entities, headless test and GPU capture). `samples/12_weather` skipped: its `Environment3D` has `sky_enabled 0`, so the component would do nothing there (the pass needs the sky), and it would add a column to the sample's state hash for no visible result |
| 51f. Docs, close | done | TBD | user guide: the cloud shadows item in 8.6, the `PostFx` row, the scripting table (27.3), the `kit3d_tour` row in 18; `docs/engine/capabilities.md` row for `CloudShadow3D` (see the PR); this record |

## GPU verification r11 (2026-11-10)

Row A31 of the GPU list. One **RTX 3070** (8 GB, driver **580.65.06**; `OpenGL 4.5.0 NVIDIA 580.65.06 on NVIDIA GeForce RTX 3070/PCIe/SSE2`, `SDL GPU (vulkan) on NVIDIA GeForce RTX 3070`) on RunPod, master 2d116c4b, with llvmpipe and lavapipe on the same pod as the reference. How it ran, the pod and the cost: `M-GPU-VERIFY.md`, "GPU verification r11".

**Cases.** The 15 `cloud shadow*` cases of `test_render_gl` (1084 assertions) pass on NVIDIA GL, NVIDIA Vulkan, llvmpipe and lavapipe. The measured values agree across the four:

| Case | NVIDIA GL | NVIDIA Vulkan | llvmpipe | lavapipe |
|---|---|---|---|---|
| Sky pixels changed (of 7360) | 0 | 0 | 0 | 0 |
| Shaded pixels at coverage 0.25 / 0.5 / 0.8 | 1063 / 5962 / 10859 | 1061 / 5965 / 10859 | 1062 / 5964 / 10858 | 1049 / 5971 / 10841 |
| 1 s of wind +x / -x | +7 / -7 px | +7 / -7 px | +7 / -7 px | +7 / -7 px |
| Luma taken, sun sin 0.019 / 0.08 / 0.25 | 0 / 25931 / 265692 | 0 / 25925 / 265740 | 0 / 25702 / 266749 | 0 / 26163 / 266849 |
| Shader against the CPU twin, 384 points, worst | 2.9e-5 | 2.9e-5 | 1.9e-6 | 2.0e-6 |
| Uniform fill, two poses: clear pixels changed, thick pixels not darker | 0, 0 | 0, 0 | 0, 0 | 0, 0 |

**Goldens.** `cloud_shadow` passes 3 of 3 runs on NVIDIA GL and fails 3 of 3 on NVIDIA SDL GPU (3 of 300 blocks over 0.5%, worst 0.955%, the same image every run); every other golden passes on both backends (GL 67 of 67, SDL GPU 66 of 67). The cause is the committed `linux-sdlgpu/cloud_shadow.png`, taken on lavapipe: lavapipe draws two far-ground blocks just under the horizon (rows y 48 to 55, columns x 8 to 15 and 24 to 31) about 1% darker than llvmpipe, NVIDIA GL and NVIDIA Vulkan, which agree with each other (NVIDIA Vulkan against the llvmpipe `linux-gl` golden: worst block 0.418%, none over). It is not a fault in the pass. The fix, left to a thread: a `linux-sdlgpu-nvidia/cloud_shadow.png` vendor golden (as for `ibl_roughness`), for example a copy of the `linux-gl` image.

**Cost.** The pass in the GL `bloom` stage of `tour_cloudshadow` (pass on against `-r.cloud_shadows 0`, 537 frames, RTX 3070):

| Tier | 1920 x 1080 | 1280 x 720 |
|---|---|---|
| Low | off (0) | off (0) |
| Medium, 2 octaves, half-size noise | **0.054 ms** | **0.037 ms** |
| High, 3 octaves | **0.076 ms** | **0.041 ms** |

The opt-in case (`MCE_CLOUD_SHADOW_COST=1`, 1280 x 720) reads 0.0396 ms (Medium) and 0.0448 ms (High) over three runs; SDL GPU at High 1080p reads +0.068 ms (fence timestamps, noisier). The whole 3D frame of the scene is 0.67 ms (High 1080p, pass off), so the pass is about 11% of it at High; next to r7's post passes it sits between lens flare (+0.060 ms) and sun shafts (+0.205 ms) at High 1080p on the faster 3080 Ti.

**The capture** (`sample_kit3d_tour_cloudshadow_gpu`'s frame: `tour_cloudshadow`, tick 300, 1280 x 720, High). The NVIDIA GL and NVIDIA Vulkan frames match llvmpipe's and lavapipe's to 0.00% of pixels over 2/255 (max 5/255), with the pass on and off; the PNGs in the project files (`gpu-runs/2026-11-10-rtx3070-r11/tour_cloudshadow_t300_cloud_shadows_{1,0}.png`) are the llvmpipe frames, since images leave the pod only as log text. What they show: a high sun over the flat pale-sand terrain running to the horizon, a pale blue sky with fine cloud texture across its top, and in the middle the row of five cream boxes (drawn as flattened ellipsoids) and six green trees. With `-r.cloud_shadows 1` soft-edged grey patches lie on the ground: a large one across the lower right that takes in two of the buildings and three of the trees (they turn tan and dark green), irregular patches through the middle distance and long thin streaks toward the horizon where the patches are foreshortened; the near left stays in sun. The edges are soft over tens of pixels, with no banding or blockiness from the half-resolution depth. The objects darken with the ground under them. The sky is unchanged (at most 1/255 in its top 15% of rows). With `-r.cloud_shadows 0` the ground is evenly lit and only the objects' own contact shadows remain. In numbers, the pass changes 38.7% of the frame by more than 12/255: 44.9% of pixels are darker by more than 2 luminance levels, none brighter, by 42 levels on average (median 0.76 of the unshaded value, deepest 0.20). The frame's mean drops from RGB 195/198/195 to 177/179/174. Whether the patch size and darkness read well in motion is a windowed look for a person (the drift at the scene's wind, TAA under the moving edges).

**Open.** The windowed look in motion (desktop). Metal and WebGPU translation of `kCloudShadowFS` (Mac). The NVIDIA SDL GPU vendor golden (above).
