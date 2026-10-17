# M-VISUAL3 — What keeps the engine from an AAA look with data alone (milestone record)

**Status:** step 1 (the plan below) done (2026-10-16). Plan: ROADMAP §M-VISUAL3 and the plan section of this file; Linear SBA-199 (step 1; the other steps are not filed, see below). Source: GitHub #428.

**Why:** blind test 3 (rounds 5 and 6) had an art agent dress a world with real scanned and low-poly content through
data and MCP only and rate it about 4/10 against an AAA bar. #428 lists what stopped it: no parallax or displacement,
no vignette, grain, chromatic aberration or depth-of-field control, an 8-bit sky with no `.hdr` / `.exr` importer (#409),
no texture compression (M-CONTENT 1), no static merge or instancing for `MeshRef`, a thin `Foliage3D`, and limits on
probes and decals.

**Model policy (coordinator, 2026-10-16):** the render pipeline is Opus work. Step 1, an Opus thread, writes the plan and
the interfaces and opens them as a PR for review; the other steps are Sonnet and follow that note, with an Opus review of
any change to the shaders or the draw path.

| # | Step | Linear | Model | State |
|---|---|---|---|---|
| 1 | Plan and interfaces: #428 checked against the code, fields, keys, arguments, cost | SBA-199 | Opus | done: the plan section below (PR #430) |
| 1b | Seam: `asset/formats.h` (`MaterialExt` height fields, `TextureFormat::RGBA16F`) | not filed | Sonnet | not started |
| 2 | Parallax occlusion mapping from a height map on `.mcmat` | not filed | Sonnet (Opus review) | not started |
| 3 | Filmic post chain: vignette, grain, chromatic aberration, lens dirt, tonemapper; per-shot focus and exposure | not filed | Sonnet (Opus review) | 3a data (fields, `PostFx::lens`, `Camera3DComponent.focus_distance`, kit extract): in review; 3b tonemap shader and goldens, 3c MCP per-shot control and `r.env_override`: not started |
| 4 | `.hdr` / `.exr` sky and image-based light (#409), sky rotation | not filed | Sonnet (Opus glance at the sky shader) | done: see "Step 4 as built" |
| 5a | Automatic instancing of identical `MeshRef` draws | not filed | Opus | not started |
| 5b | `StaticMerge3D`: merged static props per material and cell | not filed | Sonnet (Opus review) | not started |
| 6 | Foliage variation and wind, decal warnings, the demo, gates and the close | not filed | Sonnet (Opus review) | not started |

**Not in scope:** texture compression and shipped size (M-CONTENT 1); more than one `GiVolume3D` or 4 reflection
probes; retargeting, cloth, hair; subsurface scattering; per-light shaft control; a 3D importer beyond glTF. Each one,
with its reason, is in the plan's "#428 against the code" table.

**Linear:** the workspace hit Linear's free issue limit on 2026-10-16 while step 2 was being filed, so only step 1 (SBA-199) exists. The other steps are filed when the limit is lifted or old issues are archived; until then this table is the record.

**Gates:** every new feature off by default, so a scene that does not ask for one draws byte-identical frames and every
existing golden stays unchanged; replay determinism (run-to-run, Debug against Release) holds; each step has a
regression test and an on/off render; `local_ci.sh` green on every step. The hash *values* of worlds that carry a
component which grew new fields (`Environment3D`, `Camera3DComponent`, `Foliage3D`) change with the struct size, as
they did in M-VISUAL2; no test pins them.

---

## Plan (step 1)

### #428 against the code

What #428 says is missing, what the code at `1e9e5a4b` already has, and what this milestone does about it.

| #428 item | In the code today | M-VISUAL3 |
|---|---|---|
| No displacement or parallax | Normal maps only, through a cotangent frame from screen derivatives (`perturb_normal`, `shaders3d.h`); meshes have no tangent stream | Step 2: parallax occlusion mapping from a height map; no tessellation or geometry displacement (no new geometry stage, see step 2) |
| No vignette, grain, chromatic aberration, lens dirt | None of them; the tonemap pass has exposure, bloom, ACES fit, the LUT grade and the color-vision correction | Step 3 |
| No depth-of-field focus or exposure control in `render_frame` | Depth of field exists (`PostFx::DepthOfField`, M-VISUAL 8) and is on `Environment3D` (`depth_of_field`, `dof_focus_distance`, ...), off on Low. What is missing is a per-shot override: `render_frame`'s `camera` cannot set focus or exposure, and a camera entity cannot carry its own focus | Step 3: `render_frame` `camera.focus` / `camera.exposure` and an `environment` override; `Camera3DComponent.focus_distance` |
| No tonemapper choice (world.md item 11) | ACES fit only | Step 3: `Environment3D.tonemapper` (ACES fit, AgX, Khronos PBR Neutral) |
| 8-bit sky, no `.hdr` / `.exr` importer (#409) | The sky and the IBL source are equirect textures sampled as `sampler2DArray` (`u_sky`, format-agnostic); the texture importer reads PNG/JPEG into RGBA8 only (`stb_impl.c` defines `STBI_NO_HDR`) | Step 4: `.hdr` (stb) and `.exr` (tinyexr) cooked to RGBA16F; sky yaw rotation |
| No static merge or instancing for `MeshRef` | Instancing exists for `Foliage3D` / `TerrainFoliage3D` (`InstanceBatch`, GPU culled, M-VISUAL 9); `StaticInstances3D` draws one `MeshInstance` per copy. Every `MeshRef` is its own draw per submesh per pass | Step 5a: identical rigid draws merge into instanced draws in every depth and lit pass, with no data change. Step 5b: `StaticMerge3D` merges different static meshes that share a material |
| `Foliage3D`: one mesh, no wind, no colour variation | Wind exists: `wind_amplitude`, `wind_frequency`, `wind_direction`, applied in the instanced main, shadow and velocity vertex shaders (`wind_offset`). What is missing: the sway is `amplitude × h²` with `h` in metres and no reference height, so a 5 m tree bends 25× its 1 m sway; no gusts (one sine pair, phase from position only); no leaf flutter; no per-copy colour; one mesh per component; no wind for exactly placed props | Step 6: `wind_height`, `wind_gust`, `wind_flutter`, `tint_variation`, up to four meshes per component |
| Heavy scanned plants unusable (24k-triangle grass) | glTF import already makes LOD levels (`lod_levels`, `lod_ratio`, `lod_error` sidecar keys, meshoptimizer) and batches pick a level per copy | Docs only (step 6): the foliage section names the LOD sidecar keys |
| No alpha-to-coverage control (world.md) | Not in `rhi::GraphicsPipelineDesc` | Out: it changes `rhi/types.h` (160 TUs) and four backends; TAA (High tier) already softens masked edges |
| No LOD cross-fade (world.md) | Hysteresis only | Out: a dithered cross-fade needs a per-draw fade factor and TAA to hide the dither; a later item |
| One `GiVolume3D`, 4 reflection probes | `kMaxReflectionProbes = 4` (fixed PassU arrays); one volume texture at sampler bank 5 | Out: the main pass's 16 fragment sampler slots are all taken (see "Slot budget"); more volumes need an atlas and a PassU redesign, a milestone of its own |
| No retargeting, IK, cloth, hair | IK exists (M-GAMEPLAY3D 5: two-bone, look-at and foot IK in `.mcanim` v2); retargeting, cloth and hair do not | Out (animation work) |
| No per-light shafts, no SSS, emissive control only through bloom | Volumetric fog already scatters the sun (through the cascades) and the point and spot lights (with their local shadows), so shafts through windows exist with `volumetric_fog`; there is no per-light scale. `MeshRef.emissive_tint > 1` and `bloom_threshold` drive glow | Out: a per-light `volumetric_intensity` and SSS are follow-ups |
| `Decal3D`: 3 textures and 64 decals per frame, no warning | `kMaxDecalTextures = 3` (samplers 0..2), `kMaxDecals = 64`; `Scene3D::Stats::decals_dropped` counts the drops silently | Step 6: a warning naming the limit and `decals_dropped` in `perf_run`. Raising the texture limit needs a decal texture array; out |
| `asset_import` 2D only | True | Out |
| Texture compression, max size, shipped size | RGBA8 or ASTC 4×4 only | M-CONTENT 1. Step 4's HDR importer gets its own `max_size` because a float sky is 4× an RGBA8 one |

### Slot and header budget (read before any step)

- **Fragment samplers of the main pass are full.** `kMeshFS` binds 0..2 decal textures, 3 SSR, 4 volumetrics, 5 GI
  volume, 6 albedo, 7 normal, 8 ORM, 9 shadow cascades, 10 sky, 11 SSAO, 12 pre-filtered environment, 13 emissive,
  14 BRDF LUT, 15 SH9. Sixteen is the floor WebGPU and Metal guarantee, so no step adds a sampler to `kMeshFS`. Step 2
  puts the height map in layer 1 of the normal map's array (below). The tonemap pass has free slots (it uses 6, 7, 8).
- **No new RHI bank constant.** `rhi/types.h` reaches 160 TUs. Step 5a reuses `bank::kInstances` (13) for its
  per-draw storage block in the vertex stage, where only the batch shaders use it today.
- **`asset/formats.h` reaches 302 TUs**, so its two changes (step 2's `MaterialExt` fields, step 4's
  `TextureFormat::RGBA16F`) go together in seam PR 1b, merged before steps 2 and 4. `MaterialExt` keeps its 64 bytes
  and `MaterialChunk` its 112; no chunk version changes except the material importer's own version (re-cook).
- **`kit/kit3d.h` reaches 111 TUs** (below the 150 warning, no seam needed) but every kit step edits it, so the waves
  below give it one owner at a time. Step 5b's component goes in a new header so it never collides.
- `render/scene3d.h` (93 TUs), `render/forward3d.h` (80) and `render/quality.h` change in steps 3, 4 and 5a; each is
  under the warning. `shaders3d.h` reaches 2 TUs; conflicts there are textual, and the waves keep the steps in
  different blocks of it.

### Step 1b — seam: `asset/formats.h`

```cpp
// formats.h, TextureFormat
enum class TextureFormat : u32 {
    RGBA8 = 1,
    ASTC_4x4 = 2,
    RGBA16F = 3,  // M-VISUAL3 4: half-float RGBA, linear (HDR skies and environment maps); never Srgb
};
// texture_level_bytes(RGBA16F, w, h) = w * h * 8

// formats.h, MaterialExt: the six reserved words become the height map (M-VISUAL3 2). Size stays 64.
struct MaterialExt {
    ...                         // clearcoat ... ior, unchanged
    u32 height_texture[4];      // GUID {hi, lo} of a linear Texture, as two u64 split in u32 halves (keeps
                                // the struct's 4-byte alignment); all zero = no height map
    f32 height_scale;           // depth of the deepest point, in texture repeats (0.02..0.1 typical); 0 = off
    u32 parallax_steps;         // low 16 bits: minimum march steps, high 16: maximum; 0 = the defaults (8, 32)

    Guid height() const;        // the GUID from height_texture
    bool parallax() const { return height_scale > 0.0f && !(height() == Guid{}); }
    bool any() const;           // unchanged: the lobes only (parallax() is separate)
};
```

The `.cpp` definitions (`texture_level_bytes`, `MaterialExt::height`) go in `asset/src`. Tests: `static_assert`s on
both sizes, `texture_level_bytes` for RGBA16F, a `MaterialExt` round trip through `of_material`. Nothing reads the new
fields yet. Reviewer: anyone (mechanical).

### Step 2 — parallax occlusion mapping

**Data.** `.mcmat` gains the texture slot `"height"` (inside `"textures"`, like `"normal"`) and two keys:

```json
"height_scale": 0.04,          // texture repeats; 0 or absent = no parallax
"parallax_steps": [8, 32],     // min (looking straight on) and max (grazing) march steps, 4..64
"textures": { "normal": "guid:...", "height": "guid:..." }
```

White is high (Poly Haven's `disp`). The material importer (`asset/cook/src/material_importer.cpp`) stores the GUID and
the keys in `MaterialExt` and writes the extension chunk when either the lobes or the height are set; importer
version 1 → 2. The height texture is a dependency like the other slots (`textures.height`); a height texture imported
with `"srgb": true` is a cook warning naming the file. The MCP `Material` schema (`src/editor/mcp/src/asset_tools.cpp`)
lists the two keys and the slot. Only `.mcmat` materials carry a height map (glTF has no height slot), so parallax
reaches `MeshRef.material`, `Foliage3D.material` and `TerrainFoliage3D.material`, not a mesh's own submeshes, and not
`Terrain3D` (its layers already blend by height).

**Binding: no new sampler.** The normal map slot (bank 7) is already a `sampler2DArray` sampled at layer 0.
`Kit3DExtract::material()` (`kit/src/kit3d_extract.cpp`) composes a 2-layer array — layer 0 the normal map, layer 1 the
height map — with `render::compose_layer_array` (the terrain's helper; CPU, before upload), uploads it under a new
texture key and puts that key in `MaterialOverride::normal_key`. Without a normal map, layer 0 stays zero-filled and
`o_maps.x` stays 0 (the shader then reads layer 1 only). The two textures must match in size, mip count and format
(compose_layer_array's rule); on a mismatch it logs one warning naming both GUIDs and the material draws without
parallax. M-CONTENT 1 must keep this working: when it compresses normal maps (BC5, two channels) it must cook a
height map referenced by a material in the same format, height in R.

```cpp
// render/scene3d.h — MaterialOverride gains (scene3d.h is 93 TUs; no seam)
struct MaterialOverride {
    ...
    bool parallax = false;     // normal_key is the 2-layer [normal, height] array and ext holds the scale/steps
};
```

**Shader** (`render/src/shaders3d.h`, `render/src/forward3d.cpp`, `forward3d_scale.cpp`). `ObjectU` gains one `vec4`
at its end (the C++ mirror in `forward3d.cpp` and its users, the instanced path in `forward3d_scale.cpp` included):

```glsl
vec4 o_pom;  // x: height scale (texture repeats; 0 = off), y: min steps, z: max steps, w: fade-out view depth (m)
```

`o_pom.w` comes from the quality tier (`QualitySettings::parallax_distance`, below); `o_pom.x` is 0 when the tier
disables parallax. In `kMeshFS` (not `MCE_TERRAIN`), before the albedo fetch:

```glsl
vec2 uv = v_uv;
if (o_pom.x > 0.0) uv = parallax_uv(v_world, n, v_uv, depth);  // uniform branch
// every later material fetch (albedo, alpha mask, ORM, normal, emissive) reads `uv`; the cotangent frame in
// perturb_normal keeps taking its derivatives from v_uv and v_world.
```

`parallax_uv` builds T and B from the same screen derivatives as `perturb_normal` (normalized, B along −v as there),
takes the view vector into that frame, scales the height by `1 − smoothstep(0.75·w, w, depth)`, and marches
`mix(max, min, |V·N|)` layers with `textureGrad(u_normal_map, vec3(p, 1.0), dFdx(v_uv), dFdy(v_uv)).r` (explicit
gradients: the loop is non-uniform), then one linear refinement between the last two layers. No self-shadowing, no
depth offset (early-z, the SSAO prepass and the shadow maps keep the flat surface), no silhouette clipping. The SSAO
prepass and the velocity pass alpha-test at the un-offset uv; a masked parallax material can differ from the main
pass by a texel at its mask edge (documented limit).

**Quality tiers** (`render/quality.h` / `quality.cpp`):

```cpp
f32 parallax_distance = 30.0f;  // QualitySettings: view depth (m) where parallax has faded out; 0 = off
u32 parallax_max_steps = 32;    // a material's max steps are clamped to this
//   Low: 0 (off)   Medium: 15 m, 16 steps   High: 30 m, 32 steps
```

**GL vs SDL GPU.** Same GLSL through glslang to SPIR-V; `textureGrad` on `sampler2DArray` and a dynamic loop count are
core on both and on Metal/WebGPU (WGSL's `textureSampleGrad` is allowed in non-uniform control flow). 2-layer RGBA8
array uploads are the terrain's path on every backend. No backend code changes.

**Budget.** CPU: one compose per material on first use (a 2k map: ~5 ms memcpy, cached). GPU memory: the composed
array duplicates the normal map's memory (the plain normal texture stays resident for other users). Per pixel of a
parallax material: 8–32 extra taps, typically ~1.5–3× the material's base fragment cost; nothing for other pixels. The
real-GPU number joins the Mac/device session, as for M-VISUAL2 (llvmpipe timings are not meaningful).

**Determinism and goldens.** Render only; no sim state. Materials without a height map write `o_pom = 0` and take the
untouched branch, so every existing golden is byte-identical (gate). New golden `parallax_cobble` on `linux-gl` and
`linux-sdlgpu`: a grazing view of a tiled floor with a procedurally generated height/normal pair (no binary content
in the repo beyond the golden PNG).

**Acceptance.** Cook: the keys round-trip, a missing or non-texture height GUID is a `file:line:col` error, an sRGB
height warns. Kit (`tests/kit/test_kit3d.cpp`): a `.mcmat` with a height map yields `MaterialOverride::parallax` and a
2-layer key, and a size mismatch disables it with one warning. Render (`tests/render_gl/test_render3d_gl.cpp`, GL and
`MCE_RHI=sdlgpu`): with parallax the floor's pixels shift toward the camera at grazing angles and not at normal
incidence; beyond `parallax_distance` the frame equals the no-height frame; Low tier equals no-height. The golden. Docs:
`docs/user-guide/05-assets.md` (keys), `08-3d.md` (when to use it, the tier table). Changelog fragment.

### Step 3 — filmic post chain, per-shot focus and exposure

**Data.** `Environment3D` (kit3d.h) appends, before its `pad_`, keeping the layout padding-free:

```cpp
f32 vignette = 0.0f;             // 0..1: darkening at the frame's corners; 0 = off
f32 vignette_smoothness = 0.5f;  // 0.05..1: how far in from the corners it reaches
f32 film_grain = 0.0f;           // 0..1: luminance-weighted animated grain; 0 = off
f32 grain_size = 1.5f;           // 1..4: grain cell size in output pixels
f32 chromatic_aberration = 0.0f; // 0..1: radial colour fringe toward the edges (1 = 0.5 % of the view width); 0 = off
f32 lens_dirt_intensity = 0.0f;  // 0..4: how strongly bloom lights the dirt texture
u32 tonemapper = 0;              // 0 ACES fit (today's), 1 AgX (base), 2 Khronos PBR Neutral
asset::AssetRef<asset::Texture> lens_dirt;  // rgb dirt mask over the frame (linear); empty = none
```

(The asset ref goes with the other 8-byte fields at the top's alignment rules: the implementer re-checks
`sizeof(Environment3D) % 8 == 0` and that no implicit padding appears.) `Camera3DComponent` gains
`f32 focus_distance = 0.0f; // m; 0 = Environment3D's dof_focus_distance` in place of its `pad_` (size unchanged).

```cpp
// render/scene3d.h, PostFx
struct Lens {
    f32 vignette = 0.0f, vignette_smoothness = 0.5f;
    f32 grain = 0.0f, grain_size = 1.5f;
    f32 chromatic_aberration = 0.0f;
    u32 dirt_key = 0;              // TextureStore key; 0 = none
    f32 dirt_intensity = 0.0f;
} lens;
enum class Tonemapper : u8 { AcesFit, AgX, PbrNeutral };
Tonemapper tonemapper = Tonemapper::AcesFit;
```

**Shader.** Everything lands in the existing tonemap pass (`kTonemapFS`, `record_post`): no new pass, no new target.
The pass binds a new `FilmU` block at bank 1 (free in this pass) with the lens values, the output size and a grain
seed; the dirt texture at sampler 9 (free in this pass). Order, chosen so each effect sits where a camera puts it:

1. Chromatic aberration on the scene-linear input: the HDR and bloom inputs sampled three times with a radial offset
   per channel (only when `chromatic_aberration > 0`; the plain path keeps one fetch).
2. Bloom composite; lens dirt adds `bloom × dirt × dirt_intensity` (bloom must be on).
3. Vignette in linear light (a multiply before the tonemap, `smoothstep` over the corner distance).
4. The tonemapper switch (`AcesFit` = the current `aces(c * 0.6)` line, untouched).
5. Grade, colour-vision correction as today.
6. Grain in display space, after the grade: a hash of (pixel / grain_size, seed), weighted toward the mid tones;
   added to rgb only. The alpha luma FXAA reads is computed before the grain, so FXAA does not chase it; the sharpen
   pass (when on) does sharpen it, which is documented.

The seed is `floor(Scene3D::time() × 24)`: animated, deterministic per frame index in tests.

**Per-shot control over MCP** (`src/editor/mcp/src/tools_render.cpp`, `render_frame` and `render_sheet`):

- `camera.focus`: metres, or `"target"` = the distance from `camera.position` to `camera.target`; turns depth of field
  on for the shot. `camera.exposure`: a multiplier. Both ride the run as player convars.
- `environment`: an object of `Environment3D` field → value overrides for the shot (numbers, booleans, vectors as
  arrays; asset refs refused), validated against the component's reflected fields so a typo names the valid ones.
- Player side: the convar `r.env_override` (string `field=value;...`) is parsed once per change by `Kit3DExtract`
  through the reflection field registry and applied over the resource after it is read each frame; `r.dof_focus` and
  `r.exposure_scale` carry the camera shorthands. They are console tools too, not saved.

`Camera3DComponent.focus_distance` > 0 wins over the environment's focus for that camera (a timeline can animate it:
focus pulls).

**Quality.** All tiers keep vignette, grain, CA, dirt and the tonemapper (one pass, a few ALU per pixel; CA adds two
fetches per input). Depth of field stays off on Low as today.

**GL vs SDL GPU.** Shader-only; a uniform block at bank 1 and a sampler at 9 are ordinary slots both translators already
map for other passes. The tonemap writes the final target or `fin_src_`, so the M-VIEWS sub-rect path is unchanged.

**Budget.** < 0.1 ms at 1080p on a mid GPU for vignette + grain + tonemapper; CA ≈ the cost of two more full-screen
fetches of HDR and bloom. No memory beyond the optional dirt texture.

**Determinism and goldens.** Render only. Defaults are all off and `tonemapper` 0 runs the existing line: byte-identical
goldens (gate). New goldens `filmic_lens` (vignette + CA + grain at a fixed time) and `tonemap_agx` on both backends.

**Acceptance.** Kit test: the fields reach `PostFx::lens`, `Camera3DComponent.focus_distance` overrides the focus,
`r.env_override` overrides a float and a vec3 and rejects an unknown field with one warning. Render tests: each effect
off is byte-identical to today; vignette darkens the corners and not the centre; CA separates channels at the edges
and not at the centre; grain changes between two seeds and its mean stays within 1 level; AgX and Neutral differ from
ACES on a bright gradient and agree near mid grey. MCP: `render_frame` with `camera.focus: "target"` blurs the
background of a two-plane scene (test in `tests/mcp`); the transcript golden is the integration runner's to regenerate.
Docs: `08-3d.md` post section, the MCP tool docs. Changelog fragment.

### Step 4 — HDR sky and image-based light (#409)

**Cook** (`asset/cook/src/texture_importer.cpp`, `stb_impl.c`, `cmake/Dependencies.cmake`):

- `.hdr` (Radiance RGBE): enable stb's HDR path (drop `STBI_NO_HDR`; keep `STBI_NO_LINEAR`, so PNG/JPEG still never
  go through floats) and read with `stbi_loadf_from_memory`.
- `.exr`: tinyexr (BSD-3, FetchContent per ADR-0017, with its bundled miniz), uncompressed / ZIP / PIZ, the first RGB(A)
  layer. If the dependency review says no, `.exr` is dropped from the step and `.hdr` alone closes #409 (Poly Haven and
  most HDRI sources ship `.hdr`).
- Cooked as `TextureFormat::RGBA16F` (from the seam), linear, never `Srgb`; values clamped to the half range
  (65504), NaN/Inf to 0; mips box-filtered in float. Sidecar settings for these extensions: `"mips"` (default true),
  `"max_size"` (largest side, default 2048, a power-of-two downscale before mips: a float sky is 8 bytes a texel),
  `"exposure"` (EV applied at cook, default 0). `"srgb": true` on an HDR file is a cook error.
- The phone cook (`--textures astc`) keeps RGBA16F for these (ASTC HDR is not universally supported); M-CONTENT owns any
  later compression (BC6H).

**Runtime.** `render::upload_cooked_texture` (`render/src/asset_upload.cpp`) maps `asset::TextureFormat::RGBA16F` to
`rhi::TextureFormat::RGBA16F` (an array texture like every cooked texture). The sky, the IBL bake (sharp layer,
GGX pre-filter with filtered importance sampling, SH9) and reflection probes already read `u_sky` through
`sky_radiance()` without assuming a range, so `Environment3D.sky_texture` and `ibl_texture` take an HDR asset as is.
`sky_texture_intensity` stays the linear exposure of the texture.

**Sky rotation** (the art agent hand-aligned the sun with an HDRI's moon): `Environment3D` gains
`f32 sky_rotation_deg = 0.0f; // yaw of the sky texture about +Y`, `render::Sky` gains `f32 rotation = 0.0f; // radians`,
and `PassU` gains `vec4 p_sky_rot; // x cos, y sin of the sky texture's yaw`. `sky_radiance()` rotates its direction
before `equirect_uv` (one place: the main pass, the sky pass, the IBL bake, the probe captures and the GI captures all go
through it). The gradient and the atmosphere ignore it.

**GL vs SDL GPU.** RGBA16F upload: GL `GL_RGBA16F / GL_HALF_FLOAT` and SDL GPU `R16G16B16A16_FLOAT` are already mapped
(the IBL targets use them); sampled with linear filtering and mips on every backend (WebGPU `rgba16float` is
filterable; GLES 3 filters half floats). The upload path's per-format byte size comes from `rhi::bytes_per_pixel`
(8 for RGBA16F); the implementer checks each backend's texture upload honours it (the SDL GPU and WebGPU readback paths
already special-case RGBA16F).

**Budget.** A 2048×1024 HDR sky with mips ≈ 22 MB of GPU memory (4× an RGBA8 sky); cook time ~0.3 s for a 2k `.hdr`.
No per-frame cost change (same fetches, wider format).

**Risk.** A sun several thousand times brighter than the sky can leave fireflies in the rough IBL layers or ring in
SH9. The bake already samples lower mips by pdf; if the acceptance test still shows sparkles, add
`Sky::ibl_clamp` (max radiance fed to the bake, default off) rather than clamping at cook.

**Determinism and goldens.** Render only. A scene without an HDR texture and with `sky_rotation_deg = 0` draws
byte-identical frames (`p_sky_rot = (1, 0)` takes a no-rotation branch, not a matrix multiply, so the bits match).
New golden `hdr_sky` on both backends from a generated 64×32 `.hdr` (written by the test) with a 200× hot spot.

**Acceptance.** Cook tests: `.hdr` and `.exr` fixtures (generated in the test) keep values above 1 and the hot spot's
ratio within half-float precision; `max_size` halves until it fits; `srgb: true` is refused. Render tests: the HDR sky's
IBL lights a white sphere brighter on the hot spot's side than an RGBA8 copy of the same map; rotation by 90° moves the
hot spot's reflection by a quarter turn; rotation 0 is byte-identical to today. The golden. Docs: `05-assets.md`
(texture formats, `max_size`, `exposure`), `08-3d.md` (sky). Changelog fragment.

**Step 4 as built.**

- **Cook.** `texture_importer.cpp` handles `.hdr` (stb, `stbi_loadf_from_memory`) and `.exr` (tinyexr v1.0.12 pinned by commit
  `735ff73c`, with its bundled miniz, through `FetchContent` in `cmake/Dependencies.cmake`; its own `mce_tinyexr` library
  and a "tinyexr wall" in `check_walls.py`; BSD-3-Clause and MIT, recorded in `ARCHITECTURE.md` §7). Output is `RGBA16F`,
  linear, `LinearFilter` flag, never `Srgb`: exposure (2^EV) first, then negatives and NaN/Inf to 0 and a clamp to 65504,
  then the power-of-two `max_size` halving and the mip chain, both 2x2 box-filtered in float, then half conversion per level.
  The importer's version is unchanged: PNG/JPEG output is byte-identical and no existing asset re-cooks.
  `"srgb": true` is a cook error that names the fix; `max_size` / `exposure` are keys of HDR files only (a PNG that names
  them is an unknown-key error). Deviation from the plan: `STBI_NO_LINEAR` could not stay, because stb compiles
  `stbi_loadf_from_memory` out with it; the build now defines `STBI_ONLY_HDR` beside `STBI_ONLY_PNG` / `STBI_ONLY_JPEG` and
  PNG/JPEG still go through `stbi_load_from_memory` only. `TextureView::read` (the seam did not touch it) accepts
  `RGBA16F` and refuses it with the `Srgb` flag. `project_check`'s sRGB audit skips `.hdr` / `.exr`, and its credits check lists `.exr` as media beside `.hdr`.
- **Runtime.** `upload_cooked_texture` (and the terrain's layer array) map `RGBA16F` to `rhi::RGBA16F`. Every backend's
  texture upload sizes levels with `rhi::bytes_per_pixel` (8 for `RGBA16F`; checked on GL, SDL GPU, WebGPU and Metal), so
  no backend changed.
- **Sky rotation.** `Environment3D.sky_rotation_deg` (took the place of the trailing `pad_`, so the struct stays 440 bytes
  and a zero reads as before), `render::Sky::rotation` (radians), `PassU::p_sky_rot` (cos, sin; last member). `sky_radiance()`
  rotates its direction by `-rotation` once, before `equirect_uv`, only when `p_sky_rot.y != 0`, so the main pass, the sky
  pass, the IBL bake, the probe captures and the GI captures turn together; the baked `u_env` layers are already in world
  space and are read unrotated. The atmosphere's baked sky forces `(1, 0)`. The IBL cache key includes the rotation.
  Positive degrees turn the sky counter-clockwise seen from above (+X toward -Z); `ibl_texture` turns with the same angle.
- **Tests.** `tests/asset/test_cook_hdr.cpp` (generated `.hdr` and `.exr`, `hdr_fixtures.h`): values above 1 and the 200x
  ratio, NaN/Inf/negatives/overflow, `max_size`, `exposure`, float mip averages, refusals, determinism, the phone cook.
  `tests/render_gl/test_render3d_gl.cpp` (both backends): the HDR sky lights a white sphere brighter than an RGBA8 copy on the
  hot spot's side, 90-degree steps move the light a quarter turn (sphere and dome), no rotation is deterministic and the
  gradient ignores rotation, and a one-texel sun 80,000 times the sky leaves no sparkles. Golden `hdr_sky` on `linux-gl`
  and `linux-sdlgpu`, from a generated 64x32 `.hdr` cooked by the importer. Frames with no HDR texture and rotation 0 were
  compared byte for byte against the pre-change shader (8 golden scenes, both backends): identical.
- **Firefly check (the Risk paragraph).** A single-texel sun 80,000 times the sky (20000 against 0.25) on rough
  spheres (roughness 0.9 and 0.6) shows no sparkles on either backend (largest step between neighbouring pixels under
  24 levels, the shading gradient), so `Sky::ibl_clamp` was not added.
- **Hardening (4b).** The HDR decode reads the header first (`stbi_info_from_memory`, tinyexr's header parse) and refuses a source over 64 MP (8192 x 8192, each side 1..16384, 64-bit math) or empty before decoding; the decoder's buffer is processed in place (no second full-size copy) and freed on every path (tinyexr is driven through its header-based API, so the header is freed even when the pixel load fails), and a truncated `.hdr` is refused (stb would otherwise pad it silently).

### Step 5a — automatic instancing of identical draws

**Why Opus.** It rewrites how every depth and lit pass issues draws (`draw_meshes`, `shadow_draw`, the local shadow
tiles, the velocity and SSAO prepasses), must stay byte-identical with existing goldens, and budgets per-frame transient
storage in the upload ring. That is the draw path and GPU data flow the model policy reserves for Opus; a Sonnet
implementation would need the review to redo the design anyway.

**Behaviour.** No data change: `MeshRef`s (and `StaticInstances3D` copies) that draw the same rigid submesh with the
same material and tint collapse into one instanced draw per pass. On a Lantern Keep–like scene (crates, barrels,
modular walls) this divides the draws of the repeated props by their copy count in the main pass, the prepasses and
every cascade.

**Grouping** (`render/src/scene3d.cpp`, CPU, testable without a GPU). After the opaque sort, `Scene3D::build()` forms
*runs*: within each material segment of the sorted opaque list (the sort already groups by albedo key), draws are
bucketed by an instancing key and each bucket is placed at the position of its nearest member, so the list stays
roughly front to back. Two draws share a key when all of these match:

- same `GpuMesh*`, submesh, LOD level (so the same index range);
- rigid (`palette == ~0u`), no `terrain`, not drawn after opaques (no blend, no transmission);
- same `MeshInstance::material` pointer (or both null), bitwise-equal `tint` and `emissive_tint`;
- the draw does not move this frame (`prev_model == model`, bitwise) when the velocity prepass runs.

```cpp
// render/scene3d.h
struct DrawRun {           // draws()[first, first + count) draw as one instanced draw
    u32 first = 0;
    u32 count = 0;
};
struct InstancingSettings {
    bool enabled = true;   // r.instancing (default decided by the gate below)
    u32 min_count = 2;     // runs shorter than this stay single draws
};
void set_instancing(const InstancingSettings& s);
Span<const DrawRun> opaque_runs() const;          // covers [0, opaque_count()) exactly, in draw order
// For a caster list (a cascade's, a local tile's): the same keying on (mesh, submesh, lod, alpha-mask material)
void caster_runs(Span<const u32> casters, Array<u32>& sorted, Array<DrawRun>& runs) const;
// Stats: runs_instanced, draws_instanced (draws folded into runs, main view)
```

Shadow passes key on less (no tint; the material only when it alpha-masks), so their runs are longer than the main
pass's.

**GPU data** (`render/src/forward3d.cpp`, `shaders3d.h`). For a run, the per-instance data goes up with
`cmd.set_storage_data(rhi::bank::kInstances, ...)` (transient, the joint palette's mechanism) and one
`draw_indexed(..., instance_count = run.count)` replaces the run's draws:

```glsl
// kDrawInstancedVS (main, SSAO prepass): kMeshVS's math with o_model / o_normal read per instance, so a run's pixels
// equal the single draws' bit for bit.
struct DrawInst { mat4 model; mat4 normal; };
layout(std430, binding = 13) readonly buffer DrawInstances { DrawInst d_inst[]; };
// kShadowInstancedVS: per instance the premultiplied cascade/tile MVP the CPU computes today (s_mvp), not
// view_proj × model on the GPU, for the same reason.
layout(std430, binding = 13) readonly buffer ShadowInstances { mat4 d_mvp[]; };
// kVelocityInstancedVS: model only (runs never contain moving draws, so previous = current model).
```

Pipelines: instanced twins of `mesh_cull_`, `mesh_two_sided_`, `ao_prepass_`, `shadow_pipe_` and `velocity_pipe_`
(5 pipelines, created only when `Forward3DDesc::instancing` is true; `-r.instancing 0` creates none). `kMeshFS` is
unchanged; `ObjectU` per run carries everything except model and normal.

**Upload budget.** 128 bytes per instanced main/prepass draw, 64 per instanced shadow caster per cascade or tile, on
the frame's upload ring (`r.uniform_ring_mb`, 1 MiB default). Forward3D keeps a per-frame budget of half the ring for
these blocks; past it, the remaining runs draw singly and `Forward3DStats::instancing_overflow` counts them. 3,000
instanced draws in the main pass plus 3 cascades of the same is ~1 MB, so the implementer measures the ring on the
showcase and raises the default only with the coordinator's agreement.

**Not instanced** (draw singly, as today): skinned and posed meshes, blended and transmissive materials, terrain nodes,
moving draws while the velocity prepass runs, probe and GI captures (rare, budgeted separately; a follow-up if a
capture shows up in a profile). Batches (`InstanceBatch`) keep their own GPU-culled path.

**GL vs SDL GPU.** Vertex-stage storage buffers at binding 13 are already used by the batch shaders on GL, SDL GPU,
Metal and WebGPU; instanced indexed draws are core. Neither backend changes. GLES devices without vertex-stage storage
buffers already lack the batch and skinned paths; `Forward3DDesc::instancing` follows the same capability check.

**Budget.** CPU: one hash-bucket pass over the opaque list and each caster list per frame (< 0.2 ms for 5k draws, to be
measured in `perf_run`); GPU: fewer state changes, the same vertices.

**Determinism and goldens.** Render only. The gate for the default: every existing golden byte-identical with
`r.instancing 1` on `linux-gl` and `linux-sdlgpu`. If one differs (an exact depth tie between coplanar copies resolved
in a new order is the only expected cause), the step ships with the default off and reports which golden and why; the
coordinator decides. New golden `instanced_props` (a grid of identical crates under three cascades) plus a pixel test
that the frame with instancing on equals the frame with it off.

**Acceptance.** Scene tests: runs cover the opaque list exactly; a tinted, moving, blended or skinned copy breaks a run;
caster runs per cascade. Render tests: equal frames on/off; `Forward3DStats::main_draws` and the command buffer's
draw count drop by the copy count; overflow falls back to single draws with equal pixels. `perf_run` prints
`runs_instanced` and `draws_instanced`. Docs: `08-3d.md` and `docs/perf` (what a draw is now). Changelog fragment.
M-CONTENT step 2 (draw-call report by cause) builds on these counters.

### Step 5b — `StaticMerge3D`

Step 5a folds copies of the *same* mesh. Modular kits (Kenney's pieces share one colormap) are many *different* small
meshes with one material; 5b merges those.

```cpp
// kit/static_merge3d.h (new, own header so it never collides with kit3d.h edits)
// Merges the rigid MeshRef children (and their children) of this entity into one mesh per material per cell of
// `cell_size` metres at runtime, drawn in place of the children. A child qualifies when it is visible, rigid (no skin
// or posed nodes), has no Animator, interpolate 0, and an opaque or masked material; others draw as before.
struct StaticMerge3D {
    f32 cell_size = 32.0f;     // metres; bounds vertex precision (snorm16 over a cell: 0.5 mm at 32 m) and culling
    u32 cast_shadows = 1;      // 0: the merged meshes cast none (children's own flags are ANDed otherwise)
    u32 enabled = 1;
    u32 version = 0;           // bump after moving children at runtime (edits in the editor re-merge by themselves)
};
MCE_COMPONENT(StaticMerge3D, (cell_size, Range{4, 512}), cast_shadows, enabled, (version, Help{"bump to re-merge"}))
```

**Build** (`kit/src/static_merge3d.cpp`, `render/src/static_merge.cpp`, `render/include/mce/render/static_merge.h`).
`render::build_merged_mesh(Span<const MergeSource>, Array<byte>& blob)` writes a CHNK_MESH payload (the
`TerrainLod::build_mesh` precedent) from each source's dequantized vertices under its world matrix, re-quantized to the
cell's bounds, one submesh per merge key (the material identity: albedo, normal, ORM and emissive keys, the
`MaterialChunk` and `MaterialExt` bytes, tint, emissive tint, double-sided). LOD level *k* of a merged submesh is the
union of each child's level `min(k, levels − 1)`, so far cells stay cheap. `Kit3DExtract` uploads it under
`MeshStore::allocate_key()`, draws one `MeshInstance` per cell (identity world, the cell's sphere culled), skips the
merged children in its `MeshRef` loop, and re-merges when the children's (entity, mesh, material, transform) hash or
`version` changes; old keys are released through the store's retire path. The editor viewport turns merging off
(`Kit3DExtract::set_static_merge(false)`) so selection, gizmos and picking keep per-entity draws.

**GL vs SDL GPU.** None: merged meshes are ordinary meshes.

**Budget.** CPU merge on first draw and on change: ~10 ms per 100k merged vertices (measured in the test); memory: the
merged vertices duplicate the sources' (the sources stay resident for other users). Draws: one per (material, cell)
per pass.

**Determinism and goldens.** Render only (the component's bytes are hashed like any component). Requantization moves
vertices by at most half a quantum, so merged frames are close, not equal, to unmerged ones: the gate is a pixel test
with a small tolerance, and a new golden `static_merge_kit` of the merged frame.

**Acceptance.** Kit tests: qualifying children merge, an animated or blended child stays separate, moving a child
re-merges, removing the component restores the children's draws. Render: merged vs unmerged frame within tolerance;
draws per pass equal (materials × cells). `perf_run`: `merged_groups`, `merged_children`. Docs and MCP: the component
is reflected; `08-3d.md` explains when to use 5b against 5a and `Foliage3D`. Changelog fragment.

### Step 6 — foliage, decals, the demo and the close

**Step 6 status:** 6a data seam merged (#488); 6b renderer (variation, wind, multiple kinds) in the PR that adds this line; open: decal-drop warning, `decals_dropped` perf counter, showcase demo and the close (6c-6e). Linear: none filed (free issue limit).

**Foliage3D and TerrainFoliage3D** (kit3d.h; same fields on both where marked ✱):

```cpp
math::vec4 tint_variation{1, 1, 1, 1};  // ✱ each copy's tint = mix(tint, tint_variation, hash(copy origin))
// As built: tint × mix(white, tint_variation, hash(copy origin)), see "Known limits" under the closing record
f32 wind_height = 0.0f;   // ✱ metres where the sway reaches wind_amplitude; 0 = today's amplitude × h² (h in m)
f32 wind_gust = 0.0f;     // ✱ 0..1: slow gusts travelling along wind_direction across the field
f32 wind_flutter = 0.0f;  // ✱ metres of fast per-vertex leaf jitter at wind_height
asset::AssetRef<asset::Mesh> mesh_1, mesh_2, mesh_3;  // Foliage3D only: more kinds in one scatter
math::vec4 mesh_weights{1, 0, 0, 0};                  // Foliage3D only: relative share of mesh, mesh_1..3
```

`render::InstanceBatch` gains `tint_variation` and a `vec4 wind_ext` (height, gust, flutter, 0). `wind_offset()`
(`MCE_GLSL_INSTANCES`) becomes: profile `(h / H)²` when `H > 0`, else `h²` as today; gust =
`1 + wind_gust · g(dot(origin.xz, dir) − 3 m/s · t)` with `g` a smooth periodic noise; flutter added from a hash of the
vertex position at 4–6 Hz, scaled by `(h / H)`. All three are 0 by default, which takes today's exact expression.

Colour variation needs the copy's identity in the fragment stage: every vertex shader that feeds `kMeshFS`
(`kMeshVS`, `kSkinnedMeshVS`, `kInstancedMeshVS`, step 5a's `kDrawInstancedVS`) writes
`layout(location = 3) flat out vec4 v_tint` — `vec4(1)` except in the batch shader, which writes the mixed variation —
and `kMeshFS` multiplies the base colour by it. That is why step 6 follows step 5a.

Multiple meshes: one scatter (unchanged `scatter_foliage`), each point assigned to a kind by a hash weighted by
`mesh_weights`; one `InstanceBatch` per kind with a disjoint id. Determinism of placement: as today, in `seed`.

**Decals.** `Kit3DExtract` logs one warning per scene when `decals_dropped > 0`, naming the 64-decal and 3-texture
limits and the atlas + `uv_rect` workaround; `perf_run` reports `decals_dropped`.

**Demo and gates.** `samples/06_showcase` gains a "visual3" profile pair (on/off) in its sheet: parallax floor, filmic
lens, HDR sky, a crate yard (5a) and a modular wall (5b) with draw counts printed, and a varied windy meadow. Each step's
golden is already in; step 6 adds `foliage_variation`. The milestone record closes with the per-step table, the
known limits and the sheet.

**Acceptance.** Kit tests for the fields' reach and the kinds' split; render tests: defaults byte-identical, variation
changes copies' colour deterministically, `wind_height` caps a 5 m mesh's sway at the amplitude. Docs: `08-3d.md`
foliage section (including the glTF `lod_levels` keys for heavy scanned plants). Changelog fragment.

### Waves: what can run in parallel

| Wave | Steps (parallel within a wave) | Owner of shared files in the wave |
|---|---|---|
| 1 | 1b seam | — |
| 2 | 2 (POM) ∥ 4 (HDR sky) | `kit3d.h`: 4 (sky_rotation_deg); `shaders3d.h`: 2 owns `MCE_GLSL_MESH_FS`, 4 owns `MCE_GLSL_PASS_U`'s sky functions; cook: 2 the material importer, 4 the texture importer |
| 3 | 3 (filmic) ∥ 5a (instancing) | `kit3d.h`: 3 (Environment3D, Camera3DComponent); `forward3d.cpp`: 3 owns `record_post`, 5a the draw loops and pipelines; `scene3d.h`: 3 `PostFx`, 5a the run API |
| 4 | 5b (StaticMerge3D) ∥ 6 (foliage) | `kit3d.h`: 6 (5b uses its own header); `kit3d_extract.cpp`: 5b a new function and one skip in the `MeshRef` loop, 6 the foliage loops |

Modules per step stay at two or fewer besides docs (cook + kit for 2's data, render for its shader — the shader half
can be a stacked PR if the thread prefers); none touches three modules in one PR. Each PR is gated by one
`local_ci.sh --task` on its own base, per the CI process.

### Effort

| Step | Rough hours |
|---|---|
| 1b seam | 2–3 |
| 2 POM | 12–18 |
| 3 filmic, focus, exposure | 12–16 |
| 4 HDR (`.exr` adds 4–6) | 10–16 |
| 5a instancing | 20–30 |
| 5b StaticMerge3D | 16–24 |
| 6 foliage, decals, demo, close | 14–20 |
| **Total** | **86–133** (the ROADMAP's L, 80–140) |

### Decisions (coordinator, 2026-10-16, on PR #430)

1. `.exr` is in: tinyexr, vendored or through FetchContent with a pinned version, its license recorded in the
   third-party notices.
2. Step 5a ships with `r.instancing` on by default when every existing golden is byte-identical with it on, and off
   otherwise.
3. Step 5a runs on Opus.
