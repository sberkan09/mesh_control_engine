# M-CONTENT — Cooked-pack size and draw-call visibility (milestone record)

**Status:** active (not started; claimed 2026-10-17 by the routine 1 thread). Plan: ROADMAP §M-CONTENT. Source: GitHub #383
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
| 1a | `max_size` for ordinary textures (power-of-two downscale before mips), `mce ship` / `mce cook` size options, a cook size report by asset type | Sonnet | not started |
| 1b | `compress` import setting (`none`, `bc7`, `bc5` for normal maps, `bc4`/`bc1` where cheaper, `astc` on phones), a deterministic encoder, the loader and the RHI upload for the BC formats on every desktop backend, a fallback where a backend lacks BC (GLES, WebGL), a size gate on a sample scene | Sonnet (Opus review of the upload and of determinism) | not started |
| 2 | Draw-call report: `perf_run` and `mce perf` list draw calls by cause (material, no instancing, no static batch) | Sonnet | not started |
| 3 | `perf_run` progress lines while it runs and a `frames` argument for a short run; the close | Sonnet | not started |

**Not in scope:** a bundled realistic sound, texture or model library (content, not engine); HDR texture compression (BC6H),
which M-VISUAL3 left to this milestone's later work.

**Gates:** existing replay hashes unchanged (textures are render input, never sim state); a pack cooks to the same bytes twice
and on both desktop hosts; each step has a regression test; `local_ci.sh` green on every step.
