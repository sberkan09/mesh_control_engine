# M-VISUAL — High-end 3D rendering (milestone record)

**Status:** active (step 1 in progress, 2026-10-13). Plan: ROADMAP §M-VISUAL; Linear SBA-6 … SBA-15.
**Why:** `Forward3D` suits stylized and low-poly 3D but has no compute path, GI, pre-filtered IBL,
screen-space effects, local-light shadows or temporal pipeline. Each step below is opt-in, keeps GL as the
reference backend (SDL GPU and Metal follow per the M-MAC parity rule) and leaves today's frame unchanged when off.

## Steps

| # | Step | Linear | State |
|---|---|---|---|
| 1 | Compute in the RHI; clustered light build on the GPU | SBA-6 | GL and SDL GPU (Vulkan) done; Metal compute open |
| 2 | Image-based lighting + reflection probes | SBA-7 | — |
| 3 | SSAO / GTAO | SBA-8 | — |
| 4 | Local-light shadows | SBA-9 | — |
| 5 | Temporal pipeline (velocity, TAA, upscaling) | SBA-10 | — |
| 6 | Global illumination (irradiance-probe volume) | SBA-11 | — |
| 7 | Atmosphere and volumetrics | SBA-12 | — |
| 8 | Post completion | SBA-13 | — |
| 9 | Scale (GPU culling, foliage, decals, GPU particles) | SBA-14 | — |
| demo | High-profile sample side by side | SBA-15 | — |

## Step 1 — compute in the RHI

**RHI (PR #185).** `ComputePipelineDesc` + `RenderDevice::create_compute_pipeline`; `CommandBuffer::bind_storage_image`,
`dispatch`, `dispatch_indirect` and `barrier(barrier::k*)` (`glMemoryBarrier` on GL; the old no-op `barrier()` is
gone); `TextureUsage::SampledStorage`; `DeviceCaps::compute` and the work-group limits; `FrameStats::dispatches`;
`RenderDevice::read_buffer` for tests. GL reports `compute = true` (core since 4.3). SDL GPU and Metal report
`false` and skip the commands, so renderers keep their CPU paths there.

**Forward+ assignment on the GPU (PR after #185).** `ClusterLights::build` is split into `prepare()` (light pool,
slice constants, per-slice tile extents, the frustum-culled view-space light list: O(lights + tiles)) and
`assign()` (the per-cluster work). `Scene3D::build` only prepares; `Scene3D::clusters()` assigns on the CPU on
first use. When the device has compute, `Forward3D` runs `kClusterBuildCS` (one invocation per cluster, every
culled light tested against the cluster AABB in light order, lists compacted with an atomic counter) and binds its
output buffers as banks 4 and 6 for the main pass; the CPU assignment never runs. Off switches:
`Forward3DDesc::gpu_light_clusters` and `-r.gpu_light_clusters 0`.

The GPU lists are a superset of the CPU's (the radius test is inflated by 1e-4): an extra light has a window of 0
at every pixel of that cluster, and the fragment shader skips `att <= 0`, so the frame is bit-identical. Gate
`render3d_gl: the GPU light assignment draws the CPU assignment's frame` renders 300 point/spot lights both ways
and compares every pixel (0 differ). Known difference: on index-list overflow (`max_indices`) the GPU drops pairs
in cluster-completion order rather than light order; the drops are counted on the GPU and not read back.

**Budget (04_cubes, 256 point lights, 200 frames, Mesa llvmpipe — software, so only the CPU column and the
relative GPU cost mean anything; real-GPU numbers stay open like M5a's):**

| stage | CPU path | GPU path |
|---|---|---|
| CPU Forward+ cluster work (ms) | 0.301 | 0.015 (setup only) |
| GPU light assignment (ms) | 0 | 1.46 (llvmpipe) |
| GPU 3D frame total (ms) | 249.3 | 266.7 (llvmpipe) |

**SDL GPU (Vulkan).** `compile_glsl` gains `ShaderStage::Compute` and SDL's compute sets: set 0 samplers then
read-only storage buffers, set 1 storage images then read-write storage buffers, set 2 uniform banks; a buffer is
read-only when every block member is `readonly`, and the work-group size comes from the module's `LocalSize`.
Storage images always bind read-write (`StorageAccess::Read` too): SDL declares read-only storage textures as
sampled images, which GLSL's `imageLoad` cannot read; only non-array 2D images are supported. Every dispatch is
its own compute pass, because SDL binds read-write resources when a pass begins and does not order dispatches
inside one; that makes `barrier()` a no-op there. Buffers carry the compute storage usages; `read_buffer` downloads
through a transfer buffer. Caps report Vulkan's guaranteed minimums (128 invocations, 128×128×64, 65535 groups), as
SDL exposes no limits. Gates: the four `rhi_gl` compute cases run again as `rhi_sdlgpu:` on lavapipe, the SPIR-V
layout is checked without a GPU (`shader_spirv: compute resources follow SDL's compute layout`), and
`test_render_sdlgpu` runs the 300-light GPU-vs-CPU frame comparison on Vulkan (0 pixels differ). On Apple the
SDL GPU device keeps `compute = false` until the MSL path learns compute.

**Open in step 1:** compute on Metal (and SDL GPU's Metal driver); the Metal translate change in #185 is only
skipped `case` labels and has not been compiled on a Mac yet.
