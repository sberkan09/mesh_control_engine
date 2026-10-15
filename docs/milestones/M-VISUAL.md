# M-VISUAL — High-end 3D rendering (milestone record)

**Status:** active (steps 1–9 done, 2026-10-15). Plan: ROADMAP §M-VISUAL; Linear SBA-6 … SBA-15.
**Why:** `Forward3D` suits stylized and low-poly 3D but has no compute path, GI, pre-filtered IBL,
screen-space effects, local-light shadows or temporal pipeline. Each step below is opt-in, keeps GL as the
reference backend (SDL GPU and Metal follow per the M-MAC parity rule) and leaves today's frame unchanged when off.

## Steps

| # | Step | Linear | State |
|---|---|---|---|
| 1 | Compute in the RHI; clustered light build on the GPU | SBA-6 | done: GL, SDL GPU (Vulkan and Metal) and Metal |
| 2 | Image-based lighting + reflection probes | SBA-7 | sky IBL, IBL texture source and runtime reflection probes, the editor bake and goldens done (all backends, fragment passes); real-GPU numbers on the Mac/device session |
| 3 | SSAO / GTAO | SBA-8 | done: GL and SDL GPU (gated + goldens), Metal binds the stand-in; real-GPU budget on the Mac/device session |
| 4 | Local-light shadows | SBA-9 | done: GL and SDL GPU (gated + golden), Metal untested; real-GPU budget on the Mac/device session |
| 5 | Temporal pipeline (velocity, TAA, upscaling) | SBA-10 | done: GL and SDL GPU (gated + goldens), High tier on TAA; Metal untested; real-GPU budget on the Mac/device session |
| 6 | Global illumination (irradiance-probe volume) | SBA-11 | done: GL and SDL GPU (gated + goldens), editor bake, Low tier on the bake; Metal untested; real-GPU budget on the Mac/device session |
| 7 | Atmosphere and volumetrics | SBA-12 | done: GL and SDL GPU (gated + goldens), Low tier without the volumetric fog; Metal untested; real-GPU budget on the Mac/device session |
| 8 | Post completion | SBA-13 | done: GL and SDL GPU (gated + goldens), Low tier without depth of field and motion blur; SSR not taken (optional); Metal untested; real-GPU budget on the Mac/device session |
| 9 | Scale (GPU culling, foliage, decals, GPU particles) | SBA-14 | done: GL and SDL GPU (gated + goldens), CPU culling fallback without compute; Metal untested; real-GPU budget on the Mac/device session |
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

**Metal (SBA-6, the Mac session).** `translate_glsl` gains `ShaderStage::Compute`: the same binding map as the
graphics stages (uniform bank b at `[[buffer(b)]]`, SSBO binding b at `[[buffer(8 + b)]]`, sampler s at
`[[texture(s)]]`/`[[sampler(s)]]`) plus storage images, image unit u at `[[texture(16 + u)]]`; Metal takes the
work-group size at dispatch time, so the translation reports the shader's `local_size` (`MslSource::local_size`).
The device tracks 16 SSBO bindings (the cluster build uses 4 … 10). Dispatches go into a compute encoder on the
frame's command buffer (serial dispatch type, so dispatches inside it see each other's writes), one encoder per run
of dispatches; `barrier()` ends the run, and so does a render pass, a blit or a `write_timestamp`, which keeps
compute timed like any other encoder. Buffers and textures are hazard-tracked (Metal's default), so the next
encoder sees every write of the previous one: that is GL's `glMemoryBarrier` for every bit. Bindings are GL-style
persistent state and the whole table goes on before each dispatch. `SampledStorage` textures get `ShaderWrite`
usage; a mip other than 0 binds through a texture view of that level (all layers of an array). `read_buffer`
waits for an empty command buffer (the queue completes in commit order) and copies from the shared buffer. Caps:
`compute = true`, the device's `maxThreadsPerThreadgroup` per dimension (1024 × 1024 × 1024 on Apple silicon),
1024 invocations, 2³² − 1 groups per dimension, 8 image units; a pipeline whose local size exceeds its
`maxTotalThreadsPerThreadgroup` is refused.

**SDL GPU on Metal.** `spirv_to_msl` learns the compute stage with SDL's Metal compute layout: `[[texture]]` sampled
textures, then storage images (all read-write, as on Vulkan), `[[sampler]]` matching the sampled ones,
`[[buffer]]` uniform banks, then read-only storage buffers, then read-write ones. SDL GPU now reports
`compute = true` on Apple too (iOS and `MCE_RHI=sdlgpu` on macOS), with the same Vulkan minimums as caps.

Gates: `rhi_gl`'s four compute cases run as `rhi_metal:` (caps, a dispatch writing an SSBO, `dispatch_indirect`
reading arguments a dispatch wrote, a storage image written by one dispatch and read by readback and `imageLoad`
in the next, compile errors as Results), plus `rhi_metal: storage image at mip 1, then sampled by a compute
shader` (the view path) and `rhi_metal: a run of dispatches is timed like a pass`; `shader_cross: compute stage
keeps the binding map and reports its work-group size` checks the MSL without a GPU. `test_render_gl` on Metal
runs the 300-light GPU-vs-CPU frame comparison (0 pixels differ), and so does `MCE_RHI=sdlgpu test_render_gl` on
SDL GPU's Metal driver, whose `test_rhi_sdlgpu` compute cases pass there too (a macOS build with
`-DMCE_RHI_SDLGPU=ON`; the default macOS build does not compile SDL GPU). The validation stage
(`MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`) reports 0 errors. The #185 translate change is now compiled and run
on a Mac.

**Budget on a real GPU (04_cubes, 256 point lights, 200 frames, Release, Metal on an Apple M4; two runs, the
first shown, the second within 0.2 ms):**

| stage | CPU path | GPU path |
|---|---|---|
| CPU Forward+ cluster work (ms) | 0.094 | 0.004 (setup only) |
| CPU command recording (ms) | 0.246 | 0.097 |
| GPU light assignment (ms) | 0 | 0.114 |
| GPU main pass (ms) | 3.10 | 2.93 |
| GPU 3D frame total (ms) | 4.16 | 4.02 |

The compute pass costs about 0.11 ms of GPU time and saves about 0.09 ms of CPU cluster work and 0.15 ms of
command recording (the CPU path records the light lists every frame). The GPU frame total does not grow. "GL
translate + submit" is about 6.6 ms on both paths, so this step does not change it; it is left for a later look at
the Metal submit path.

**Open in step 1:** nothing. SDL GPU compute on an iOS device builds (`ios-arm64`) but has not run on the phone
yet; it goes with the next device session.

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


## Step 3 — SSAO

**Scene API.** `Scene3D::set_ambient_occlusion(AmbientOcclusion)`: `enabled` (off by default), `radius` (world
units), `intensity`, `power` and `bias` (a cosine threshold that ignores occluders near the tangent plane).
Off switches: `Forward3DDesc::ssao` (no pipelines or targets) and `-r.ssao 0`.

**Passes (half resolution, after the light clusters, timestamp `ssao`).** A prepass draws every mesh (static and
skinned, alpha masks honoured, no culling) into an RGBA16F target holding the view normal and view depth. The
occlusion pass takes 12 samples on a 3-turn spiral rotated by a 4×4 Bayer pattern; each sample is snapped to a
texel centre (otherwise flat ground self-occluded by ~4%), and an occluder in reach adds its cosine minus `bias`
times a `1 - d²/r²` falloff. Visibility is `clamp(1 - 2·intensity·sum/N)^power`. A separable 9-tap Gaussian
with a depth weight (`1 - |Δz| / 0.05z`) removes the noise without bleeding across edges. The main pass samples
bank 11 (`rhi::bank::kAo`) at the fragment's screen uv and multiplies the ambient term only: sun, local lights
and emissive are untouched. With AO off, bank 11 binds a 1×1 white stand-in (Metal needs every declared sampler
bound), so the main pass and the reflection-probe capture are unchanged.

**Gates.** `render3d_gl: SSAO darkens the ground at the box's foot, not open ground` (ambient only: foot
158→141, open ground and box top unchanged, two prepass draws) and `SSAO leaves direct light alone and off draws
the frame without it` (0 pixels differ in a direct-only scene; the AO target is half size, white on open ground,
dark in the crease). Goldens `ssao_crease` and `ssao_scene` on linux-gl and linux-sdlgpu; the earlier goldens are
unchanged. Real-GPU cost, and Metal pixels, belong to the Mac/device session.

**Budget (04_cubes `-ao 1`, 256 point lights, 1280×1024, 110 measured frames, Mesa llvmpipe — software, so
only the relative cost means anything; real-GPU numbers on the Mac/device session):**

| | AO off | AO on |
|---|---|---|
| SSAO pass (prepass + occlusion + blur, ms) | 0 | 36.5 |
| GPU 3D frame total (ms) | 384.4 | 432.2 |

## Step 4 — local-light shadows

**Scene API.** `PointLight::cast_shadows` (off by default) asks for a shadow; `Scene3D::set_local_shadows(LocalShadowSettings)`
sets the budget: `tile` (texels per side, default 512), `layers` (atlas layers, must match
`Forward3DDesc::local_shadow_layers`, default 1) and `max_lights` (16). Off switches: `enabled = false`,
`Forward3DDesc::local_shadow_layers = 0` (no memory) and `-r.local_shadows 0`. A mismatch between the scene's settings
and the renderer logs once and draws the lights unshadowed.

**Atlas.** The local shadows live in extra layers of the sun's cascade array (bank 9, the same `sampler2DArrayShadow`
and PCF 3×3 as the sun, no new sampler slot). Each layer is cut into `tile`-sized squares: a spot light takes one (a
perspective view down its cone, capped at 83° half-angle), a point light six (the cube faces +X −X +Y −Y +Z −Z, each
a little wider than 90° so a PCF footprint at a face edge stays inside its tile). Each tile is a depth pass with its
viewport on the tile; the first pass on a layer clears it, the rest load it. Casters are the shadow draws whose
bounding sphere touches the tile's frustum. Give a lamp's own bulb mesh `cast_shadows = false`, or it shadows its
light.

**Budget.** `Scene3D::build` ranks the lights that ask by screen importance: luminance × (radius / distance)²,
capped when the camera is inside the light. Lights whose sphere is off screen are skipped. Tiles go out in that
order; a point light that no longer fits leaves its room to later spot lights. The remaining lights are drawn
unshadowed that frame and counted in `Stats::local_shadows_dropped`.

**Shading.** `GpuPointLight` grows a `shadow` row (first record, record count; 64 bytes). The main pass reads the
records from std430 bank 11 (`rhi::bank::kLocalShadows`: tile view-projection, uv rect, layer and texel scale).
Point lights pick the face by the major axis of the light-to-fragment vector. The receiver gets the sun's normal
offset, scaled by the texel size at its distance, and the PCF taps are clamped inside the tile.

**Gates.** The `render3d_gl` tests:
- A shadowed spot leaves the floor behind the box at 26 against 153 lit. Open floor and the box's lit face don't change.
- A shadowed point light darkens the floor behind the box. Five floor points across the −Y, −X and +Z faces and their seams don't change, so there is no acne.
- With local shadows off, or with mismatched settings, 0 pixels differ.
- The budget test checks the importance order, the six tiles per point light, a point light dropped for a later spot, `max_lights`, and that unrequested or off-screen lights get no tiles.

Golden `local_shadows` (a warm spot and a cool point light at night) on linux-gl and linux-sdlgpu; the earlier
goldens are unchanged.

**Budget (04_cubes `-shadow_lights N`, 256 point lights, 2048² layer of 512 tiles, 1280×1024, 110 frames, Mesa
llvmpipe; relative only):**

| | no local shadows | 2 shadowed point lights (12 tiles) |
|---|---|---|
| shadow stage (cascades + local, ms) | 20.9 | 26.2 |
| GPU 3D frame total (ms) | 379.4 | 399.5 |

Asking 8 lights for shadows grants the 2 most important (16 tiles hold 2 cube lights) at the same cost. Metal pixels
and real-GPU numbers belong to the Mac/device session.

## Step 5 — temporal pipeline (TAA, temporal upscaling)

**API.** `PostFx::taa` (off by default) and `taa_blend` (the new frame's weight, 0.1). TAA replaces FXAA: the
FXAA pass is skipped on a TAA frame. `MeshInstance::prev_world` / `prev_pose` give last frame's world matrix and
node pose of a moving or animated instance (null / empty: only the camera moved it). `Forward3D::reset_history()`
drops the history on a camera cut. Off switches: `Forward3DDesc::taa` (no pipelines) and `-r.taa 0`. The High
quality tier now turns TAA on in place of FXAA (`QualitySettings::taa`), and kit scenes follow the environment's
anti-aliasing switch. `Kit3DExtract` remembers each entity's world matrix from the previous extract, so moving
MeshRefs carry their velocity; animated poses are not carried yet.

**Passes.** The projection is shifted by a Halton(2, 3) sub-pixel offset over 8 frames (main pass, sky and SSAO
prepass). After the MSAA resolve, a velocity prepass draws the opaques again at the 3D size into an RGBA16F target
with its own depth. It writes the unjittered uv motion since last frame (rigid draws through `prev_model`, skinned
ones through last frame's joints, which Scene3D appends to the palette), with b = 1 where a mesh is. The resolve
then reads this frame's sample under each output pixel at its jittered position. It fetches the history at the
reprojected uv (velocity, or the camera's reprojection of the far plane for the sky) with a 5-tap Catmull-Rom,
clips it to the 3×3 neighborhood's variance box in YCoCg over Karis-weighted colors, and blends. Bloom and the
tonemap read the result; timestamp `taa` covers the velocity pass and the resolve.

**Temporal upscaling.** The history lives at the output size, so with `render_scale` < 1 the resolve upsamples.
Each output pixel takes the input sample under it, weighted by how close that sample's jittered position landed
(`2·blend·exp(−1.5·d²)`, d in output pixels). Over the jitter cycle the samples cover the sub-pixel positions of
the full-size frame.

**Gates.** The `render3d_gl` tests, on GL and SDL GPU:
- TAA off draws today's frame, and frames after a TAA run match it again (0 pixels differ).
- On a still frame, 16 TAA frames bring the 85 edge pixels from 30.6 to 14.3 mean levels off a 4× MSAA reference, while open ground keeps its shade.
- A box sliding 0.3 m per frame ends 0.72 levels off its un-anti-aliased frame with velocity, against 0.95 without.
- A skinned pose's previous joints double the palette, and its draws point at them.
- A half-scale frame upscaled over 24 frames is 0.45 levels off the full-size reference, against 1.03 bilinear.

Goldens `taa_still` and `taa_upscale` on linux-gl and linux-sdlgpu; the earlier goldens are unchanged.

**Budget (04_cubes, 256 point lights, 1280×1024, 110 frames, Mesa llvmpipe; relative only):**

| | FXAA (4× MSAA) | TAA (4× MSAA) | TAA (no MSAA) |
|---|---|---|---|
| main pass (ms) | 277.2 | 275.8 | 234.7 |
| MSAA resolve (ms) | 7.7 | 10.3 | 0 |
| TAA: velocity prepass + resolve (ms) | 0 | 38.3 | 38.5 |
| FXAA (ms) | 19.6 | 0 | 0 |
| GPU 3D frame total (ms) | 357.3 | 377.4 | 329.7 |

Metal pixels and real-GPU numbers belong to the Mac/device session.

## Step 6 — global illumination (irradiance-probe volume)

**API.** `Scene3D::set_gi_volume(GiVolume)` (off by default): `bounds` (world box), `counts` (probes per axis,
2..32, corners included), `intensity`, `normal_bias`, `fade_distance`, `hysteresis` (the old value's weight per
update), `probes_per_frame` (rolling updates, 0 = frozen after the fill), `version` (bump to refill), `baked` (a
TextureStore key) and `updates` (false = the bake only). Kit: the `GiVolume3D` component (box around the entity,
`probes_x/y/z`, the same knobs; the first enabled one is used). Off switches: `enabled = false`,
`Forward3DDesc::gi = false` (no pipelines) and `-r.gi 0`. The Low quality tier sets `QualitySettings::gi_updates =
false`: the bake only, and no bake means no GI.

**Probes.** DDGI-style without ray tracing. Each probe stores the light arriving from every direction as four
tetrahedral samples V_k = (1/4π)∫L(1 + ω·d_k)dω (an L1 lobe that stays ≥ 0), in an RGBA16F volume texture of
(counts[0]·4) × (counts[1]·counts[2]) texels (row y + counts[1]·z, texel x·4 + k), sampled from bank 5
(`rhi::bank::kGiVolume`). Shading takes the eight surrounding probes, offset along the normal by `normal_bias`,
weighted trilinearly × a backface term ((dot + 1)/2)² + 0.05, and reads E/π = Σ V_k (0.25 + 1.5 d_k·n). Inside the
box this replaces the sky's diffuse ambient; the sky's reflections are scaled by how much of the sky the probes
see (their luma against the sky irradiance's), and the sun-side darkening of the sky ambient is skipped. Over
`fade_distance` outside the box the sky's ambient returns. With GI off the frame is bit-identical to before.

**Captures.** A probe renders six 16² faces with the main PBR pipelines into an RGBA16F array: the sun, shadowed
by its own orthographic fit over the volume (an extra layer of the cascade array, made on first use), the point
and spot lights (the 32 that matter most to that probe, ranked by luminance × r² / (d² + r²), as one cluster),
emissive surfaces, the sky and the volume itself, so each update adds a bounce. Each face draws the meshes inside
its frustum that are wider than a texel, at most the 96 widest. A projection pass (one per run of probes along a
volume row) integrates the faces into the four lobes and blends them in by 1 − `hysteresis` (1 for the fill).

**Fill and rolling updates.** A new volume (first frame, new bounds, counts, version or bake) clears and fills:
at most 32 probes a frame, fewer when the frame's capture budget runs out (1024 uniform uploads, a draw one and a
face three, the first probe always goes), so a crowded scene keeps its frame inside the 1 MiB uniform ring. The
volume lights the frame from the frame it is full (`Forward3DStats::gi`). After that `probes_per_frame` probes
are re-captured each frame in x-major order, under the same budget. The `gi` timestamp covers the sun fit, the
captures and the projection.

**Bake.** `Forward3D::record_gi_export` draws the volume as RGBM (range 16) RGBA8; the editor's File > Bake GI
volume runs the fill without hysteresis plus two sweeps, reads it back, writes `assets/gi/<scene>/gi_<id>.mcenv`
and sets the component's `baked` (one undo step). A loaded bake is decoded into the volume and replaces the fill.

**Gates.** The `render3d_gl` tests, on GL and SDL GPU:
- GI off draws today's frame, frames after a GI run match it again, and the Low tier without a bake leaves no trace (0 pixels differ).
- The ground in front of an emissive red wall goes from the sky's 125,135,156 to 200,55,64 once the volume fills (32 probes, then 16), and rolling updates then capture 8 a frame.
- The box face away from the sun goes from luma 1 to 125 with sunlight bounced off the ground.
- A lamp switched on reaches a far ground point only through bounces: green 60 on the first frame, 78 after 24 rolling frames, against 77 for a full refill.
- A volume exported as RGBM and loaded back lights like the capture; the Low tier draws the bake.
- 400 posts through the volume fill over 17 frames without a uniform-ring overflow, each face at most 96 draws.

The editor test bakes a volume, reloads it and checks the bake lights the scene. Goldens `gi_bleed` and `gi_bounce`
on linux-gl and linux-sdlgpu; the earlier goldens are unchanged.

**Budget (04_cubes `-gi 1`: 7 × 3 × 7 probes over the panel cubes, 7 probes a frame, 256 point lights, 1280×720,
110 frames, two runs each, Mesa llvmpipe; relative only):**

| | GI off | GI on |
|---|---|---|
| GI update: sun fit + 7 probes' captures + projection (ms) | 0 | 51.8 |
| main pass (ms) | 280.4 | 349.7 |
| GPU 3D frame total (ms) | 362.9 | 487.8 |

The main pass pays for the eight-probe lookup (32 volume fetches a pixel, every pixel of 04_cubes sits inside the
volume). Capture frames stay at about 450 KiB of the 1 MiB uniform ring (the frame alone: 150 KiB).

**Limits.** Light leaks through walls thinner than the probe spacing, and probes buried in walls darken what is near
them (no probe relocation or visibility term yet): keep the grid's planes off walls. Diffuse only; reflections stay
with the sky and the reflection probes. Lightmaps were not needed for the step's gates. Metal is untested; Metal
pixels and real-GPU numbers belong to the Mac/device session.

## Step 7 — atmosphere and volumetric fog

**API.** `Scene3D::set_atmosphere(Atmosphere)` and `Scene3D::set_volumetrics(Volumetrics)`, both off by default.
With both off the frame is bit-identical to before. Off switches: `enabled = false`; for the fog also
`Forward3DDesc::volumetrics = false` (no pipelines), `-r.volumetrics 0` and the Low quality tier
(`apply_quality(q, Volumetrics&)`: Low turns it off, Medium coarsens the grid to 16 px tiles × 48 slices, High
allows 8 px × 64). Kit: `Environment3D.atmosphere` (`atmosphere_intensity`, `atmosphere_haze`), `time_of_day`
(hours on a simple day's arc, `sun_direction_at`; -1 keeps `sun_direction`) and `volumetric_fog`
(`volumetric_density`, `volumetric_height_falloff`, `volumetric_anisotropy`, `volumetric_albedo`,
`volumetric_distance`). 04_cubes: `-atmos 1`, `-hour H`, `-volfog 1`.

**Atmosphere.** Single scattering through an Earth-like atmosphere (Rayleigh air, Mie aerosols, ozone absorption;
Hillaire 2020's constants), computed on the CPU in double precision: a transmittance table (32 × 256) and a
sky-view table (64 elevations × 25 azimuths, 24 steps a ray). `Scene3D::build` tints the sun's color by the air
between the ground and the sun (orange at dusk, black below the horizon) and sets the ambient from the sky's
irradiance and the lit ground (`Atmosphere::sun_transmittance`, `ambient`). Forward3D bakes the sky into a 256 × 128
equirect RGBA16F texture with mips that replaces the gradient and the sky texture, for the dome, the reflections and
the IBL (which re-bakes with it). The sun direction is quantized to `rebake_degrees` (0.25°), so a moving sun
re-bakes in steps (about 9 ms of CPU a bake) and a still one never (`Forward3DStats::atmosphere_baked`).

**Volumetric fog.** A froxel grid over the view: `tile` px columns × `slices` depth slices, log-spaced out to
`max_distance`, stored as an RGBA16F atlas (8 slices a row). The inject pass evaluates a height fog
(`density` × exp(−`height_falloff` · (y − `base_height`))), lit by the sun through the shadow cascades with a
Henyey-Greenstein phase (`anisotropy`), the clustered point and spot lights with their local shadows, and the
ambient; it stores scattered light and extinction. The integrate pass walks the slices front to back
(energy-conserving) into in-scattered light and transmittance. The main and sky passes apply it per pixel (bank 4,
`rhi::bank::kVolumetrics`) in place of the exp² fog. The sky pass carries the fog, so a scene with the sky off
clears to the fog color without it. The `volumetrics` timestamp covers both passes.

**Gates.** The `render3d_gl` tests, on GL and SDL GPU:
- Atmosphere and volumetrics off draw today's frame, frames after a run with them match it again, and the Low tier's fog leaves no trace (0 pixels differ).
- The noon sky is blue (81,124,164); the lit ground goes from 234,232,230 at noon to 83,56,37 at 17:36 (red/blue 1.0 → 2.2).
- The sky bakes once per sun position, its IBL with it; a sun moved by less than `rebake_degrees` keeps the bake.
- Looking into a low sun, forward scattering (g = 0.7) is brighter than backward (g = −0.7), and the other way round with the sun behind.
- A roof's shadow cuts a shaft out of the sunlit fog: luma 90 under it against 124 with the roof casting no shadow; the open side does not change.
- A point light short of the ground glows orange in the fog around it.

Goldens `atmosphere_sunset` and `volumetric_shafts` on linux-gl and linux-sdlgpu; the earlier goldens are unchanged.

**Budget (04_cubes, 256 point lights, 1280×1024, 110 frames, two runs each, Mesa llvmpipe; relative only):**

| | off | `-atmos 1` | `-volfog 1` | both |
|---|---|---|---|---|
| volumetric fog: injection + integration (ms) | 0 | 0 | 47.0 | 47.0 |
| main pass (ms) | 300.1 | 362.4 | 311.8 | 380.0 |
| GPU 3D frame total (ms) | 388.5 | 451.3 | 443.8 | 513.1 |

The atmosphere's main-pass cost is the sky texture's: the reflections sample the baked sky as they would a sky
texture (the gradient is arithmetic). The fog's grid is 80 × 64 × 64 froxels at 16 px tiles.

**Limits.** Single scattering only (no multiple scattering: twilight skies are darker than real ones), a flat
ground and the viewer near it (`altitude`; world y is not used). The fog is a height fog without noise or local
volumes, and has no temporal reprojection, so thin shafts alias at coarse tiles. Metal is untested; Metal pixels and
real-GPU numbers belong to the Mac/device session.

## Step 8 — post completion (auto exposure, color grade, depth of field, motion blur)

**API.** Four more `PostFx` members, all off by default; with them off the frame is bit-identical to before.
`auto_exposure` (`key`, `compensation` in EV, `min_exposure` / `max_exposure`, `low_percent` / `high_percent`,
`speed_up` / `speed_down` in EV per second, `dt`), `lut_key` + `lut_strength` (the 2D path's 256 × 16 strip LUT, see
`make_identity_lut`), `dof` (`focus_distance`, `focus_range`, `blur_range`, `max_radius` as a fraction of the view
height) and `motion_blur` (`intensity` as a shutter fraction, `max_length`, `samples`). Off switches:
`Forward3DDesc::post_fx = false` (no pipelines) and `-r.post_fx 0`; the Low quality tier drops depth of field and
motion blur (`QualitySettings::dof_motion_blur`). Kit: `Environment3D.auto_exposure` (`exposure_compensation`,
`auto_exposure_speed_up/down`), `color_grade` (a texture imported with `"srgb": false, "mips": false`) and
`color_grade_strength`, `depth_of_field` (`dof_focus_distance`, `dof_focus_range`, `dof_blur_range`,
`dof_max_radius`) and `motion_blur` (`motion_blur_intensity`); the player passes the frame's wall time as the
adaptation step (a capture frame a fixed 1/60 s). 04_cubes: `-autoexp 1`, `-dof <meters>`, `-mblur 1`.

**Order.** The velocity prepass (M-VISUAL 5) now runs for TAA, motion blur or depth of field, and also writes the
view depth (alpha). After TAA: depth of field, then motion blur, each into an RGBA16F ping-pong target at the post
size, then the exposure histogram of the result; bloom and tonemap follow. The `post_fx` timestamp covers the
three.

**Auto exposure.** A compute pass bins the log2 luminance of every other texel each way into 64 bins (bin 0 below
2⁻¹², 63 bins up to 2¹², shared-memory histograms per 16 × 16 group, then global atomics); a single 64-thread
group drops the darkest `low_percent` and the brightest above `high_percent`, averages the rest and moves the
exposure toward key / average (clamped) by 1 − e^(−rate·dt) in EV, faster when the frame brightens. It empties the
bins for the next frame. The result stays on the GPU (bank `kExposure`, `Forward3D::exposure_buffer()`): the
bloom bright-pass and the tonemap multiply `Scene3D::exposure()` by it. The first frame, and the first after a frame
without it or `reset_exposure()`, snaps.

**Color grade.** In the tonemap pass, on the display-space (sRGB) values after ACES, as the 2D post pass does:
mixed by `lut_strength`, encoded back to linear when the target is an sRGB format.

**Depth of field.** One full-resolution pass: a 32-tap Vogel disk out to the largest circle of confusion
(`max_radius`), the circle growing from 0 within `focus_range` of `focus_distance` to the largest over
`blur_range`. A tap counts as far as its own circle reaches; a tap behind this pixel counts only as far as this
pixel's own circle reaches, so a blurred background does not spill over a sharp foreground (a blurred
foreground does spill over the background). A sharp pixel passes through exactly.

**Motion blur.** The velocity prepass's motion (objects with their previous transforms, skinned poses included),
the camera's reprojection for the sky; scaled by `intensity`, capped at `max_length`. Each pixel takes the longest
velocity of 16 neighbors on two rings out to `max_length` and samples along it; a tap counts where its own blur
or this pixel's reaches the other (McGuire et al. 2012 without the depth test), so a moving object smears over
the still background beside it. Under half a pixel of motion a pixel passes through exactly.

**Gates.** The `render3d_gl` tests, on GL and SDL GPU:
- Post effects off draw today's frame; frames after a run with all four match it again, and the Low tier's depth of field and motion blur leave no trace (0 pixels differ).
- Auto exposure: a sun 10 × dimmer gives luma 111 against 236 with the fixed exposure, 102 against 97 with auto exposure (×0.88 and ×0.08, measured averages 0.21 and 2.21); one 60 Hz frame after the switch it has moved from ×0.88 to ×0.78 only; the bins are empty after each frame.
- The identity LUT keeps the frame (box 190,117,91 both ways); the inverted LUT flips it (65,138,164; ground 227 → 27), half strength gives 128.
- Depth of field focused on the box keeps its center pixel and most of its edges while the far ground edge loses 78% of its edge energy; focused far, the box loses 83%.
- Motion blur of a box moved 0.6 m in a frame softens the edges in its band by 21%; with motion blur on but nothing moving, the frame is bit-identical to motion blur off.

Goldens `dof_focus` and `motion_blur_grade` (with auto exposure and a warm grade) on linux-gl and linux-sdlgpu; the
earlier goldens are unchanged.

**Budget (04_cubes, 256 point lights, 1280×1024, 110 frames, two runs each, Mesa llvmpipe; relative only):**

| | off | `-autoexp 1` | `-dof 12` | `-mblur 1` | all three |
|---|---|---|---|---|---|
| velocity prepass (ms) | 0 | 0 | 8.9 | 8.6 | 8.4 |
| post effects (ms) | 0 | 4.8 | 154.9 | 104.2 | 255.4 |
| GPU 3D frame total (ms) | 395.1 | 405.1 | 570.0 | 516.7 | 661.1 |

Depth of field is the most expensive (33 color and 33 depth taps a pixel at full resolution); motion blur's
neighborhood search reads 17 velocities a pixel before its taps. A half-resolution depth of field is the obvious next
step if a real GPU puts it over budget.

**Limits.** SSR (optional in the plan) is not part of this step. The depth of field is a single gather (no
separate near and far layers or bokeh shapes), and the motion blur has no depth test, so a moving object behind a
still one can smear over its edge. Metal is untested; Metal pixels and real-GPU numbers belong to the Mac/device
session.

## Step 9 — scale (GPU culling, instanced foliage, decals, GPU particles)

**API.** `Scene3D::add_batch(InstanceBatch)`: many copies of one rigid mesh (`transforms`, kept alive until the
scene is drawn; `id` stable, `version` bumped when the matrices change), with `tint`, a `material` override,
`cast_shadows`, `cull_distance` and a foliage wind (`wind_amplitude` × h² metres sideways, h the height above
the copy's origin, at `wind_frequency` Hz along `wind_direction`, a phase that varies over the ground).
`scatter_foliage(FoliageScatter)` fills a jittered, shuffled grid of yawed and scaled matrices (deterministic
in `seed`). `InstanceCulling` picks the GPU path (`gpu`, `-r.gpu_culling 0`) and its occlusion test
(`occlusion`, `occluder_divisor`, `-r.occlusion 0`). `Scene3D::add_decal(Decal)` projects a texture (or a plain
color) down a box; `Scene3D::add_particle_emitter(ParticleEmitter)` simulates sprites on the GPU.
`Scene3D::set_time(seconds, dt)` is the clock of the wind and the particles. Kit: `Foliage3D` (scatters at the
entity and re-scatters when its inputs change), `Decal3D` (`size` metres at the entity's transform) and
`ParticleEmitter3D` (bump `burst_version` for a burst); the extract runs the clock on the frame dt.
04_cubes: `-foliage <count>`, `-decals 1`, `-particles 1`. With none of them used, the frame is bit-identical.

**RHI.** `CommandBuffer::draw_indirect_buffer(buffer, offset, count)`: indexed draws whose commands a dispatch
wrote (GL `glMultiDrawElementsIndirect`, SDL GPU `SDL_DrawGPUIndexedPrimitivesIndirect`, Metal one indirect
draw per record). Gated on GL and SDL GPU by a compute pass that writes the commands.

**Culling.** The batches' matrices live in a storage buffer per batch id, uploaded when the version, count or
mesh changes. Every (view, batch) gets one indirect command per (LOD level, submesh) and a visible list per
level; the views are the main view and each shadow cascade (when a batch casts shadows). A compute pass resets
the commands from a template, then one dispatch per (view, batch) tests each instance's bounding sphere against
the view's planes, the cull distance and, in the main view, the depth pyramid; a survivor takes a slot in its
level's list with an atomic add on that level's commands. The instanced shaders read
`i_world[i_visible[slot + gl_InstanceID]]`. The level is the per-instance pick of `MeshInstance` (screen-space
error from the main camera, `shadow_bias` coarser in the cascades) without the hysteresis. Without compute,
the same tests minus the pyramid run on the CPU (`cull_batch`) and each list draws instanced: the same frame.

**Occlusion.** The frame's rigid, opaque, non-masked draws render their view depth into an RGBA32F target at
1/`occluder_divisor` of the frame (cleared far); four compute reductions (4 × 4 each, the farthest depth) make
a five-level pyramid. An instance is hidden when the nearest point of its sphere lies behind the farthest
occluder depth over its screen rectangle, read at the level where the rectangle spans at most two texels. The
cascades are not tested (a hidden copy still casts its shadow).

**Decals.** In the lit pass (`kMeshFS`), before lighting: up to 64 records (inverse box matrix, uv rect, color,
roughness / metallic / material weight / emissive, fades) in a storage buffer and up to three textures; a
surface point inside a box takes the decal's color by its coverage × opacity × the angle and edge fades, and its
roughness and metallic by the material weight; emissive decals add light. Derivatives are taken outside the
loop (`textureGrad`), so the texture filters cleanly at the box edges. `Scene3D::build` culls them against the
view and drops what is past 64 or past three distinct textures (stats). Probe and GI captures take no decals.

**Particles.** One ring buffer per emitter id (two vec4 per particle), kept across frames. A dispatch per
emitter spawns this frame's new particles into ring slots [cursor, cursor + n) from a hash of (seed, particles
spawned so far, slot) and moves every live one (gravity, drag, an optional bounce off a ground plane). They draw
after the blended draws as camera-facing quads (six vertices per particle, the dead ones collapsed), additive
or premultiplied alpha, depth tested without writing, unsorted and unlit.

**Gates.** `render3d_scale` (test_render_gl), on GL and SDL GPU:
- Batches, decals and particles off draw today's frame; after frames with all three the frame matches it again (0 pixels differ).
- GPU-culled batches draw the CPU path's frame exactly (0 pixels differ); both keep the 40 copies in view and drop the 5 behind the camera; a batch's shadows change the frame.
- Occlusion: of 18 copies, the 9 behind a wall are dropped (18 → 9 visible), the frame is unchanged and the cascades still draw all of them.
- Wind: with an amplitude the frame changes over time; without one the clock changes nothing.
- A decal turns the ground under it blue and leaves the ground outside unchanged, one behind the camera is culled; an emissive decal glows in a shadow.
- A burst of 200 particles glows (additive HDR) and survives into the next frame; an emitter not added frees its ring.

Headless (`test_render`): `scatter_foliage` is deterministic, inside its rectangle, upright and spread out;
`cull_batch` keeps the frustum and the cull distance and matches `batch_lod`; `Scene3D` counts batches, culled and
dropped decals and clamps an emitter. Kit (`test_kit`): the three components become a batch, decals and an
emitter; a changed seed re-scatters under a new version; a bumped `burst_version` bursts once.
Goldens `scale_foliage_decal` and `particles_burst` on linux-gl and linux-sdlgpu; the earlier goldens are
unchanged.

**Budget (04_cubes, 64 point lights, 1280×720, 60 frames, debug build, Mesa llvmpipe; relative only):**

| | off | `-foliage 20000` | ... `-r.occlusion 0` | ... `-r.gpu_culling 0` | `-decals 1` | `-particles 1` |
|---|---|---|---|---|---|---|
| scene build, CPU (ms) | 0.46 | 7.79 | 7.76 | 7.92 | 0.40 | 0.35 |
| scale stage: occluders + pyramid + culling + particles (ms) | 0 | 13.5 | 3.7 | 0 | 0 | 1.8 |
| shadows (ms) | 23.5 | 168.4 | 170.9 | 164.6 | 21.0 | 21.3 |
| main pass (ms) | 196.4 | 713.6 | 793.7 | 759.6 | 196.4 | 186.1 |
| GPU 3D frame total (ms) | 289.5 | 969.2 | 1042.1 | 997.1 | 282.5 | 271.6 |

The 20000 pebbles are textured panel cubes (12 triangles each) under three cascades, so the shadow and main
passes dominate on a software rasterizer. The depth pyramid costs about 10 ms and saves about 80 ms of main
pass here (the panel cubes and the orrery hide some of the scatter). The extra 7.3 ms of scene build is the batch's
bounds pass over 20000 matrices in a debug build; a caller that knows its bounds sets `InstanceBatch::bounds`
and skips it. Decals and the 4096-particle fountain are within the run-to-run noise of llvmpipe.

**Limits.** Batches are rigid (skinned meshes are skipped) and do not appear in local-light shadows,
reflection probes or the GI volume; LOD has no hysteresis per copy. Decals have no normal maps, at most three
textures a frame, and also cover the copies of batches inside them. Particles are unsorted and unlit, take no
collision but a ground plane and no shadows. Metal is untested; Metal pixels and real-GPU numbers belong to the
Mac/device session.
