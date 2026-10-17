# M-GPU-VERIFY — Real-GPU verification rounds (milestone record)

**Status:** ongoing. The Linux CI renders with Mesa llvmpipe (GL) and lavapipe (Vulkan, `MCE_RHI=sdlgpu`) only, so
real-GPU checks run in rounds on rented RunPod GPUs (RTX 3070 for r1–r5; RTX 4090 and RTX PRO 4000/4500 Blackwell for
the parallel round). Each round's raw logs and its report
(`RAPOR.md`) are in the project files under `gpu-runs/<date>-<gpu>-r<N>/`. The coordinator starts the pods;
threads fix what a round finds and write down here what the next round must check.

**Linear:** these fixes have no Linear card (the workspace hit the free issue limit on 2026-10-16, see
`M-VISUAL3.md`); this file is the record.

## Rounds

| Round | dev_claude | Found | Fixed by |
|---|---|---|---|
| r1–r3 (2026-10-17) | before #467 | goldens `filmic_lens`, `parallax_cobble`, `ibl_roughness` over threshold; shadow map bound dead (NVIDIA UB warning); 50k sprites crashed; render-texture test too strict; no 2D `gpu_ms` | #467, #470, #474, #479 |
| r4 (2026-10-17) | 70113c9 | `parallax_cobble` still 1.81% (GL) / 1.80% (sdlgpu); sdlgpu BC vs RGBA difference larger than GL's; no 2D `gpu_ms` on sdlgpu | #487, #498 (below) |
| r5 (2026-10-17) | 70113c9 | BC vs RGBA re-measured on 04_diorama / 06_showcase (same numbers as r4) | analysis below, no code change |
| parallel (2026-10-17) | d221a48, 0ad5735 | r4 checks pass (`parallax_cobble` 0 blocks, BC equal on both backends, sdlgpu `gpu_ms` 539/540); `ibl_roughness` NVIDIA-specific; sdlgpu 2D `gpu_ms` p50 about 0.001 ms; `editor_play_smoke` 0 ticks on the 4090; `player_capture_no_display`; BC `auto` misses most real normal maps | #506, #512, BC-PR (below) |

## r4 findings

### 1. `parallax_cobble` over threshold (fixed, #487)

The parallax-shifted material maps and the height march went through `textureGrad` on the material sampler, which has
`max_aniso 4`. Anisotropic filtering is implementation-defined. On llvmpipe alone the golden moves 2.3% between 1x
and 4x and 1.8% between 4x and 16x, the same size as the 3070 gap; #470's gradient change did not touch it. The
shader now filters the shifted maps itself (`map_aniso` in `shaders3d.h`: up to 4 trilinear taps along the
footprint's major axis) and marches the height map at one isotropic level. llvmpipe (aniso) and lavapipe (none) agree
on the golden to 0.08% (2.3% before). Goldens re-baselined; threshold unchanged. Details: `M-VISUAL3.md`, step 2.

### 2. BC vs RGBA difference larger on sdlgpu (not a bug; the measurement was not deterministic)

r4/r5 compared a `-frames 120 -capture` of the BC pack against one of the RGBA pack. Those runs are not reproducible:
`06_showcase` orbits its camera on `time_`, which advances by however many fixed ticks the wall clock allowed, and TAA
and auto exposure take the frame's real `dt`. With `-r.vsync 0` on a fast GPU, frame 120 lands at a different time on
every run, more so on sdlgpu, whose frame pacing differs. Checked:

- Locally (debug build, one tick per frame because the software rasterizer is slow): RGBA vs RGBA on the same backend
  differs by 0.00–0.09 mean; BC vs RGBA by 0.29 (GL) and 0.23 (sdlgpu) on `06_showcase`, 0.20 / 0.17 on
  `04_diorama`. The two backends agree.
- `test_render_sdlgpu` (the BC1/4/5/7 upload, sample and bound checks, and the BC5 normal unpack) passed on the 3070
  in r4.
- The sdlgpu path reviewed: format mapping (BC7/BC1 sRGB to the `_UNORM_SRGB` formats, BC5 to `BC5_RG_UNORM`), every
  cooked mip uploaded per level and layer, sampler `max_lod` 1000. Nothing wrong found.

The next round re-measures it deterministically (checklist below).

### 3. No `gpu_ms` on sdlgpu (fixed, #498)

SDL GPU has no timestamp queries. The device now has `DeviceCaps::fence_timestamps`: a timestamp outside a pass ends
the SDL command buffer with a fence, and a waiter thread stamps the CPU time each fence passes (`engine/rhi.md`). The
player's two frame timestamps under `-perf_capture` use it, so sdlgpu captures have `gpu_ms` for 2D and 3D frames
(49 of 50 frames on lavapipe in a 50-frame `extras_2d` capture). Forward3D's per-stage columns stay GL/Metal only.

## Parallel round findings

Report: `gpu-runs/2026-10-17-parallel/RAPOR.md`. Pod F ran the r4 checklist below on 0ad5735 (#487 + #498).

### 1. sdlgpu 2D `gpu_ms` near zero (fixed, #506)

3D frames read the same as GL (kit3d_tour p50 1.61–1.86 ms against GL's 1.61–1.96), but 2D frames, which are a few
microseconds of GPU work, read p50 0.001 ms (coop) and 0.000 ms (extras_2d) against GL's 0.008 / 0.026. The waiter
blocked in `SDL_WaitForGPUFences` and woke tens of microseconds after the GPU finished, so the frame's first stamp was
taken late and the second fence, already passed, collapsed onto it. The waiter now polls `SDL_QueryGPUFence` (yielding)
for up to 2 ms before it blocks, so a short frame's stamps are taken when the fences pass (`engine/rhi.md`).

### 2. `ibl_roughness` NVIDIA-specific (NVIDIA golden, #512)

One block of 300 at 0.974%, the same on the 3070, 4090 and Blackwell, under GL and sdlgpu, on three drivers. The
difference is 8 pixels on the boxes' bottom edges (4x MSAA coverage); the 4090 and Blackwell images are identical
(max difference 0). The golden check now looks in `linux-<backend>-nvidia/` first on an NVIDIA device; that directory
holds only `ibl_roughness`, taken from the 4090's actual images. The llvmpipe/lavapipe repo goldens are unchanged.

### 3. `editor_play_smoke` 0 ticks on the 4090 (fixed, #512)

90 frames without vsync took 13 ms there, less than one 60 Hz tick, so play never ticked. The test runs
`-sim.fast_forward 1 -sim.max_catchup 1` (one tick a frame).

### 4. `player_capture_no_display` (fixed, #512)

Not an NVIDIA problem: the pod sets `SDL_VIDEO_DRIVER=offscreen` (SDL 3's spelling), which the test did not unset, so
the first `platform::init` succeeded and the fallback line was never printed. The test unsets both spellings.

### 5. BC `auto` normal-map threshold (BC-PR)

Calibration on 14 Poly Haven 1k textures (`70_bc_calibration.csv`): at 99% unit normals only 4 of 14 normal maps
pick BC5 (pass fractions 0.56–1.00, most 0.89–0.99), while non-grey colour maps pass at most 0.003. The report's
90% would still miss `clay_block_wall` (0.8905), so the threshold is 85%: 13 of 14 (`forrest_ground_03`, 0.56, stays
BC7). The grey rule runs first, so roughness and AO maps (up to 0.80) never reach it. The size gate (0.27 of RGBA8)
counts the six textures of `05_ashen_court` only; the report's 0.381 is the whole pack and is not the gate's number.

## Next round: what to check

Run on the dev_claude head that has #506, #512 and BC-PR. Do not start pods from a thread; the coordinator does. The r4
checklist passed in the parallel round (pod F) and is not repeated, except where a fix touched it.

1. **sdlgpu `gpu_ms`:** `mce_player -perf_capture` with `-sim.fast_forward 1 -sim.max_catchup 1 -r.vsync 0
   -perf_warmup 60 -frames 600` on `09_coop` (arena), `extras_2d` and `kit3d_tour -split_views 0/2/3/4`, under
   `MCE_RHI=gl` and `MCE_RHI=sdlgpu`. Pass: `summary.gpu_ms.count` close to the frame count, and the 2D scenes' p50
   on sdlgpu at least GL's (0.008 / 0.026 ms on the 4090), not about 0.001. 3D p50 should stay within about 0.1 ms
   of the parallel round's (1.61 / 1.68 / 1.77 / 1.86 ms), and `frame_ms` within about 0.1 ms of its
   (2.20 / 2.35 / 2.44 / 2.58 ms): the waiter's poll is on its own thread.
2. **Goldens:** the full `test_golden_gl` on both backends, 3 runs each. Pass: 0 failing blocks, `ibl_roughness`
   included (it now reads `linux-<backend>-nvidia/`). On a non-NVIDIA GPU, report `ibl_roughness`'s block count
   against the plain golden, without adding a vendor directory.
3. **`ctest -L gpu`** on both backends, with the pod's usual `SDL_VIDEO_DRIVER=offscreen` for GL: 0 failures.
   `editor_play_smoke` and `player_capture_no_display` must pass on every pod (they failed on the 4090s).
4. **BC `auto`:** re-run the Poly Haven calibration (`70_bc_calibration.csv`'s 14 textures, cooked with
   `--textures bc`): 13 of 14 normal maps cooked as BC5 (all but `forrest_ground_03`), no colour map as BC5. Report
   the cooked pack sizes of `04_diorama`, `05_ashen_court` and `06_showcase` against RGBA8.
