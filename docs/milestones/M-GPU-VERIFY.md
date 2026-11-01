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
| parallel (2026-10-17) | d221a48, 0ad5735 | r4 checks pass (`parallax_cobble` 0 blocks, BC equal on both backends, sdlgpu `gpu_ms` 539/540); `ibl_roughness` NVIDIA-specific; sdlgpu 2D `gpu_ms` p50 about 0.001 ms; `editor_play_smoke` 0 ticks on the 4090; `player_capture_no_display`; BC `auto` misses most real normal maps | #506, #512, #521 (below) |
| r6 (2026-11-01) | e57210d (master) | `ctest -L gpu` on both backends, goldens x3 on both backends, visual milestones (14_seasons, fireflies/pollen/dust/flock/butterfly in 12_weather, 13_water, 11_daynight, 09_openworld dusk and sky IBL) against llvmpipe: nothing engine-side failed. `06_showcase` 1080p budget measured. Two harness findings: GL needs `SDL_VIDEODRIVER=offscreen` for NVIDIA (Xvfb's GLX is Mesa), Vulkan needs a display (the NVIDIA ICD has no `VK_EXT_headless_surface`) | no code change; `scripts/ci/gpu_compare.py`, `scripts/ci/runpod_agent.sh` (below) |

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

### 5. BC `auto` normal-map threshold (#521)

Calibration on 14 Poly Haven 1k textures (`70_bc_calibration.csv`): at 99% unit normals only 4 of 14 normal maps
pick BC5 (pass fractions 0.56–1.00, most 0.89–0.99), while non-grey colour maps pass at most 0.003. The report's
90% would still miss `clay_block_wall` (0.8905), so the threshold is 85%: 13 of 14 (`forrest_ground_03`, 0.56, stays
BC7). The grey rule runs first, so roughness and AO maps (up to 0.80) never reach it. The size gate (0.27 of RGBA8)
counts the six textures of `05_ashen_court` only; the report's 0.381 is the whole pack and is not the gate's number.

## r6 (2026-11-01): RTX 3070, master e57210d

One community-cloud RTX 3070 (8 GB, driver 595.99.02, $0.13/h), about 55 minutes, terminated afterwards (`list-pods` empty). Started from a thread at Sami's request (the "coordinator starts pods" rule above was lifted for this round). Release (RelWithDebInfo) build with `MCE_GPU_TESTS=ON`: 2151 steps in 8 minutes on the pod's cores.

**How to get the real GPU (the part nobody wrote down).** The pod image only needs `NVIDIA_DRIVER_CAPABILITIES=all`; then `libGLX_nvidia`, `libEGL_nvidia` and the Vulkan ICD (`/etc/vulkan/icd.d/nvidia_icd.json`) are mounted.
- **GL:** `SDL_VIDEODRIVER=offscreen`. Under `xvfb-run` the GLX is Mesa and the run is llvmpipe again (`glxinfo` says so); the offscreen driver goes through NVIDIA's EGL (NVIDIA's GLSL compiler messages, `error C0000`, show it).
- **sdlgpu (Vulkan):** `xvfb-run` plus `VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json` (else Mesa's lavapipe is also enumerated). `SDL_VIDEODRIVER=offscreen` does not work here: the NVIDIA ICD has no `VK_EXT_headless_surface`, so `SDL_ClaimWindowForGPUDevice` fails (`test_rhi_sdlgpu`, `test_render_sdlgpu`, `terrain_player_sdlgpu` fail under offscreen and pass under Xvfb).
- Set `XDG_RUNTIME_DIR` (a writable dir), or Vulkan prints an error per call.

**Results.**
- `ctest -L gpu` (87 tests): GL offscreen 84 pass, the 3 failures are the sdlgpu tests above (config, not engine). sdlgpu under Xvfb with `MCE_RHI=sdlgpu` for the whole label: 84 pass; the 3 failures are `player_capture_no_display` (expects no display, the Xvfb run has one), `test_rhi_gl` (Xvfb GLX is Mesa) and `test_mcp_agent_gpu` (see open item). The r5 asks "`editor_play_smoke` and `player_capture_no_display` must pass" hold on GL offscreen.
- `test_golden_gl` (47 cases, 739 assertions): 3 of 3 runs pass on GL and 3 of 3 on sdlgpu, 0 flakes.
- Visual milestones, real GPU against llvmpipe, same pinned tick, 1280x720 (share of pixels differing by more than 12/255): `14_seasons` autumn 0.00%, winter 0.00%; `13_water` 0.00%; `11_daynight` 0.02%; `12_weather` (carries `Firefly`, `Pollen`, `Dust`, `Flock` and `Butterfly`) 0.05% at tick 120 and 600; `09_openworld` (dusk, sky IBL, auto exposure, #954) 0.07% at tick 600 and 0.01% at tick 1500. Every run exited 0 and drew a non-blank frame; GL error count 0. This says the real driver draws what the reference rasterizer draws; whether the specks read as birds or butterflies still needs a human look at a windowed run (the milestone records say so).
- `06_showcase -sheet -sheet.w 1920 -sheet.h 1080` (1080p cells, means over the settled frames, GPU time):

| 3D frame total (ms) | current | highbase | high | medium | low |
|---|---|---|---|---|---|
| GL (NVIDIA) | 1.01 | 3.53 | 3.71 | 2.79 | 0.88 |
| sdlgpu (NVIDIA Vulkan) | 0.91 | 3.22 | 3.27 | 2.31 | 0.81 |

  The biggest stages at High on GL: GI volume update 1.18, main pass 1.24, shadows 0.31, velocity + TAA 0.20, SSAO 0.16 (llvmpipe's order was shadows, main pass, GI, SSAO). High at 1080p is far under the 16.7 ms frame on a mid-range desktop GPU. The mobile tiers (Low and Medium on a phone) are still a device measurement.

**Open items.**
- `09_openworld` `-frames 180` capture (`openworld_sample_player_gpu`) is not renderer-independent: two runs on the same renderer repeat to the digit, but the GPU frame (mean RGB 54/63/36) and the llvmpipe frame (41/87/10) differ because the simulation tick reached at frame 180 depends on wall-clock catch-up. Pinned ticks agree (above). A test that wants a comparable frame should use `-capture_tick`. At tick 120 the two renderers differ by 6% of pixels (mean abs 2.2), not at 600 or 1500; probably auto-exposure adapting on wall-clock time, not checked further.
- `test_mcp_agent_gpu` under `MCE_RHI=sdlgpu` on NVIDIA: `render_frame {software:true}` creates an offscreen window, which the NVIDIA Vulkan ICD cannot present to (no `VK_EXT_headless_surface`). The player reports a clean device-creation error, the test then stops on a failed `REQUIRE`. Not hit in the default GL route; a software render fallback for sdlgpu would need a different path (not planned).
- Not done: Lantern Keep (364 MB zip, not on the pod), Metal, phones (cannot run on a Linux pod), 2D budgets FR-R2D-090/100 and the split-screen cost numbers (`kit3d_tour_split_views` passes, its `gpu_ms` was not measured).

**Tools added.** `scripts/ci/gpu_compare.py <build> <ctest name>[@tick]...` runs a windowed capture test on the real GPU and on llvmpipe and prints mean colour, deviation and the differing-pixel share. `scripts/ci/runpod_agent.sh` is the pod entry used here: it installs the deps, prints a driver report and then runs `.runpod-jobs/<n>.sh` files it finds on its git branch in order (output in the pod log), and deletes the pod itself after `MCE_POD_MAX_HOURS` (4) if a thread forgot.

## Next round: what to check

Run on the dev_claude head that has #506, #512 and #521. Do not start pods from a thread; the coordinator does. The r4
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

## Open items from threads

- **Long windowed captures stall under load (2026-10-18, M-DAYNIGHT 4).** `sample_daynight_gpu` (`mce_player -capture_ticks 150,1050,2100,2700`, about 45 s of real time) finished in 36 to 82 s on its own, but under a full `--task` (`ctest -j 4` on xvfb, llvmpipe) it sat for over an hour with all four frames already written, and under `--premerge` it hit its 240 s timeout. The player simulates in real time, so a long `-capture_ticks` list is 45 s or more of wall clock, and the cause of the stall (a catch-up spiral under load, or a hang at exit) is not known. Workaround: the demo's GPU test is one frame at tick 150 with a 120 s timeout. The next GPU round should (1) run `sample_daynight_gpu` with the old tick list 20 times at `-j 4` and note which of the frames or the exit stalls, (2) check whether `-capture_ticks` could step the simulation deterministically instead of by wall clock.

Update 2026-10-18: even the one-frame version (`-capture_tick 150`, 4 s alone, 120 s timeout) timed out in another thread's loaded `--task` (M-IMPORTMEDIA, #576). It is now `slow` (integration run only) and `--task` keeps the headless `sample_daynight_headless_600`. 6 parallel copies under separate xvfb servers all passed locally, so the stall needs the full ctest load (one shared xvfb, `-j 4`); the next GPU round should reproduce it with `ctest -j 4 -L gpu` and a `gdb`/`py-spy`-style stack of the stuck `mce_player`.

Update 2026-10-18 (root cause found, integration run on 3ade7ddc): the stall was not load or a hang. Four `mce_player` windows on one Xvfb (`ctest -j 4`, one shared X server) with no window manager: only the first window keeps focus, the others get a focus-out, and the player's lifecycle rule (`pause_on_focus_loss`, FR-RUN-060) pauses the sim. A paused sim never reaches the capture tick, so the player renders frames forever (frame 288 and counting in gdb, llvmpipe threads busy) until the ctest timeout. Reproduced locally: 4 copies of the daynight command on one Xvfb, 1 finishes in 2.8 s and 3 time out; with `-pause_on_focus_loss 0` all 4 finish in 2.5 s. Why `--task` and older integration runs only sometimes failed: it depends on which windows start at the same moment. Fix: `-pause_on_focus_loss 0` (the flag Room3D, SolsticeReach, MarbleGarden and scene_demo already pass) plus `-sim.fast_forward 1 -sim.max_catchup 32` on both tests. Any new windowed capture test with `-capture_tick(s)` must pass `-pause_on_focus_loss 0`. The earlier "next round" items about this stall are closed.
