# M-LENSFLARE — Lens flare: a soft glare halo, ghosts along the sun-to-centre axis and an optional starburst, occluded by scene depth (milestone record)

**Status:** planned (2026-11-03), steps 38a to 38f. **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of the 3D visual assessment (a bright sun reads as a flat disk: the frame has bloom, lens dirt and light shafts but nothing that says "a camera looked at the sun") and of M-VISUAL3 3 (the camera look in the tonemap pass: vignette, grain, chromatic aberration, lens dirt). Today the renderer draws a sky sun disk (`Sky::sun_size`, `Sky::sun_intensity`), bloom (`PostFx::bloom*`), lens dirt (`PostFx::Lens::dirt_key` / `dirt_intensity`, lit by bloom) and volumetric light shafts (`Volumetrics`), and has no flare. No Linear issues (free issue limit, Linear unreachable); this record is the tracker. The pass step (38c) is Opus-written and merged after an independent Opus review plus a green `--task`, as M-CLOUDS 9b, M-AURORA 16c and M-METEOR 18c were.

**Naming:** no type, file or milestone called `LensFlare`, `LensFlare3D`, `SunGlare` or `lens_flare` exists (`projects/`, `src/`, `docs/`, `tests/`, `samples/` checked 2026-11-03; the only "flare" hits are the mirror flares of the `solstice-reach` game design, which are point lights, not a post effect). The render setting is `render::LensFlare` (in `PostFx::flare`), the pure helpers live in `mce/render/lens_flare.h`, the kit component is `LensFlare3D` (`mce/kit/lens_flare.h`).

**Goal.** When the sun is on screen, an opt-in post pass adds a soft glare halo around it, a few tinted ghost discs along the line from the sun through the screen centre and, optionally, a starburst of thin rays. A 1×1 occlusion pass samples the view depth around the sun's projected position, so a wall, a tree or a character in front of the sun hides the flare in proportion to how much of the sun it covers. The flare fades as the sun nears the screen edge, sinks to the horizon or goes behind the camera, and with cloud cover and rain (folded in by the kit). It is added to the HDR frame **before bloom**, so bloom, lens dirt and the tonemapper act on it like on any bright light. **Render-side only: nothing is written to a component and nothing is hashed.** With `PostFx::flare.enabled` false (the default), no `LensFlare3D`, the Low tier or `r.lens_flare 0`, the pass is not recorded and every frame and golden is bit-identical.

**Not in scope:** flares of point, spot or emissive lights (the sun only; a per-light flare is a follow-up), a moon flare, authored flare textures or `.mcflare` assets (the elements are procedural), anamorphic streaks, temporal smoothing of the occlusion (each frame is measured on its own), transparent or particle occluders (the depth used is the opaque prepass; glass and smoke do not hide the sun), cloud-layer occlusion per pixel (the kit dims by the cloud coverage number), a Luau API (the component has no runtime state to script) and the MCP `render_frame` `environment` override (it covers `Environment3D` fields only).

## Design

- **Render setting (seam, 38a).** A top-level POD `render::LensFlare` in `scene3d.h`, just above `PostFx`, and a member `LensFlare flare;` in `PostFx` after `lens`. Fields, defaults and the ranges `apply_quality` clamps to (NaN falls back to the default):

  | Field | Default | Range | Meaning |
  |---|---|---|---|
  | `bool enabled` | false | | off draws the frame without the pass |
  | `f32 intensity` | 1.0 | 0..4 | overall gain on the sun's light (`DirectionalLight::color * intensity`) |
  | `f32 visibility` | 1.0 | 0..1 | the caller's dimming (the kit folds cloud cover and rain into it) |
  | `f32 halo` | 1.0 | 0..4 | strength of the soft glare around the sun |
  | `f32 halo_radius` | 0.25 | 0.02..0.6 | halo falloff radius, fraction of the view height |
  | `u32 ghost_count` | 4 | 0..`kLensFlareMaxGhosts` (6) | ghosts drawn |
  | `f32 ghost_intensity` | 1.0 | 0..4 | gain on the ghost table |
  | `f32 ghost_spacing` | 1.0 | 0.25..2 | scales each ghost's position along the axis |
  | `f32 ghost_chroma` | 0.5 | 0..1 | 0 = white ghosts, 1 = the full tint table |
  | `f32 starburst` | 0.0 | 0..4 | ray strength; 0 = no starburst |
  | `u32 starburst_rays` | 6 | 2..16 | rays (the shader draws `rays` symmetric spokes) |
  | `f32 occlusion_radius` | 0.015 | 0.002..0.1 | radius of the depth sample disk around the sun, fraction of the view height |
  | `f32 edge_fade` | 0.1 | 0.01..0.5 | the flare fades to 0 over this fraction of the screen as the sun reaches a border |

  `scene3d.h` is at 172 of 978 TUs (`include_fanout.py`, 2026-11-03, over the 150 warning) and `forward3d.h` at 155, so 38a is the seam PR for both, data only, merged first, with the numbers in its description. In `forward3d.h` it adds `bool lens_flare = false;` and `f32 lens_flare_strength = 0.0f;` to `Forward3DStats`, and the private members 38c needs (`flare_occ_pipe_`, `flare_pipe_`, `flare_occ_` (the 1×1 target) and the declaration `void record_flare(rhi::CommandBuffer&, const Scene3D&);`); the handles go into the existing destroy list in `forward3d.cpp` so they are referenced. No later step touches either header.
- **Pure CPU helpers (render, 38b).** New narrow header `mce/render/lens_flare.h` (includes `mce/math` only, forward-declares `struct LensFlare;`), bodies in `src/lens_flare.cpp`; f32, fixed evaluation order, no `rand`, no state:
  - `inline constexpr u32 kLensFlareMaxGhosts = 6;`
  - `struct LensFlareGhost { math::vec2 center; f32 radius; f32 intensity; math::vec3 tint; f32 pad_; };` (center in uv, radius a fraction of the view height).
  - `struct LensFlareFrame { math::vec2 sun_uv; f32 strength; f32 aspect; f32 halo; f32 halo_radius; f32 starburst; u32 rays; f32 occlusion_radius; u32 ghost_count; LensFlareGhost ghosts[kLensFlareMaxGhosts]; };`
  - `bool lens_flare_project(const math::mat4& view_proj, math::vec3 toward_sun, math::vec2& uv)`: projects the direction (w = 0) to uv in the fullscreen-pass convention (row 0 is v = 0, rhi-renderer.md); false when the sun is behind the camera (clip w <= 0) or the input is not finite.
  - `f32 lens_flare_horizon_fade(math::vec3 toward_sun)` = `smoothstep(-0.02, 0.04, y)`: 0 below the horizon, so a night frame never records the pass.
  - `f32 lens_flare_edge_fade(math::vec2 sun_uv, f32 edge_fade)` = `smoothstep(0, edge_fade, min(u, 1-u, v, 1-v))`: 0 on and beyond the border (off-screen the depth cannot be read, so no flare).
  - `u32 lens_flare_ghost_layout(const LensFlare&, math::vec2 sun_uv, f32 aspect, LensFlareGhost* out)`: ghost `k` sits at `0.5 + (sun_uv - 0.5) * kGhostT[k] * ghost_spacing` (a fixed table of six axis positions, most of them on the far side of the centre, e.g. -0.35, -0.7, -1.1, 0.45, -1.5, 1.25), with radii, intensities and tints from fixed tables (tint lerped from white by `ghost_chroma`); returns `min(ghost_count, 6)`. 38b fixes the tables and the tests pin them.
  - `LensFlareFrame lens_flare_frame(const LensFlare&, const math::mat4& view_proj, math::vec3 toward_sun, f32 aspect, bool starburst_allowed)`: everything the pass needs; `strength = intensity * visibility * horizon_fade * edge_fade`, exactly 0 for `enabled` false, `intensity` 0, `visibility` 0, the sun behind the camera, below the horizon, off screen or NaN input (the pass skips on 0). `starburst` is 0 when `starburst_allowed` is false. Because the GPU only receives these computed numbers, there is no CPU/GPU table mirror to keep in sync.
- **Quality tiers (render, 38b).** `QualitySettings::lens_flare` (Low false, Medium and High true) and `QualitySettings::lens_flare_starburst` (Low and Medium false, High true); cvar `r.lens_flare` (bool, Saved, "Sun lens flare (PostFx::flare; off = the frame without it)") next to `r.meteor` in `quality.cpp`. The existing `apply_quality(q, ShadowSettings&, PostFx&)` clamps the `flare` ranges and turns `enabled` off for `!q.lens_flare` or `r.lens_flare 0`; `quality.h` (115 TUs) only gains the two fields. Low is off, so the Low frame never pays for the prepass or the pass; Medium drops only the starburst (the per-pixel angular term).
- **Kit component (kit, 38d).** Opt-in `LensFlare3D` (`mce/kit/lens_flare.h`, one per scene, the first in ECS order is read, registered by `install_kit3d` next to `Environment3D`; a separate component so the wide `kit3d.h`, 243 TUs, is not touched). Fields (all `Save`, with the `Range` above): `enabled` 1, `intensity`, `halo`, `halo_radius`, `ghost_count`, `ghost_intensity`, `ghost_spacing`, `ghost_chroma`, `starburst`, `starburst_rays`, `occlusion_radius`, `edge_fade`, plus `cloud_dim` 1.0 (0..1: how far cloud cover and rain dim the flare) and an explicit zeroed `pad_`. Pure kit helper `lens_flare_visibility(const LensFlare3D&, f32 cloud_coverage, f32 rain_intensity) -> f32` = `clamp(1 - cloud_dim * max(cloud_coverage, rain_intensity), 0, 1)`, inputs clamped, NaN as 0. `Kit3DExtract::apply_environment` (`.cpp` only) copies the fields into `post.flare` before `scene.set_post(post)`, with `visibility` from the helper (the cloud coverage after `apply_quality` and the first `Weather`'s rain, both already computed there); with no `LensFlare3D` or `enabled` 0 the default (disabled) setting is written, so a stale one never survives.
- **Determinism.** The flare is a pure function of the camera, the sun direction, the settings and this frame's depth; it never feeds the simulation, a component or the state hash. A scene without `LensFlare3D` is byte-identical (one empty query in the extract, which is not hashed); a scene with it differs only in the saved component and in frames.

## Pass design (38c)

- **Where it sits.** `record_flare(cmd, scene)` runs in `Forward3D::record` between `record_post_fx` and `record_post`: after TAA, depth of field, motion blur and the auto-exposure histogram (so the flare does not pump the exposure when the sun peeks out), before the bloom chain (so bloom thresholds it, lens dirt lights up where it blooms, and the tonemapper and grade act on it). Its time falls in the existing `bloom` timing bucket (between `kTsPostFx` and `kTsBloom`); no new timestamp, no header change.
- **CPU side.** `lens_flare_frame(scene.post().flare, jitter_ * view.view_proj, -normalize(light.direction), aspect, q.lens_flare_starburst)`; 38c first checks which direction the sky pass draws its disk with (`atmosphere_sun()` when the atmosphere is on, line ~1406 of `forward3d.cpp`, else the light) and uses the same one, so the flare sits on the disk. `strength == 0` returns before recording anything (bit-identical frame, `stats_.lens_flare` false).
- **Depth.** The main depth buffer is a non-sampled (and possibly MSAA) `DepthTarget`, so the pass reads the half-resolution opaque prepass `ao_gbuf_` (`RGBA16F`, `a` = view depth, 0 where nothing was drawn, i.e. sky), calling `record_prepass` (a no-op when SSAO, SSR, contact shadows or the water drew it already this frame). Cost when none of those is on: one half-resolution opaque prepass; recorded in the risks.
- **Occlusion pass.** A fullscreen triangle into the 1×1 `RGBA8_UNorm` target `flare_occ_` (`kFlareOccFS`): 32 taps on a fixed Vogel disk of `occlusion_radius` (scaled by the aspect) around `sun_uv`, `textureLod(..., 0.0)`, each tap counts as visible when `a == 0` (sky); taps outside [0, 1] are skipped and the count renormalised; `r` = visible fraction. One texel, 32 fetches: negligible.
- **Composite pass.** A fullscreen pass (`kFlareFS`) from `post_source_` into `post_fx_target(post_size_)` (the ping-pong pair depth of field and motion blur use), then `post_source_` = that target. It never blends into `post_source_` in place, because with TAA `post_source_` is the history slot the next frame reprojects, and a flare written there would smear into later frames. `out = src + sun_color * strength * occ * (halo + ghosts + starburst)`, with `occ = texelFetch(u_occ, ivec2(0), 0).r`:
  - halo: `halo * exp(-(r / halo_radius)^2)` in aspect-corrected distance `r` from the sun;
  - ghosts: for each of `ghost_count` (a constant-bound loop of 6 with an early `continue`), a soft disc `smoothstep(radius, radius * 0.7, d)` times its intensity and tint, slightly brighter at the rim;
  - starburst (when `starburst > 0`): `pow(abs(cos(rays * 0.5 * atan(dy, dx))), 32) * exp(-r * 6)` times `starburst`.
  A uniform block `FlareU` (bank `kPass`; 3 `vec4` of sun, halo and starburst parameters plus 2 `vec4` per ghost, 15 `vec4`, 240 bytes) is declared in `forward3d.cpp` next to `PostU` and in `shaders3d.h` with `static_assert`s on the size and offsets. No change to `PassU`, `FrameU` or any existing block.
- **Per backend.** The two new fragment shaders are GLSL 450 in `shaders3d.h`, like every pass: the GL backend compiles them as they are; SDL GPU goes GLSL → SPIR-V (glslang) for Vulkan and SPIR-V → MSL (`rhi_sdlgpu/src/shader_msl.cpp`, SPIRV-Cross) on Apple; the Metal backend translates them with `rhi_metal/src/shader_cross.cpp` (`translate_glsl`); WebGPU goes through `rhi_webgpu/src/shader_wgsl.cpp`. To keep them portable they use only what the post passes already use: combined samplers at the existing banks (`kAlbedo` for the source, `kNormal` for `ao_gbuf_`, `kOrm` for `flare_occ_`), one uniform block at `kPass`, no SSBO, no compute, no `textureGather`, explicit `textureLod`/`texelFetch` (no derivatives in non-uniform control flow, which WGSL rejects), constant loop bounds. The uv convention is checked with an asymmetric sun (upper left) on GL and SDL GPU so a flipped v shows up as a test failure; Metal's render-to-texture y flip (`kFlipYConstant`) keeps the same texture layout. `tests/rhi_metal/test_shader_cross.cpp` gets one case that translates both new shaders (it runs without a GPU); WebGPU translation is checked if its test target builds in the container, otherwise recorded as a follow-up.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 38a. Seam: setting, stats and members | `render::LensFlare` and `PostFx::flare` in `scene3d.h`; `Forward3DStats::lens_flare` / `lens_flare_strength`, the private pipeline and texture members and the `record_flare` declaration in `forward3d.h` (referenced in the destroy list); a compile test that the defaults are as listed; the `include_fanout.py` numbers in the PR description. Data only: nothing reads it | render | Sonnet |
| 38b. Pure helpers and quality | `mce/render/lens_flare.h` / `src/lens_flare.cpp` (`lens_flare_project`, `_horizon_fade`, `_edge_fade`, `_ghost_layout`, `lens_flare_frame`), the ghost tables, `QualitySettings::lens_flare` / `lens_flare_starburst`, `r.lens_flare`, the clamps in `apply_quality`; `tests/render/test_lens_flare.cpp` (see GPU verification for the list) | render | Sonnet |
| 38c. Occlusion and composite passes | `kFlareOccFS`, `kFlareFS`, `FlareU` and its `static_assert`s, the two pipelines and the 1×1 target, `record_flare` between `record_post_fx` and `record_post`, the prepass call, stats; `tests/render_gl/test_render3d_flare_gl.cpp` (GL and SDL GPU), goldens `lens_flare_sun` and `lens_flare_occluded` in `linux-gl` and `linux-sdlgpu`, the `test_shader_cross` case | render, tests | **Opus** (independent Opus review) |
| 38d. Kit component and extract feed | `LensFlare3D`, `lens_flare_visibility`, registration in `install_kit3d`, zeroed pad and `test_padding` entry, the three `engine_components.cpp` includes, the `capabilities` row and a `sample_coverage.allow` line until 38e, the MCP transcript golden `components` bump; the feed in `kit3d_extract.cpp`; `tests/kit/test_lens_flare.cpp` and `tests/kit/test_lens_flare_extract.cpp` | kit | Sonnet |
| 38e. Sample | `samples/11_daynight` gets a `LensFlare3D` on its environment entity if its clock passes a low sun with an occluder in view at a capture tick (38e checks), else `samples/kit3d_tour`; the sample's expectations, the allow-line removed | samples | Sonnet |
| 38f. Docs and close | the `08-3d` post-processing paragraph (what the flare is, the occlusion by opaque depth only, the edge, horizon and cloud fades, before bloom, Low off, Medium without starburst, `r.lens_flare`), the render API table row for `PostFx`, milestone status, follow-ups, ROADMAP row, changelog | docs | Sonnet |

38c is the only step that writes a shader or a draw path, so it is Opus-written with an independent Opus review (the author does not review it). The review checklist: the off-check (strength 0 records nothing), the order against the exposure histogram and bloom, no write into the TAA history, the `FlareU` mirror, the uv convention, the prepass reuse, the constant loop bounds and the backend constraints above. Steps stack in order (38a, 38b, 38c, 38d, 38e, 38f); each touches one module. 38d may start right after 38a (it only fills `PostFx::flare`), in parallel with 38b and 38c.

## Gates

- With `flare.enabled` false, the Low tier, `r.lens_flare 0` or no `LensFlare3D`, every existing golden under `tests/render_gl/golden/*` and every sample hash is byte-identical through 38a to 38e (only the 38e sample's expectations change).
- The only wide headers touched are `scene3d.h` and `forward3d.h`, both in the 38a seam PR; `mce/render/lens_flare.h` and `mce/kit/lens_flare.h` are included only by their `.cpp`, `forward3d.cpp`, `quality.cpp`, the extract, the install and the tests.
- Nothing written to a component or the state hash (a kit test hashes the world with and without `LensFlare3D` over a run).
- Slow tests: the GPU tests render one small frame per case (64×64 or 128×128, one scene); if any case exceeds 60 s it ships a smoke variant and the full sweep is labelled with `mce_mark_slow`.
- Generated files (`docs/engine/capabilities.md`, `sample_coverage.allow`, the MCP transcript golden) change only as far as 38d's and 38e's own checks need; staleness after a `master` merge is the integration runner's.

## Risks

- **Prepass cost.** With SSAO, SSR, contact shadows and water all off, the flare alone triggers the half-resolution opaque prepass. The pass only runs while the sun is on screen and above the horizon, and Low is off; 38c records the llvmpipe draw count (relative only) and the real cost goes to the GPU list.
- **Half-resolution depth.** A thin occluder (a pole, a wire) narrower than two pixels may be missed at half resolution; the 32-tap disk over `occlusion_radius` makes partial cover fade smoothly rather than pop. Recorded; a full-resolution depth copy is a follow-up.
- **Off-screen sun.** The flare is 0 once the sun leaves the screen (the edge fade reaches 0 at the border), because the depth there cannot be read; real lenses still flare with the sun just outside the frame. Recorded as a look choice.
- **Look.** The ghost tables, the halo falloff and the default `intensity` are look choices that only a real display can judge; llvmpipe pixels prove placement, occlusion and the off-checks, not taste.

## GPU verification

What the headless tests (Mesa llvmpipe for GL, lavapipe for SDL GPU, in the container) can prove:
- **38b (no GPU):** projection of a known sun direction to a known uv; behind-camera and NaN return false; horizon fade 0 below -0.02 and 1 above 0.04; edge fade 0 on the border, 1 inside the margin; ghost positions on the line through the centre, the count capped at 6, chroma 0 gives white; `strength` exactly 0 for each off case; the clamps and the Low / Medium / High tier values; `r.lens_flare 0` disables.
- **38c (GL and SDL GPU pixel tests):** flare off equals the plain frame exactly; the sun on screen and unoccluded brightens pixels at the sun and at the ghost centres the CPU layout gives, and only there beyond the halo; an opaque box over the sun gives the plain frame (exact, or within 1 LSB, which 38c decides and records); a box over half the sun disk gives an intermediate brightness; the sun behind the camera, below the horizon or at the border gives `stats().lens_flare` false and the plain frame; an asymmetric sun position proves the uv convention; with bloom on, the flare raises the bloom around the sun; TAA on leaves the next frame's history free of the flare (two frames, the second with the sun occluded, equal the occluded plain frame); Medium has no starburst pixels. Goldens `lens_flare_sun` and `lens_flare_occluded` in `linux-gl` and `linux-sdlgpu`.
- **38d (no GPU):** extract tests (no component gives the disabled default; fields copied; cloud cover and rain lower `visibility`; Low tier and `r.lens_flare 0` disable).

What must be looked at on a real GPU or a windowed run (none blocks the milestone; each goes to the GPU list):
- **Golden frames on a hardware GPU.** `lens_flare_sun` and `lens_flare_occluded` regenerated or compared in `linux-gl-nvidia` and `linux-sdlgpu-nvidia` (the vendor directories `golden.h` picks), three runs, no flake.
- **Visible and occluded, live.** A windowed `samples/11_daynight` (or `kit3d_tour`) run: the flare appears as the sun clears a building or a tree line, fades smoothly as it slides behind it (no popping from the half-resolution depth), vanishes at the screen edge and at sunset, and does not shimmer under TAA camera jitter or a slowly turning camera.
- **Per backend.** The same frame on GL and SDL GPU (Vulkan) on Linux NVIDIA; Metal (both the Metal backend and SDL GPU via MSL) on the Mac; WebGPU in a GPU-backed Chromium if available. Check the ghosts lie on the same side of the centre on every backend (the v-flip trap) and the occlusion matches.
- **Look and defaults.** `intensity`, the halo radius, the ghost tables and the starburst against bloom and lens dirt on a real display, with each tonemapper.
- **Cost.** `perf_run` at 1080p on High with the sun on screen, with and without SSAO (the prepass reuse), against the frame without the flare.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 38a | planned | | |
| 38b | planned | | |
| 38c | done (2026-11-04, awaiting the independent Opus review and `--task`) | | See "38c notes" below |
| 38d | planned | | |
| 38e | planned | | |
| 38f | planned | | |

## 38c notes

- **Sun direction (checked).** The sky pass draws its disk with `-p_light_dir`, i.e. `-normalize(DirectionalLight::direction)` (fallback `-y`), with or without the atmosphere (`kSkyFS`, set in `bind_main_state`). `Scene3D::atmosphere_sun()` (line ~1406 of `forward3d.cpp`, `update_atmosphere`) is the quantized direction the baked atmosphere sky and its glow are computed from, and with `Atmosphere::sky_sun_direction` set it is a different direction altogether; the disk never uses it. `record_flare` therefore uses the light's direction, the same expression and fallback as the disk, so the flare sits on the disk.
- **Starburst gate.** `Forward3D` holds no `QualitySettings`; `record_flare` passes `quality_settings(quality_tier()).lens_flare_starburst` (the `r.quality` tier) to `lens_flare_frame`, on top of `apply_quality`, which already zeroes `starburst` for a tier without it.
- **Prepass dependency.** The occlusion reads `ao_gbuf_`, whose pipelines exist only when `Forward3DDesc` (and the `r.*` restart cvars) keeps SSAO, SSR or contact shadows; with all three off at init the flare pipelines are not created and the flare is skipped (no pass, `stats().lens_flare` false). A shader the driver refuses also keeps the frame without the flare instead of failing init. `reserve_instancing` counts the flare as a prepass user so the prepass keeps its instancing budget.
- **Stats.** `lens_flare_strength` is the CPU strength (`intensity * visibility * horizon * edge`); the GPU occlusion share is not read back, so a fully occluded sun still reports the pass as drawn.
- **Off-check and occluded.** strength 0 returns before `record_prepass` and before any pass. An opaque box over the whole occlusion disk gives the frame without the flare **exactly** (occ = 0 adds 0, and the R11G11B10F values copy exactly into the RGBA16F ping-pong target) on llvmpipe and lavapipe; the tests check equality, not 1 LSB.
- **Halo extent.** The Gaussian halo is cut to 0 between 2 and 3 radii (`1 - smoothstep(2r, 3r, d)`) so the frame beyond it is untouched; the starburst has no cut (it only draws when `starburst > 0`).
- **Backends.** GL and SDL GPU (Vulkan) are run by `tests/render_gl/test_render3d_flare_gl.cpp`; the SPIR-V compile and binding table by a `test_shader_spirv` case (Linux, no GPU); Metal's `translate_glsl` by a `test_shader_cross` case (macOS only, not built in the container). WebGPU (`shader_wgsl.cpp`) is not configured in the container: a follow-up.

## Follow-ups

- Flares for bright point and spot lights (a per-light opt-in field and a light list in `FlareU`).
- A moon flare from the `Moon` component at night.
- Temporal smoothing of the occlusion value (a 1×1 ping-pong), and a full-resolution depth for thin occluders.
- A faint flare with the sun just outside the frame.
- Authored ghost textures and anamorphic streaks.
- A Luau setter and the MCP `render_frame` override for one-off shots.
- 38c: check the two flare shaders through `rhi_webgpu` `shader_wgsl.cpp` (Tint) in a build with WebGPU on.
- 38c: the flare without SSAO, SSR and contact shadows at init (create the prepass pipelines for it without changing the water's depth read).
