# M-VISUAL — High-end 3D rendering (milestone record)

**Status:** active (step 1 in progress, 2026-10-13). Plan: ROADMAP §M-VISUAL; Linear SBA-6 … SBA-15.
**Why:** `Forward3D` suits stylized and low-poly 3D but has no compute path, GI, pre-filtered IBL,
screen-space effects, local-light shadows or temporal pipeline. Each step below is opt-in, keeps GL as the
reference backend (SDL GPU and Metal follow per the M-MAC parity rule) and leaves today's frame unchanged when off.

## Steps

| # | Step | Linear | State |
|---|---|---|---|
| 1 | Compute in the RHI; clustered light build on the GPU | SBA-6 | GL and SDL GPU (Vulkan) done; Metal compute open |
| 2 | Image-based lighting + reflection probes | SBA-7 | sky IBL, IBL texture source and runtime reflection probes, the editor bake and goldens done (all backends, fragment passes); real-GPU numbers on the Mac/device session |
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

## Step 2 — image-based lighting

**Sky IBL (part 1).** `Sky::ibl` turns it on per scene; `Forward3DDesc::ibl` / `-r.ibl 0` keep the resources
away. Three bakes, all full-screen fragment passes so GL, SDL GPU and Metal run the same code (no compute, no
cubemaps, no mip render targets in the RHI):

- **Pre-filtered sky:** an RGBA16F 128×64 equirect 2D array, one layer per GGX roughness 0.2, 0.4 … 1.0
  (64 importance samples, filtered importance sampling reads blurrier sky mips). Roughness 0 reads the sky
  itself, so mirror reflections keep the sky texture's detail; between 0 and 0.2 the two are blended.
- **SH9 irradiance:** a 9×1 RGBA16F target, one texel per coefficient (a 64×32 sphere integral), stored already
  convolved with the cosine lobe and divided by π, so `sh_irradiance(n) * albedo` replaces the two-color
  hemisphere (`DirectionalLight::ambient_*`), scaled by `Sky::diffuse_ambient`.
- **BRDF LUT:** 64×64 RGBA16F split-sum scale/bias of F0 (256 samples, Smith-Schlick k = α/2), baked once,
  replacing the analytic `env_brdf` fit.

The sky and pre-filter bakes re-run only when what `sky_radiance()` reads changes (gradient colors, texture,
intensity, additive flag; `Forward3DStats::ibl_baked`), before the frame's first timestamp, so they stay out of the
per-stage table. Samplers 12 (pre-filtered sky), 14 (LUT) and 15 (SH9) are `rhi::bank::kIbl*`. With `Sky::ibl`
false the shader takes the old branch: the frame is the one without IBL (gate below compares every pixel).

Gates (`test_render_gl`, and on Vulkan as `test_render_sdlgpu`): IBL off = the frame of a renderer without IBL
resources (0 pixels differ); a constant sky c bakes SH coefficient 0 = c·0.282095·4π and the others 0, the LUT
reflects all of F0 when smooth and head-on and less when rough, and a second frame with the same sky re-bakes
nothing; under a constant sky IBL matches the hemisphere ambient of the same color (luma within 8); with a blue
zenith over a red ground the floor and the box top are lit blue. 04_cubes takes `-r.sky_ibl 1` (key I).

**Budget (04_cubes, 256 point lights, 200 frames, Mesa llvmpipe — software, relative cost only):**

| stage | IBL off | IBL on |
|---|---|---|
| CPU scene build (ms) | 0.191 | 0.186 |
| CPU command recording (ms) | 0.263 | 0.257 |
| GPU main pass (ms) | 185.5 | 204.2 |
| GPU 3D frame total (ms) | 248.6 | 266.9 |

The main pass pays for 9 SH fetches and 2–3 equirect taps per lit pixel; the bakes run once per sky change.

**Reflection probes (part 2).** `Scene3D::add_reflection_probe` (up to `kMaxReflectionProbes` = 4; the kit
component `ReflectionProbe3D`: a world-axis box of `half_extents` around the entity, the capture point at
`offset`) replaces the sky's pre-filtered specular inside its box. Runtime capture, again fragment passes only:

- **Capture:** six 90° views of the scene from the probe into a 128×128 RGBA16F 2D array (own D32 array for
  depth), drawn with the main PBR pipelines over `Scene3D::probe_draws()` (every opaque submesh at LOD 0, culled
  or not), then the sky. Captures see the sun and the sky IBL but no shadows and no point lights.
- **Reproject:** the six faces into one 128×64 equirect through the same view-projection matrices.
- **Pre-filter:** into the probe's slot of the IBL array (`ibl_env()` grows to 5 + 4·6 layers once a scene has
  probes): the sharp capture at roughness 0, then GGX layers at 0.2 … 1.0 (256 samples, no mips to read).
- **Sampling:** the probe whose box contains the pixel deepest wins (fade over `blend_distance` from the box
  faces back to the sky); the reflected ray is intersected with the box and the hit seen from the capture point
  (box projection), so a room's walls line up in a mirror floor. Keep floors and walls inside the box by more
  than the blend distance.

A probe re-captures only when its position, box, `version` or the sky changes (`Forward3DStats::probes_captured`,
`probe_draws`), before the frame's first timestamp. Moving objects do not re-capture on their own: bump `version`.
The main pass pays one box test per probe and two equirect taps inside a box.

Gates: `test_render` (probe draws ignore the view frustum and skip blended submeshes; the fifth probe is
dropped), `test_kit` (`ReflectionProbe3D` extracts to the entity's box; disabled ones do not), `test_render_gl` /
`test_render_sdlgpu`: a red emissive wall behind a mirror floor under a dim sky; without a probe the floor
reflects the sky, with one it reflects the wall (red > sky + 60); a second frame captures nothing, a version
bump captures again; with `Sky::ibl` off probes are ignored.

**IBL source (part 3).** `Sky::ibl_texture` (kit: `Environment3D::ibl_texture` + `ibl_texture_intensity`) is an
equirect the bakes read in place of the visible sky, so the lighting can come from an HDR-ish environment while
the dome shows something else; the dome still draws the sky. The sky's slot of `ibl_env()` is now laid out like a
probe's: layer 0 the sharp source, then roughness 0.2 … 1.0 (probe slots start at layer 6). Below roughness 0.2
the visible sky is still read directly for full texture detail; with an IBL texture the sharp layer is read
instead. Gate (`test_render_gl` / `test_render_sdlgpu`): a red IBL texture under a blue sky turns the ground red
while the dome pixels stay within 2 of the blue sky's, and an unchanged texture re-bakes nothing.

**Baked probes (part 4a: format, cook, load).** A probe slot leaves the GPU as one RGBM RGBA8 strip
(`Forward3D::record_probe_export`, 128×384: the six layers stacked, range 16) and is stored as a `.mcenv` file
(`mce/cook/envmap.h`: a 24-byte header, then the layers in texture row order). The `envmap` importer cooks it to
a linear RGBA8 2D array of six layers, never ASTC (the alpha exponent would not survive block compression).
`ReflectionProbe::baked` / `ReflectionProbe3D::baked` name it; a resident bake of the right shape is decoded into
the probe's slot once (`Forward3DStats::probes_loaded`) and nothing is captured at runtime. Gates: `test_cook`
(the cooked header, layer bytes and a truncated file's error), `test_render_gl` / `test_render_sdlgpu` (a capture
exported, uploaded the way the cook stores it and loaded back reflects within 6 of the capture, still showing a
wall since removed from the scene; decoded once; without the bake the probe captures again).

**Editor bake (part 4b).** File ▸ *Bake reflection probes* (`EditorSession::bake_reflection_probes`) captures
every enabled `ReflectionProbe3D` of the open scene through the preview (`PreviewRenderer::bake_probes`: the
extract in bake mode, so every mesh is drawn whatever its cull distance or static-cell visibility, and existing
bakes are ignored), writes `assets/probes/<scene>/probe_<entity id>.mcenv` with its sidecar (a re-bake keeps the
file's GUID), hot-reloads them and points each component's `baked` at its file in one undo step. It refuses a
scene without probes or without `Environment3D.sky_ibl`. Gate: `test_editor_gl` (GL and SDL GPU): the refusal,
the file and sidecar, the bake mounted and not black, undo, the GUID kept on re-bake, and the preview then loading
the bake instead of capturing (0 validation errors).

**Golden images (part 5, FR-DBG-020).** `tests/render_gl/test_golden_gl` renders each step's scenes through the
shared 3D fixture (`tests/render_gl/fixture3d.h`) and compares them with `tests/render_gl/golden/<os>-<backend>/*.png`
(`golden.h`): 8×8 blocks, every block's mean luminance within 0.5% of full scale. Dev-machine-only and never a CI
gate (not registered with ctest): a missing golden is written and passes, `MCE_UPDATE_GOLDENS=1` rebaselines, and a
mismatch leaves `<name>.actual.png` and `<name>.diff.png` under the build's `golden_out/`. `scripts/golden.sh` runs
it three times per backend (0 flakes over 3 runs). PNG in and out is `mce/cook/image.h` (stb stays behind the
cook wall). Step 2's goldens: `ibl_off`, `ibl_sky`, `ibl_roughness` (four metal boxes, roughness 0.05 to 0.95),
`ibl_texture` and `probe_room`, baselined on Mesa llvmpipe (GL) and lavapipe (SDL GPU); later steps add theirs.

**Open in step 2:** real-GPU numbers (the budgets above are llvmpipe, relative cost only); they need a device and
go with the Mac session.

