# M-THUMBS — Seeing assets: CPU thumbnails for the asset browser and for agents (milestone record)

**Status:** planned 2026-10-18. **Owner: the "Geliştirme rutini 3" thread** (a gap taken from the assessments at the coordinator's request: no unowned milestone was left). No Linear issues (Linear is unreachable and the workspace is at its free issue limit); this record is the tracker.
Source: `assessments/unreal-kiyaslama-2026-10-16.md` (Editor row: "çoklu sahne sekmesi ve küçük resimler kesilmişti"). The multi-scene tabs landed with `scene_documents` / `scene_switch`; the asset thumbnails of M8 (FR-EDT-110) never did, so the asset browser is still a typed list. For an AI agent the same gap is bigger: `asset_list` names a texture, a mesh or a sprite sheet but an agent cannot look at it without rendering a whole scene.

**Goal.** One CPU function turns any cooked or source asset into a small RGBA picture with no GPU: textures and sprites downscaled (alpha over a checker), meshes drawn flat-shaded from a fixed three-quarter view with the CPU line/triangle raster, audio as a waveform, fonts as a sample string where the font importer already has glyphs, other kinds as a labelled tile. The asset browser shows them (cached per asset GUID and content hash, so a re-cook refreshes only what changed) and an MCP tool `asset_thumbnail` returns the PNG, so an agent can see its generated assets.

**Not in scope:** GPU-rendered material previews (needs a device: see `assessments/gpu-gereken-isler.md`), animated previews, video frames, editing from the thumbnail.

## Steps
| Step | What | Modules | Model |
|---|---|---|---|
| 1. Thumbnail core | `mce/editor/asset_thumb.h`: `render_thumbnail(path or guid, size, Image&)` for textures, sprites, meshes, audio and the fallback tile; pure function of the asset bytes (same asset, same pixels); exact-pixel golden tests per kind | editor core | Sonnet |
| 2. MCP | `asset_thumbnail` (one asset) and a contact-sheet mode for a folder, PNG image content, with `size` and `kind` filter; read-only | editor mcp | Sonnet |
| 3. Asset browser | thumbnails in the browser grid, cached by GUID plus content hash, built lazily a few per frame; list mode unchanged | editor layer | Sonnet |
| 4. Docs, close | guide 03 and 14, the milestone closed | docs | Sonnet |

## Gates
- Every step: `local_ci.sh --task` green. Step 1 has exact-pixel goldens (a 4x4 texture at 2x2, a known mesh's covered-pixel count, the waveform of a known tone); a thumbnail is deterministic.
- No new component, no change to a wide header (`importer.h`, `formats.h`, `handle.h`, `container.h`, `kit3d.h`); the new header is small and included by few files.
- The browser change (step 3) is UI like its peers; it compiles and is covered by the model tests, and its look needs a display check later (listed in `assessments/gpu-gereken-isler.md` if that file tracks such checks).

## Step status
| Step | Status | PR | Notes |
|---|---|---|---|
| 1 | done (this PR) | | `mce/editor/asset_thumb.h`: `thumb_from_image` / `thumb_from_pcm` / `thumb_from_mesh` / `thumb_tile` and `render_thumbnail(project, asset, size, out)`; exact-pixel tests. Textures read the source PNG, audio the source WAV (Ogg and the other kinds show the tile for now), meshes the mounted cooked chunk |
| 2 | planned | | needs 1 |
| 3 | planned | | needs 1 |
| 4 | planned | | |
