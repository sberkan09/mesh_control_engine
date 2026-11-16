# M-METAL-HOSTED — Metal checks on a GitHub-hosted macOS runner (milestone record)

**Status:** workflow in (`.github/workflows/metal.yml`, on demand and on edits to itself; not a PR gate). First results 2026-11-09
(PR #1194, runs 1 to 6). Owner: the thread that wrote it; fixes go forward from the next Metal finding.

**Why:** the Metal rows of the GPU list (`/mnt/project-files/assessments/gpu-gereken-isler.md`, A24 and the "test_shader_cross
on the Mac" notes of A11 to A19, C1) waited for a developer Mac. A standard hosted `macos-26` runner (arm64, free for this public
repository) has a virtual Apple GPU ("Apple Paravirtual device") that is enough for part of that list.

## What the workflow runs

Builds `macos-release` with `MCE_GPU_TESTS=ON`, then (each with its own time limit, results in `out/summary.txt`, the logs, the
`tour_reflect` frame as PNG and the written goldens uploaded as the `metal-results` artifact):

1. `test_shader_cross`, `test_rhi_metal`
2. `test_render_gl -tc=*ssr*,*SSR*` (the 14 SSR cases of M-REFLECT2 on the Metal backend)
3. `test_golden_gl`, `sample_kit3d_tour_reflect_headless`, `sample_kit3d_tour_reflect_gpu`
4. the same Metal tests again with `MTL_DEBUG_LAYER=1`, then with `MTL_SHADER_VALIDATION=1`, one at a time
   plus `golden` with the debug layer (it found the `u_volSmplr` bug below, fixed in #1199)

## Results (macos-26, Apple Paravirtual GPU, runs 5 and 6)

| Check | Result |
|---|---|
| `test_shader_cross` (GLSL to MSL for the engine's shaders, no GPU) | **12/12, 107 assertions.** Includes the Mac-only cases the GPU list names: lens flare (A11), edge outline (A12), light cookie (A13), translucency/wrap (A14), lens rain (A15), heat haze (A16), underwater (A17), sun shafts (A18). It has no SSR case; SSR translation is shown by the 14 SSR cases below, which compile and run `kSsrFS` (`p_ssr2`, gather, `u_vel`) on Metal. |
| `test_rhi_metal` | 18/19. The one failure: `caps().timestamp_queries` is false on the virtual GPU (GPU timestamps unsupported there). |
| M-REFLECT2 SSR on Metal, 14 cases | **14/14, 918 assertions** (106 s; 65 s with the debug layer). Includes the roughness ladder, the normal-map widening and the velocity path (reflection centroid of a moving cube: with `r.ssr.motion` 89.4 px, camera path only 85.6 px, truth 92.6 px; the still-scene TAA check differs by 0). |
| `sample_kit3d_tour_reflect_headless` / `_gpu` | pass (frame 120 written; 8 s). Frame in the artifact. |
| `test_golden_gl` | 65/65, **but not a comparison**: there is no `macos-metal` golden directory, so the suite writes every baseline on the first run and passes. The written images are from a virtual GPU and were **not** committed. |
| `MTL_DEBUG_LAYER=1` alone | same results as without (SSR 14/14, `test_rhi_metal` 18/19). |
| `MTL_SHADER_VALIDATION=1` | **breaks this runner**: the compute pipelines (`mce.3d.cluster_build`, `hiz_reduce`, `particle_sim`, `exposure_hist`, `exposure_reduce`) fail to compile ("Compilation failed"), `test_rhi_metal` drops to 15/19 (compute readbacks return 0), the SSR cases hit `kIOGPUCommandBufferCallbackErrorTimeout`. Do not use it on hosted runners. |

## Limits of the hosted runner (what this does not prove)

- The GPU is virtual: no pixel conclusion about a real Apple GPU, no timings (no timestamp queries; a paravirtual device is not
  representative), no windowed look by a person, no thermal or memory behaviour. Goldens for `macos-metal` need the real Mac.
- The SSR, shader-translation and tour results prove: the shaders compile and run on the Metal backend, the SSR measurements
  hold (not only on lavapipe and NVIDIA), nothing aborts. They do not prove the look.
- `MTL_SHADER_VALIDATION` cannot be checked here (see above); it stays a Mac item.

## Open items

- ~~**Engine bug:** `MTL_DEBUG_LAYER=1` aborted `test_golden_gl` in "a reflection probe mirrors the room in the metal floor"
  with `missing Sampler binding at index 4 for u_volSmplr[0]`.~~ **Fixed in PR #1199** (2026-11-09). Two capture passes ran on
  a scene's first frame before `Forward3D::bind_main_state` had bound every bank the lit shader declares: the reflection probe
  faces left bank 4 (`u_vol`, volumetric fog) unbound, and the irradiance-volume faces left banks 0 to 2 (decal textures) and
  the decal records unbound. GL reads an unbound unit as zero and the shaders never read them there (`PassU::vol.x`,
  `decals.x` are 0), so only Metal's validation saw it. Both passes now bind the stand-ins (`ao_off_`, `bind_decals(.., false)`).
  Verified on the hosted runner with a temporary workflow (runs 1 to 3 on #1199): `test_golden_gl` 65/65 with `MTL_DEBUG_LAYER=1`
  (assert mode), and with `MTL_DEBUG_LAYER_ERROR_MODE=nslog` the whole `test_golden_gl` and `test_render_gl` (317 cases) log
  **no** Metal validation message.
- **Metal-only test failure (not a binding bug, open):** `test_render_gl` on the hosted runner is 316/317, with or without the
  debug layer: `edge outline: with TAA the lines stay on the edges, flicker bounded` fails `CHECK(plain_jumps == 0)` with 12
  (`test_render3d_outline_gl.cpp:414`; the plain TAA frame, without outlines, has 12 pixels that step by over 32 between frames
  4 and 7, max step 41). The case passes on GL. Either the paravirtual GPU's TAA history differs or Metal's TAA path is not
  stable there; it needs a look on the real Mac before it is called a bug. The whole `test_render_gl` has not run in
  `metal.yml` (only the SSR subset); adding it is worth doing once this case is understood.
- A real-Mac pass remains for: `macos-metal` goldens, pixel look of SSR, GPU timestamps and 1080p cost, `MTL_SHADER_VALIDATION`.
- Linear: no card (Linear did not connect from this thread); this file is the record.
