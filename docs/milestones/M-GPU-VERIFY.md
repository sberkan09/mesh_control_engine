# M-GPU-VERIFY — Real-GPU verification rounds (milestone record)

**Status:** ongoing. The Linux CI renders with Mesa llvmpipe (GL) and lavapipe (Vulkan, `MCE_RHI=sdlgpu`) only, so
real-GPU checks run in rounds on a rented RunPod RTX 3070 (driver 590.48). Each round's raw logs and its report
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

## Next round: what to check

Run on the dev_claude head that has #487 and #498. Do not start pods from a thread; the coordinator does.

1. **Golden `parallax_cobble`:** `test_golden_gl` under `MCE_RHI=gl` and `MCE_RHI=sdlgpu`, 3 runs each. Pass: 0
   blocks over 0.5%. If it still fails, keep `golden_out/linux-*/parallax_cobble.{actual,diff}.png` and report the
   worst block and count.
2. **Other goldens:** the full `test_golden_gl` on both backends must not regress. `ibl_roughness` (1 block, 0.97%)
   is a known machine-specific candidate; do not change its repo golden.
3. **BC vs RGBA, deterministic:** for `06_showcase` and `04_diorama`, on both backends, capture RGBA twice and BC once
   with `-frames 120 -capture <ppm> -sim.fast_forward 1 -sim.max_catchup 1 -r.vsync 0 -platform.error_box 0`
   (one fixed tick per frame, so frame 120 is the same moment every run). Report mean |R| and the share of pixels
   with |R| > 16 for RGBA-vs-RGBA (the noise floor) and BC-vs-RGBA, per backend. Also `06_showcase -sheet`
   (fixed 60 Hz settles) for RGBA and BC on both backends. Expected: BC-vs-RGBA is about the same on GL and sdlgpu
   and well above the noise floor only in textured detail. If sdlgpu BC-vs-RGBA is still over 2x GL's with a flat
   noise floor, keep the ppm files: that would be a real sdlgpu BC difference.
4. **sdlgpu `gpu_ms`:** `mce_player -perf_capture` with `-sim.fast_forward 1 -sim.max_catchup 1 -r.vsync 0
   -perf_warmup 60 -frames 600` on `09_coop` (arena), `extras_2d` and `kit3d_tour -split_views 0/2/3/4`, under
   `MCE_RHI=sdlgpu`. Pass: `summary.gpu_ms.count` close to the frame count (not 0). Compare p50 with GL on the same
   scenes: sdlgpu should read a little higher (fence wake-up latency), not several times higher. Also check
   `frame_ms` against r4's sdlgpu numbers (5.08 / 5.38 / 5.87 / 6.23 ms for 1–4 views): the two extra submits per
   frame should not move it by more than about 0.1 ms.
5. **`ctest -L gpu`** on both backends: `test_rhi_sdlgpu` includes the new fence-timestamp case; no new failures
   (`player_capture_no_display` is the known SDL offscreen one).
