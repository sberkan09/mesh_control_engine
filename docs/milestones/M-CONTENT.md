# M-CONTENT — Cooked-pack size and draw-call visibility (milestone record)

**Status:** active (step 1a done; claimed 2026-10-17 by the routine 1 thread). Plan: ROADMAP §M-CONTENT. Source: GitHub #383
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
| 1b | `compress` import setting (`none`, `bc7`, `bc5` for normal maps, `bc4`/`bc1` where cheaper, `astc` on phones), a deterministic encoder, the loader and the RHI upload for the BC formats on every desktop backend, a fallback where a backend lacks BC (GLES, WebGL), a size gate on a sample scene | Sonnet (Opus review of the upload and of determinism) | not started |
| 2 | Draw-call report: `perf_run` and `mce perf` list draw calls by cause (material, no instancing, no static batch) | Sonnet | not started |
| 3 | `perf_run` progress lines while it runs and a `frames` argument for a short run; the close | Sonnet | not started |

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
