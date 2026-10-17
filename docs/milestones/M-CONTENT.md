# M-CONTENT — Cooked-pack size and draw-call visibility (milestone record)

**Status:** done (all steps shipped; see Milestone close below). Earlier: active (steps 1a and 1b done; claimed 2026-10-17 by the routine 1 thread). Plan: ROADMAP §M-CONTENT. Source: GitHub #383
(blind test 3). **Tracker:** this table. Linear is at its free issue limit, so no issues are filed.

**Why:** the Lantern Keep project had 78 textures at 1024² and its cooked `assets.mce` was 614 MB (228 MB zipped, 386 MB in
the shipped zip) because ordinary textures cook to uncompressed RGBA8 plus mips; the texture importer has no max-size
option and no block compression on desktop (ASTC 4x4 exists for phones through `mce_cook --textures astc`). Its 3,100 draw
calls on a 280k-triangle scene had no report that explained them, and `perf_run` printed nothing while it ran.

**Boundaries.** M-VISUAL3 step 4 already gave HDR textures (`.hdr` / `.exr`) their own `max_size` and `exposure` and keeps
RGBA16F; this milestone does not touch that path. M-VISUAL3 step 2 (POM) owns the material importer. M-CONTENT works in the
texture importer's LDR path, the cook CLI and `mce ship`, the RHI upload mapping for BC formats, and `perf_run`.

| # | Step | Model | State |
|---|---|---|---|
| 1a | `max_size` for ordinary textures (power-of-two downscale before mips), `mce ship` / `mce cook` size options, a cook size report by asset type | Sonnet | done |
| 1b | `compress` import setting (`none`, `bc7`, `bc5` for normal maps, `bc4`/`bc1` where cheaper, `astc` on phones), a deterministic encoder, the loader and the RHI upload for the BC formats on every desktop backend, a fallback where a backend lacks BC (GLES, WebGL), a size gate on a sample scene | Sonnet (Opus review of the upload and of determinism) | done (#444 #460 #463 #466 #469 #472; §Step 1b plan below; as built after it) |
| 2 | Draw-call report: `perf_run` and `mce perf` list draw calls by cause (material, no instancing, no static batch) | Sonnet | done (#476) |
| 3 | `perf_run` progress lines while it runs and a `frames` argument for a short run; the close | Sonnet | done (this PR, GH-390) |

**Step 1a as built.** The LDR (PNG/JPEG) path of `texture_importer.cpp` takes a sidecar `"max_size"` (power of two 1..16384,
same wording as the HDR key; no default, so no limit) and `ImportInput::max_texture_size`, the project-wide cap that
`CookOptions::max_texture_size` carries; the effective cap is `min(sidecar, project)`. A bigger image is box-halved (the
mip filter, `downsample`: sRGB-aware, no renormalizing of normal maps) until both sides fit, before the mip chain. HDR is
untouched and ignores the project cap. With no setting the code path and the bytes are those of before (a test compares
manifests). The cap is part of the re-cook key: `cook_tool_version(textures, max_texture_size)` adds log2+1 of the cap in
bits 24..31, but only for texture importers (`tool_version_for` in `cook.cpp`), so changing the cap re-cooks the textures and
not the sounds and scenes; materials follow through their dependency hashes. `mce_cook --max-texture-size N` (invalid: exit 1
with the fix), `mce cook` (own option, still passes through after the project), `mce ship` (desktop and script projects
re-cook with the cap into `<project>/assets.mce`; web and `scripts/ship.py` pass `-DMCE_COOK_MAX_TEXTURE_SIZE=N` to CMake,
which `cmake/MceMobile.cmake` adds to `MCE_COOK_TEXTURE_ARGS`) and MCP `cook_run {"max_texture_size": N}`. The project
settings file has no cook or texture section, so there is no project-file setting yet. The size report (`CookReport::sizes`):
bytes of every live asset's cooked file by type (textures, meshes, audio = sfx + music, scenes = scenes + prefabs, other),
the five largest by path, the texture count and how many were cut; it is computed on every cook, cached or not (a texture
is read back to compare its base size with the source only when a cap could apply). `mce_cook` (not `--quiet`) prints
`mce_cook: size: ...`, `mce_cook: textures: N, D downscaled ...` and the five largest after the existing summary line, whose
text is unchanged; `cook_run` returns it as `sizes`. Tests: `tests/asset/test_cook_texture_cap.cpp`, the `cook_run` case in
`tests/mcp/test_mcp.cpp`, `tests/import/test_cli_max_texture_size.py`. The MCP golden (`demo_edit`) changed legitimately: the
`cook_run` schema and description, and its result gained `sizes`.

**Not in scope:** a bundled realistic sound, texture or model library (content, not engine); HDR texture compression (BC6H),
which M-VISUAL3 left to this milestone's later work.

**Gates:** existing replay hashes unchanged (textures are render input, never sim state); a pack cooks to the same bytes twice
and on both desktop hosts; each step has a regression test; `local_ci.sh` green on every step.

## Step 1b plan

Texture block compression for desktop packs (BC7 / BC5 / BC4 / BC1), beside the existing ASTC 4x4 for phones. Written
against `origin/dev_claude` after 1a; file:line references are to that tree. Guesses are marked *(guess)*.

**Why.** Lantern Keep's 78 textures at 1024² cook to about 416 MiB of RGBA8 plus mips (of the 614 MB pack), on disk and in GPU memory.
BC7 and BC5 store one byte per texel, BC4 and BC1 half a byte. With the same textures the pack drops to about 104 MiB
and the GPU memory drops by the same amount. This is the largest size cut left after 1a's `max_size`.

**What exists.**
- `asset::TextureFormat` (`asset/include/mce/asset/formats.h:103-107`): `RGBA8 = 1`, `ASTC_4x4 = 2`, `RGBA16F = 3`. The
  comment there still says "BC1/BC5/BC7 at v0.5". `texture_level_bytes` (`asset/src/formats.cpp:129-136`) handles only
  those three formats. `TextureView::read` (`formats.cpp:144-148`) refuses every other format with `Errc::Unsupported`,
  so an old runtime refuses a BC pack cleanly and `TEXTURE_VERSION` (`formats.h:15`) can stay 1. The chunk is a
  `TextureHeader` (32 bytes), a `TextureMip` table, and then each level's layers back to back. That layout is format
  agnostic and does not change.
- `rhi::TextureFormat` (`rhi/include/mce/rhi/types.h:35-41`) already lists `BC1_UNorm, BC3_UNorm, BC5_UNorm, BC7_UNorm,
  BC7_SRGB`. It has no `BC4_UNorm` and no `BC1_SRGB`. `is_compressed_format` is at `rhi/src/types.cpp:9-20`. Every backend
  already maps the five BC formats: GL `gl_device.cpp:39-43` (S3TC through `EXT_texture_compression_s3tc`; RGTC and
  BPTC are core in GL 4.5), SDL GPU `sdlgpu_device.cpp:29-33`, WebGPU `webgpu_device.cpp:27-31`, Metal
  `metal_device.mm:29-33`. Every backend also takes compressed uploads per level: GL `glCompressedTextureSubImage3D`
  (`gl_device.cpp:379-384`, `416-423`), WebGPU `row_layout` (`webgpu_device.cpp:50-55`), Metal `row_bytes`
  (`metal_device.mm:52-54`). The block size is computed as `f == BC1_UNorm ? 8 : 16` in five copies:
  `gl_device.cpp:54-60`, `texture_store.cpp:16-22`, `sdlgpu_device.cpp:43-49`, `webgpu_device.cpp:41-47` and `52`, and
  `metal_device.mm:43-54`. BC4 is 8 bytes per block, so each of those copies would be wrong for it.
- No backend reports which formats it can sample. `DeviceCaps` (`rhi/render_device.h:31-47`) has no flag for it.
  WebGPU asks for `TextureCompressionBC` and `...ASTC` only when the adapter has them (`webgpu_device.cpp:230-236`). SDL
  GPU logs "format not supported" and returns a null handle (`sdlgpu_device.cpp:455-462`). GL simply issues the call,
  so an ASTC pack on a desktop driver is a GL error and draws nothing (`gl_device.cpp:47`). `render::create_texture` does
  not check the handle (`texture_store.cpp:53`). M-MOBILE records "BC textures are rejected where the GPU lacks them".
  There is no fallback today, for either direction.
- `render::upload_cooked_texture` (`render/src/asset_upload.cpp:13-35`) maps the asset format to the RHI format at
  `:27-32`, and `compose_layer_array` (`render/src/terrain.cpp:382-387`) repeats that mapping. Everything else is
  `memcpy` of `layer_size` bytes, so it already works for block formats.
- The ASTC cook is in `texture_importer.cpp:517-557`. astc-encoder 5.6.0 (`cmake/Dependencies.cmake:213-246`; the tag
  resolves to `2c9eafa7`) is built as the no-SIMD path (`ASTCENC_SSE=0 ...`) with `-ffp-contract=off` and
  `/fp:precise`. It encodes every level of the RGBA8 chain with one thread at medium quality. The "astcenc wall" is
  at `scripts/ci/check_walls.py:56-57`. Every LDR texture becomes ASTC whenever `ImportInput::texture_compression ==
  Astc4x4`. The sidecar has no per-texture key; its keys are `srgb, mips, filter, max_size` (`:429`).
- Mode plumbing: `cook::TextureCompression { None, Astc4x4 }` (`cook/importer.h:67`), `CookOptions::textures`
  (`cook.h:150`), and `cook_tool_version` (`cook.h:160-168`), which puts the mode in bits 16..23 for every importer and
  1a's cap in bits 24..31 for textures only (`cook.cpp:134-135`). The sidecar is in the cache key through
  `settings_hash` (`cook.cpp:145`). The command-line side: `mce_cook --textures rgba8|astc` (`tools/cook/main.cpp:77-81`),
  `MCE_COOK_TEXTURES` and `MCE_COOK_TEXTURE_ARGS` (`cmake/MceMobile.cmake:33-43`, astc by default on phones), and `mce ship`
  (`tools/mce_cli/mce.py:457`), which has no `--textures` yet.
- CPU readers of cooked texture data: `compose_layer_array` (layer arrays for terrain and parallax,
  `kit3d_extract.cpp:120`, `:258`), which copies blocks without interpreting them. `sprite_extract.cpp:137`, `:234` and
  1a's report (`cook.cpp:190`) read only the header. Tests read RGBA8 texels (`test_cook_texture_cap.cpp`,
  `test_cook_project.cpp`, `test_guid_formats.cpp`, `tests/render/test_terrain.cpp`); with the default mode those tests
  keep seeing RGBA8. The editor and the MCP image tools (`image_diff`, `render_frame`) never decode cooked texels; their
  previews go through the GPU. So nothing on the CPU breaks under BC. The GPU-side readers that do break are the
  shader's normal decode (below) and the 2-layer [normal, height] parallax array.

**Encoder: bc7enc_rdo (BC7 through `bc7enc.cpp`, BC1/BC4/BC5 through `rgbcx`).**

| Option | License | BC7 | Fit |
|---|---|---|---|
| **bc7enc_rdo** (richgel999) | MIT or Unlicense | yes | scalar C++, no SIMD or threads. We compile only `bc7enc.cpp` (2.6k lines), `rgbcx.cpp` (3.1k lines plus a 444 KB table header) and `bc7decomp.cpp` (tests). **Pick.** |
| ISPC texcomp | MIT | yes | needs the ISPC compiler on every host: no |
| Compressonator | MIT | yes | large SDK with its own build and GPU paths: too heavy for a cook dependency |
| DirectXTex | MIT | yes | Windows first, DirectXMath/SIMD paths, its BC7 is fastest on the GPU: no |
| stb_dxt | public domain | **no** | BC1/BC3/BC4/BC5 only; we would still need a BC7 encoder |

Probe measurements on this container (x86-64, 4 cores, one thread, a 1024² gradient+noise image). BC7 at uber level 0
with 16 partitions and perceptual weights: 0.42-0.54 s, PSNR 37.7 dB. BC1 at level 10: 0.19-0.25 s. BC5: 8-27 ms with
`encode_bc5`, but 1.3-1.6 s with `encode_bc5_hq`. The output bytes (FNV hash) were identical across gcc 13.3 -O0/-O2
and clang 18 -O2/-O3 `-march=native`, all with `-ffp-contract=off`. With `-ffp-contract=fast -march=native` the BC7 and
BC1 bytes changed, so the flag is required, as it is for tinyexr and astcenc. An arm64 host is not measured. *(guess:
the encoder is scalar float with only `sqrtf` and `floorf`, which IEEE rounds exactly, so arm64 matches)*. Until it is tested, cook golden hashes are guaranteed only on x86_64
(coordinator decision); 1b-ii's acceptance adds a cross-arch encoder byte-equality test (marked slow/manual if no arm64
host is available), which settles it. Cook cost for a 1024² texture with mips is about 0.6 s; jobs run
textures in parallel (`cook.cpp:549`).

ADR-0017 integration: `FetchContent_Declare(bc7enc_rdo GIT_REPOSITORY https://github.com/richgel999/bc7enc_rdo.git
GIT_TAG b9438627eef73a1157e84201b6fa6eb2ffd6d9f0 SOURCE_SUBDIR _no_cmake_)`. The repo has no tags, and `git ls-remote`
on 2026-10-17 gives this commit for `master`. Use a commit pin and no `GIT_SHALLOW`, like tinyexr. Build it as
`mce_bc7enc` STATIC inside `if(NOT CMAKE_CROSSCOMPILING)`, with `-w -ffp-contract=off`, `/W0 /fp:precise` on MSVC, and
PIC. Never compile `bc7e.ispc`, the repo's `ispc.exe` or the RDO files. Walls: a "bc7enc wall"
`[<\"](bc7enc|rgbcx|bc7decomp)\.h|\bbc7enc_\w+\(|\brgbcx::`, allowed in `src/engine/asset/cook/src/texture_bc.cpp` and
`tests/asset/test_cook_bc.cpp`, and a "bc7enc link wall" `\bmce_bc7enc\b`, allowed in `src/engine/asset/cook/` and
`tests/asset/`. Record the license in `ARCHITECTURE.md` §7 and the ADR-0017 amendment list.

**Cook** (`texture_importer.cpp` and a new `texture_bc.cpp` / `texture_bc.h` in `asset/cook/src`).
- The sidecar gains `"compress"` with the values `"none" | "auto" | "bc7" | "bc5" | "bc4" | "bc1" | "astc"`. The cook
  mode decides what each value produces. A missing key means "the mode's default", so every existing pack stays
  byte-identical:

  | `compress` | `--textures rgba8` (default) | `--textures bc` (new) | `--textures astc` |
  |---|---|---|---|
  | missing | RGBA8 | as `auto` | ASTC 4x4 (today) |
  | `none` | RGBA8 | RGBA8 | RGBA8 (pixel art, LUTs) |
  | `auto` | RGBA8 | the rule below | ASTC 4x4 |
  | `bc7` / `bc5` / `bc4` / `bc1` | RGBA8 | that format | ASTC 4x4 |
  | `astc` | RGBA8 | RGBA8 | ASTC 4x4 |

  An unknown value is a `file:line:col` error that lists the allowed values. HDR files keep RGBA16F, and `compress` on an
  `.hdr` or `.exr` is an unknown-key error (BC6H is out of scope). `envmap_importer.cpp` never compresses (RGBM).
- **`auto` under `bc`.** The rules are checked in order on the base level, after `max_size`. Each test is integer-only,
  so the choice is deterministic. (1) `srgb` true → `BC7` with the `Srgb` flag. (2) `srgb` false, `r == g == b` on every texel and `a == 255` → `BC4` (height and other grey
  data; tested before the normal rule because bright grey, c about 194..208, also passes the unit-length test). (3) `srgb` false and at least 99% of
  texels decode to a unit normal → `BC5`. A texel passes when `x,y,z = 2c-255`, `x²+y²+z²` is within `[0.81, 1.21]·255²`,
  `z > 0`, and `a == 255`. (4) Otherwise → `BC7`. *(provisional; will be calibrated with RunPod measurements. guess: the 99% and the length bounds; 1b-iv tunes them on the Lantern Keep and
  Poly Haven normal maps)*. The rule never picks `bc1`, which is lossy on gradients. `bc1` is for opaque colour by
  explicit choice. `auto` also keeps `none` for LUT-shaped strips (`w == h*h` or `h == w*w`, with `h >= 16`), where
  colour grading cannot take block error *(guess)*.
- **Sizes.** BC is used only when both base sides are multiples of 4. WebGPU and D3D12 refuse a BC texture whose base
  is not. Otherwise the texture cooks to RGBA8 with a cook warning that names the file and the fix (resize the image, or
  pick a `max_size`). Mips below 4 pixels are encoded as one padded block: the 4x4 input repeats edge texels
  (clamp), and `texture_level_bytes` rounds up. Every level is encoded from the RGBA8 chain that the box filter built,
  exactly as ASTC does. The order is 1a's `max_size` halving, then the 4-multiple check, then the mips, then the encode.
  The importer writes `layers = 1` only. Arrays are composed at runtime and cube maps come only from the envmap
  importer, so neither case reaches the encoder.
- **Encoder settings** live in `texture_bc.cpp` and are part of the output. Changing them bumps the texture importer's
  `version()`. BC7: `bc7enc_compress_block_params_init`, uber 0, max partitions 16, perceptual weights for sRGB and
  linear weights otherwise. BC5: `rgbcx::encode_bc5` on R and G; BC4: `encode_bc4` on R. BC1: `encode_bc1(level 10,
  allow_3color false)`; an alpha below 255 is a cook warning, because BC1 here is opaque. The one-time table inits
  (`bc7enc_compress_block_init`, `rgbcx::init`) write globals, so they run under `std::call_once` before the first encode
  (cook stages run on `jobs::` threads). `texture_bc.h` exposes `encode_bc_level` and, for tests, `decode_bc_level`.
- **Modes and keys.** `TextureCompression` gains `Bc = 2`. In `cook_tool_version`, `bc` is a new value in bits 16..23, so
  switching mode re-cooks. That re-cooks everything, as switching to ASTC does today: `test_cook_project.cpp:457`
  expects that behaviour, so it stays. `compress` reaches the key through `settings_hash`. The importer's version does
  not change for a missing key (an RGBA8 manifest compare, as in 1a).
- **CLI and ship.** `mce_cook --textures rgba8|bc|astc`. `MCE_COOK_TEXTURES` accepts `bc` only when the build is neither
  `MCE_MOBILE` nor Emscripten (otherwise `FATAL_ERROR` naming the reason). `mce cook --textures` passes the flag
  through. `mce ship --textures rgba8|bc`: desktop and script projects re-cook into `<project>/assets.mce`, like 1a's cap,
  and can combine it with `--max-texture-size`. `--target web` with `bc` exits with "a web pack serves every browser,
  phones included: ship it rgba8". `scripts/ship.py --target android|ios` always cooks astc and refuses `bc`. The desktop
  ship default stays `rgba8` in 1b. MCP `cook_run` gains `"textures": "rgba8"|"bc"` (the `demo_edit` golden changes),
  and the `asset_set_import_settings` text (`tools.cpp:1534`) names `compress`.

**Runtime.**
- `asset::TextureFormat` gains `BC1 = 4, BC4 = 5, BC5 = 6, BC7 = 7`. sRGB stays a flag. `texture_level_bytes` gives
  `ceil(w/4)·ceil(h/4)·8` for BC1/BC4 and `·16` for BC5/BC7. `TextureView::read` accepts the four, refuses `Srgb` on BC4
  and BC5 (like RGBA16F), and refuses a BC base whose side is not a multiple of 4.
- `rhi::TextureFormat` appends `BC4_UNorm, BC1_SRGB` at the end, so existing values stay put. `rhi::block_bytes(f)`
  (8, 16, or 0 for uncompressed) and `rhi::level_bytes(f, w, h)` go in `rhi/types.h` and `types.cpp`. They replace the
  five private copies listed above, and each backend maps the two new formats (GL `GL_COMPRESSED_RED_RGTC1` and
  `GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT` = 0x8C4D; SDL GPU `BC4_R_UNORM`, `BC1_RGBA_UNORM_SRGB`; WebGPU `BC4RUnorm`,
  `BC1RGBAUnormSrgb`; Metal `BC4_RUnorm`, `BC1_RGBA_sRGB`).
- `DeviceCaps` gains `bool texture_bc = false, texture_astc = false;`. GL sets `texture_bc` when it has
  `GL_EXT_texture_compression_s3tc` and `texture_astc` when it has `GL_KHR_texture_compression_astc_ldr`. SDL GPU uses
  `SDL_GPUTextureSupportsFormat` on BC1/BC4/BC5/BC7 as a `2D_ARRAY` sampler, and on ASTC 4x4. WebGPU uses
  `wgpuDeviceHasFeature` on the two features it already requests. Metal uses `supportsBCTextureCompression` (macOS
  11+) for BC and `supportsFamily(MTLGPUFamilyApple2)` for ASTC.
- `render::rhi_format_for(asset::TextureFormat, bool srgb)` (in `asset_upload.h`, which reaches 44 TUs) becomes the one
  mapping, used by `upload_cooked_texture` and by `compose_layer_array`. `create_texture` refuses a compressed format
  that the caps lack, with `Errc::Unsupported` at `render.texture.format`, plus one error line per process: "texture
  <guid> is BC7 but this GPU cannot sample BC textures: cook the pack with --textures rgba8 (web) or astc (phones)". It
  also checks for a null handle. The texture then draws as the existing error checker.
- **Fallback decision: no CPU decode at load. A pack has to be cooked for its target.** Decoding BC7 back to RGBA8
  costs 4× the GPU memory (5.3 vs 1.3 MiB per 1024² chain, so Lantern Keep returns to ~416 MiB) and *(guess)* 10-30 ms
  of load time per 1024² chain on one core. It also puts a decoder in every runtime, the wasm player included. The only
  targets without BC (phones and web) already get their own cook, and the ship tools above enforce it. The clean
  refusal and the error checker cover a mis-shipped pack. The same rule now covers an ASTC pack on a desktop, which is
  a silent GL error today.
- **Normal decode.** BC5 samples as `(x, y, 0, 1)`. A helper, `vec3 unpack_normal(vec4 t)`, rebuilds
  `z = sqrt(max(1 - dot(xy, xy), 0))` only when `t.b == 0.0`. Otherwise it returns `t.xyz * 2 - 1` exactly as today, so
  RGBA8 maps (b >= 128 in practice) keep their bits. It replaces the five decode sites: `shaders3d.h:560, 564, 568, 725`
  and the 2D lit sprite at `renderer.cpp:51`. No uniform and no new sampler (the bank is full).
- **Parallax array.** `compose_layer_array` stays strict: same format, size and mips. It gains one lossless case: a BC4
  layer in a BC5 array becomes BC5 blocks `[bc4 block, 8 zero bytes]`, so G = 0 and the POM reads `.r`. Under `auto` a
  grey height map is BC4 and its normal map is BC5, so parallax keeps working (M-VISUAL3 step 2's requirement).
  `Kit3DExtract::material()` logs one warning when a BC4 texture is bound to a slot that reads more than R (base colour,
  metallic-roughness, emissive, normal), naming the GUID and `compress: bc7`.
- **Per backend.** GL (desktop 4.5): BC4, BC5 and BC7 are core; BC1 depends on S3TC, which every desktop driver and Mesa
  (≥ 17.3) expose *(guess for llvmpipe: it claims GL 4.5, so BPTC and RGTC are there; 1b-i's caps test prints the
  answer)*. SDL GPU: Vulkan and D3D12 desktop have BC; *(guess: lavapipe sets `textureCompressionBC`; the CI test skips
  with a message if the cap is false)*; phones fail the cap. WebGPU: BC only where the adapter offers it (most desktop
  browsers; not Android), which is why the web pack stays RGBA8. Metal: BC on every Mac (Apple silicon and Intel); not
  on iPhone, which uses ASTC.

**Determinism and goldens.** Textures are render input and never sim state, so replay and `state_hash` are unaffected.
A pack cooks to the same bytes twice and on both desktop hosts: the cross-host test pins a hash of a generated texture's
BC7, BC5, BC4 and BC1 chains and runs on Linux and macOS in `local_ci`. With a missing key under `rgba8` or `astc`,
every pack, manifest and golden is byte-identical, and the existing goldens are expected to stay unchanged. The
`unpack_normal` branch is verified the same way as step 4: the 8 golden scenes on both backends, byte for byte. Image
quality is gated by error bounds, not byte equality, because GPU BC decode may differ by a level between vendors:
generated 256² gradient+noise images, decoded by `decode_bc_level` and by a GPU readback, must reach PSNR ≥ 35 dB for
BC7 and ≥ 38 dB per channel for BC5/BC4, with a maximum per-channel error ≤ 48 *(guess: thresholds from the probe's
37.7 dB for BC7, set before the measurement in 1b-ii)*.

**Budget** (1024², with mips = ×4/3):

| Format | Bytes/texel | One texture | Lantern Keep (78) |
|---|---|---|---|
| RGBA8 | 4 | 5.33 MiB | 416 MiB |
| BC7 / BC5 / ASTC 4x4 | 1 | 1.33 MiB | 104 MiB |
| BC4 / BC1 | 0.5 | 0.67 MiB | 52 MiB |

The pack bytes are the GPU bytes, plus headers. *(guess: zip deflates BC payloads 10-30%, against far more for flat
RGBA8 art, so the shipped zip shrinks less than 4×)*. Cook time is about 0.6 s of CPU per 1024² BC7 chain, run in
parallel. There is no per-frame cost: sampling BC costs the same or less bandwidth.

**Risk.** (1) Pixel art and UI cooked with `bc` blur at block edges: the docs tell 2D projects to stay `rgba8` or to
set `compress: none`. *(guess: most 2D projects never ship `bc`)*. (2) The auto normal-map test can misfire on a flat
data texture. The warning names the choice, and the sidecar overrides it. (3) Colour palettes (kit `colormap.png`)
bleed across cells that are not 4-aligned. `auto` on a palette is a visible change, so the docs suggest `none` for
palettes. (4) arm64 bytes differ from x86. The fallback is a per-host golden and no cache sharing across hosts, but the
plan expects equality. (5) bc7enc_rdo is a personal repository with no releases. We pin a commit, and the three files
are small enough to vendor if it disappears.

**Out of scope.** BC6H for HDR: RGBA16F skies are few and large, and the encoder needs its own float path and
determinism work. Streaming and residency, which are a separate milestone. KTX2 / Basis Universal: a supercompressed
transcode-at-load format would put a transcoder in every runtime and give up the byte-identical cook we gate on. Per-target
packs already give each device its native format.

**Acceptance.**
- `tests/asset/test_cook_bc.cpp`: each `compress` value under each mode follows the table; the `auto` picks hold on
  generated albedo, normal, grey and ORM images; a non-multiple-of-4 base warns and gives RGBA8; mips down to 1x1 have
  the right byte sizes; `srgb`+BC4 is refused; `compress` on `.hdr` is refused; the PSNR bounds hold; the cook is
  deterministic (twice, plus the pinned hash); a missing key under `rgba8` gives an identical manifest; `--textures bc`
  re-cooks.
- Cross-arch encoder byte-equality test (x86_64 vs arm64 pinned hash; `mce_mark_slow` / manual when no arm64 host).
- Rejection error text for a pack the device cannot sample names the target and the fix ("re-cook for this target").
- `tests/asset/test_guid_formats.cpp`: `texture_level_bytes`, and `TextureView::read` for the four new formats.
- `tests/render_gl/test_render_bc_gl.cpp` (GL and `MCE_RHI=sdlgpu`): upload, sample and read back each format within
  the bounds; `unpack_normal` on BC5 matches RGBA8 shading within the error bound; a format whose cap is false is
  refused cleanly and draws the error checker; the BC4-into-BC5 parallax compose.
- `tests/import/test_cli_textures.py`: `mce cook` and `mce ship --textures bc` pass the flag; web plus `bc` exits; an
  invalid value exits.
- **Size gate** (in `test_cook_bc.cpp`, using 1a's `CookReport::sizes`): `samples/05_ashen_court` (six 256² textures:
  albedo, normal, ORM) cooked with `--textures bc` must have `sizes.textures` ≤ 0.27 × the `rgba8` cook, and its two
  normal maps must cook to BC5. A Lantern Keep number goes in "as built" as information only (the content is not in the
  repo).
- Docs: `docs/user-guide/05-assets.md` (the `compress` key, the mode table, pixel-art and LUT advice, "keep the pack
  small"), `15-shipping.md` (`--textures`), ARCHITECTURE §7 (license), ADR-0017 amendment, a changelog fragment, and this
  record's "as built".

**PRs** (one `local_ci.sh --task` each; fanout from `scripts/ci/include_fanout.py`):

| PR | Content | Hours | Review |
|---|---|---|---|
| 1b-i seam | `formats.h` (306 TUs): the four enum values and comment; `rhi/types.h` (162): `BC4_UNorm`, `BC1_SRGB`, `block_bytes`, `level_bytes`; `cook/importer.h` (167): `TextureCompression::Bc`; `render_device.h` (125, carried here so 1b-ii and 1b-iii can run in parallel): the two caps. The `.cpp` sides: `texture_level_bytes`, `TextureView::read`, the five block-size copies replaced, the caps set per backend. Nothing produces BC yet. The PR description gives CI rule M3's reason: three headers over 150 | 3-4 | anyone (mechanical) | **done, #460**
| 1b-ii cook | dependency, walls, `texture_bc.cpp`, explicit `compress` values, `--textures bc` in `mce_cook` and CMake, determinism and PSNR tests | 8-12 | **Opus** (determinism) | **done, #463**
| 1b-iii runtime | `rhi_format_for`, the caps refusal, the new formats mapped on four backends, `unpack_normal`, BC4→BC5 compose, the Kit3D warning, the render tests | 8-12 | **Opus** (upload, per backend) | **done, #466**
| 1b-iv auto + ship + gate | the `auto` rule and thresholds, `mce ship`/`ship.py --textures`, MCP `cook_run`, the size gate, docs | 6-9 | Sonnet; coordinator checks the thresholds | **done, this PR**
| **Total** | | **25-37** | |

Also merged for 1b: #444 (this plan) and #469 (the `texture_bc_srgb` cap: a BC1 sRGB pack is refused cleanly on a device without it).

1b-ii and 1b-iii run in parallel after the seam: 1b-ii owns `asset/cook`, 1b-iii owns `render` and the `rhi_*`
backends. 1b-iv follows both.

## Step 1b as built

**Done:** #444 (plan), #460 (1b-i seam), #463 (1b-ii cook), #466 (1b-iii runtime), #469 (BC1 sRGB cap) and 1b-iv (this PR).

- **Cook (#463).** `compress` in the texture sidecar, `--textures bc` in `mce_cook` and CMake (`MCE_COOK_TEXTURES=bc`, fatal
  on phones and Emscripten), bc7enc_rdo behind the walls, `choose_auto` (srgb to BC7, grey to BC4, unit normals to BC5, else
  BC7; grey is tested before normals), non-multiple-of-4 bases cook RGBA8 with a warning.
- **Runtime (#466, #469).** The four asset formats, `rhi::block_bytes` / `level_bytes`, `DeviceCaps::texture_bc`,
  `texture_astc`, `texture_bc_srgb`, a clean refusal plus the error checker on a device that cannot sample the pack,
  `unpack_normal` for BC5, the BC4-into-BC5 parallax compose and the Kit3D BC4 warning.
- **Ship (1b-iv).** `mce cook --textures rgba8|bc|astc` passes through. `mce ship --textures rgba8|bc`: desktop and script
  projects re-cook into `<project>/assets.mce` (with `--max-texture-size` if given), `--target web` with `bc` exits ("a web
  pack serves every browser, phones included: ship it rgba8"), `astc` is refused (phones use `scripts/ship.py`).
  `scripts/ship.py --textures rgba8|bc` sets `MCE_COOK_TEXTURES` on every ship (desktop rgba8 or bc, android and ios always
  astc, web rgba8) and refuses `bc` for android, ios and web. The desktop default stays `rgba8`. MCP `cook_run` takes
  `"textures": "rgba8"|"bc"` (`astc` is an argument error: phone packs come from `ship.py`); the plumbing is a trailing
  `cook::TextureCompression` parameter on `EditorProject::cook` and `EditorSession::cook` (headers with a fanout of 90 and
  83 TUs, under the 150 warning; no wide header was touched). The `demo_edit` transcript golden and
  `docs/engine/capabilities.md` follow the changed `cook_run` schema; the `asset_set_import_settings` text names `compress`.
- **Size gate.** `tests/asset/test_cook_bc.cpp` cooks the six 256² textures of `samples/05_ashen_court` with `rgba8` and `bc`:
  `sizes.textures` must be at most 0.27 times the RGBA8 cook, and both normal maps must be BC5. The thresholds, and the
  `auto` thresholds (99% unit normals, length bounds 0.81..1.21), are provisional and will be calibrated with RunPod
  measurements, including the Lantern Keep and Poly Haven normal maps (the Lantern Keep number is information only: that
  content is not in the repo).
- **Tests.** `test_cook_bc` (cook, auto, PSNR, determinism, size gate), `test_cook_bc_arch` (slow), `tests/mcp/test_mcp.cpp`
  (`cook_run` `textures`), `tests/import/test_cli_textures.py` (`cli_textures`: flag passed through, web plus `bc` exits,
  `ship.py` refuses `bc` on android, ios and web).

**Follow-ups.**
- Runtime check of BC upload and sampling on Metal, WebGPU and D3D12 goes on the RunPod GPU list (only GL and SDL GPU
  on Linux are exercised in CI; the other backends are mapped but not run).
- Latent ASTC issue: WebGPU and D3D12 want a compressed texture's base size to be a multiple of the block size. The BC
  cook falls back to RGBA8 for a base that is not a multiple of 4, but the ASTC 4x4 cook (`--textures astc`) does not, so an
  ASTC pack with such a texture can fail on a WebGPU device. No ship path combines the two today (the web is rgba8);
  fix it before ASTC reaches a WebGPU target.
- Calibrate the `auto` thresholds and the size gate with real content (RunPod).
- Cross-arch encoder byte equality (`test_cook_bc_arch`) still waits for an arm64 host.

## Step 3 as built, and the milestone close

**Step 3.** `perf_run` reports progress when the client sends a `progressToken`: a `notifications/progress` with the phase in
`message` (`launching the player`, `warmup N/M`, `capturing N/M`, `reading the capture`) and `progress` / `total` counting
warmup plus captured frames. The player prints `perf warmup N/M` and `perf capturing N/M` about ten times per phase (the frame
counts decide, not the clock, so the lines repeat); `spawn_player` reads its stdout as it arrives (`Process::read_some`) and
`perf_run` turns the lines into notifications. A client with no token gets nothing extra. `frames` now starts at 1; below 30
captured frames the report carries a `note` that the percentiles are noisy (the counters still repeat). The stats are
unchanged for 30 or more.

**Milestone close.** Shipped: `max_size` and a cook size report (1a); the BC `compress` setting, deterministic encoder, loader
and upload on the desktop backends, `auto` rule and size gate (1b); the draw-call report by cause in `perf_run` / `mce perf`
(2); progress and short runs (3). Stays open: GPU verification of Metal, WebGPU and D3D12 uploads on RunPod; calibrating the
`auto` thresholds and the size gate with real content; ASTC 4x4 on a WebGPU device for a texture whose base is not a multiple
of 4; and offender names in the `perf_run` draw-call report (it names causes and counts, not the materials or entities).
