# M-VISUAL2 — Rendering gaps left after M-VISUAL (milestone record)

**Status:** active (steps 1–2 done, 2026-10-15). Plan: ROADMAP §M-VISUAL2; Linear SBA-101 … SBA-105.
**Why:** the engine comparison (`assessments/motor-kiyaslama-eksikler.md`, item 10) lists what the 3D frame
still lacks next to Godot, Unity and Unreal after M-VISUAL. Every step is opt-in, keeps GL as the reference
backend with SDL GPU gated alongside, and leaves today's frame unchanged when off. Real-GPU and mobile budgets
and Metal pixels wait on the Mac/device session, as for M-VISUAL.

## Steps

| # | Step | Linear | State |
|---|---|---|---|
| 1 | Screen-space reflections | SBA-101 | done: GL and SDL GPU (gated + goldens), Low tier drops it; Metal untested |
| 2 | Spatial upscaler and contrast-adaptive sharpening | SBA-102 | done: GL and SDL GPU (gated + golden), saved player convars; Metal untested |
| 3 | Contact shadows | SBA-103 | not started |
| 4 | Material extensions: clear coat, sheen, anisotropy, transmission | SBA-104 | not started (replaced Hi-Z occlusion culling, which M-VISUAL step 9 already ships) |
| 5 | Kit/MCP reach, `samples/06_showcase`, docs and goldens | SBA-105 | not started |

## Step 1 — screen-space reflections

**What.** `ScreenSpaceReflections` on `Scene3D` (`set_reflections`), `Environment3D.ssr` and its knobs in the
3D kit, `Forward3DDesc::ssr` and `-r.ssr 0` as off switches, `QualitySettings::ssr` (off on Low). M-VISUAL step
8 had left SSR out as optional.

**How.** SSR reuses the half-resolution view normal + depth prepass SSAO already draws (`record_prepass`: it
runs once a frame when either effect is on, so a scene with both draws its opaques into it once). A
full-screen pass (`kSsrFS`, `mce.3d.ssr`, RGBA16F at half size) marches each pixel's mirror ray in screen space,
with uv and 1/z interpolated so the steps are perspective-correct, jittered per pixel by a 4×4 Bayer pattern.
A sample is a hit when it lies behind the depth buffer by less than `thickness` plus the step's own depth
travel, at a surface facing the ray; four bisections refine it. Requiring a facing surface keeps a ray from
stopping on its own start: on a grazing floor the half-size depth texels around the origin are in front of
the ray, and without the test half the floor's rays ended there (a checkerboard of misses in the first
build). The hit's view position is reprojected with last frame's jittered view-projection and last
frame's resolved HDR color is read there. Forward3D keeps that matrix and a valid flag; a resize, a new
Forward3D or the first frame reflect nothing, so SSR shows from the second frame. The confidence (alpha) fades
toward the screen's edges, the ray's end and rays turning toward the camera, and is 0 for misses and the sky.

The main pass (`kMeshFS`, sampler slot `bank::kSsr` = 3, the last free fragment slot of 16) reads the trace
at its screen position and blends it over the specular ambient (sky, IBL, probes) by
`confidence × clamp((max_roughness − roughness) / roughness_fade)`, through the same split-sum BRDF. Captures
(probes, the GI volume) bind a stand-in and never read it. A material at or above `max_roughness`, a miss, or
SSR off draws exactly the pixels it drew before.

**Gates.** `render3d_gl` (GL and `MCE_RHI=sdlgpu` on lavapipe): the probe room's mirror floor shows the red
wall from the second frame, the first SSR frame and the frame with it off are byte-identical, a roughness-0.8
floor is byte-identical with SSR on, and SSAO + SSR share one prepass. Goldens `ssr_room` on `linux-gl` and
`linux-sdlgpu`; every earlier golden is unchanged. `kit3d`: the Environment3D fields reach the scene and the
Low tier drops SSR.

**Known limits.** Moving objects reflect where they were last frame (reprojection uses the camera only); the
prepass has no normal maps, so the ray follows the geometric normal while the blend uses the mapped
roughness; rough reflections fade to the probes rather than blur. Budget on llvmpipe is not meaningful; the
real-GPU number joins the Mac/device session.

## Step 2 — spatial upsample and sharpening

**What.** `PostFx::upscaler` (`Upscaler::Bilinear`, today's path, or `Spatial`) and `PostFx::sharpen` (0..1);
`Environment3D.upscaler` (1 = spatial, the kit's default) and `Environment3D.sharpen`; the saved player
convars `r.upscaler` (`scene`, `bilinear`, `spatial`) and `r.sharpen` (-1 = the scene's). The temporal
upscaler stays `PostFx::taa` with `render_scale < 1` (M-VISUAL 5); with TAA on, the spatial upsample has
nothing to do and only the sharpen runs.

**How.** Both run on the display-space frame after the tonemap and FXAA, which then stay at the frame's own
size in `fin_src_` (RGBA8) instead of writing the output. The upsample (`kUpscaleFS`) reads the 4×4 source
texels around each output pixel and weights them with the polynomial Lanczos-2 approximation over offsets
rotated into the local edge's frame (the luma gradient of the four nearest texels, bilinearly weighted),
squeezed across the edge and stretched along it by how clean the edge is, then clamps to the four nearest
texels so the negative lobes cannot ring. The sharpen (`kSharpenFS`) is AMD CAS's filter: the pixel minus a
share of its four neighbors, largest where the local contrast is low, falling to none where it would clip.
With both on, the upsample writes `fin_up_` at the output size and the sharpen reads it. Both count in the
post stage's timing.

**Gates.** `render3d_gl` (GL and SDL GPU): a Bilinear frame and a Spatial frame at full size are
byte-identical to today's; at half scale the spatial frame's mean difference to the full-resolution 4× MSAA
frame is 0.83 against bilinear's 1.03 (llvmpipe and lavapipe alike); sharpen 0.8 raises the full-size frame's
edge energy by 47 % at a mean difference of 0.3 levels; `r.upscaler` and `r.sharpen` override the scene's
choice both ways. Golden `spatial_sharpen` on both backends; every earlier golden is unchanged.
