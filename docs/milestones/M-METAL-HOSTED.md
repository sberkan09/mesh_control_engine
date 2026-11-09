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
   plus `golden` with the debug layer (it finds the `u_volSmplr` bug below)

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

- **Engine bug, confirmed on the hosted runner (needs a fix thread, Metal backend / render):** with `MTL_DEBUG_LAYER=1` alone
  (no shader validation) `test_golden_gl` aborts in `golden: a reflection probe mirrors the room in the metal floor` with
  `-[MTLDebugRenderCommandEncoder validateCommonDrawErrors:]: Fragment Function(main0): missing Sampler binding at index 4 for
  u_volSmplr[0]` (runs 4, 5 and 7, reproducible; the volumetric-fog sampler of the lit shader is not bound on that draw). The
  API-level debug layer does not depend on the GPU, so it should reproduce on the M4 as well. Without the debug layer the draw
  runs, so no test fails today. The SSR subset does not reach this draw. Fix: bind a (dummy) sampler/texture for `u_volSmplr`
  on the draws that do not have a volumetric texture, then run `golden` with `MTL_DEBUG_LAYER=1` (`golden_debuglayer` step).
- A real-Mac pass remains for: `macos-metal` goldens, pixel look of SSR, GPU timestamps and 1080p cost, `MTL_SHADER_VALIDATION`.
- Linear: no card (Linear did not connect from this thread); this file is the record.
