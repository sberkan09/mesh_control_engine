# M-ASSETLOOK — Seeing and tuning materials and particle presets (milestone record)

**Status:** in progress 2026-10-19. **Owner: the "Geliştirme rutini 3" thread** (a gap taken from the assessments at the coordinator's request: no unowned milestone was left). No Linear issues (Linear is unreachable and the workspace is at its free issue limit); this record is the tracker.
Source: `assessments/motor-erisim-bosluklari.md`, section 11 (Editor): "Editörü olmayanlar: malzeme, partikül preset'i, ...". Most of that list has an answer now (project settings, UI documents, timelines, animation graphs, behavior trees, skeletons, terrain), but `.mcmat` materials and `.mcpart` particle presets are still hand-written JSON for a human, and an agent that writes one cannot look at the result without rendering a scene (`asset_thumbnail` shows a plain tile for both).

**Goal.** A material and a particle preset can be *seen* on the CPU, without a GPU (a lit sphere for a material, a frame of simulated particles for a preset), by the asset browser, by `asset_thumbnail` and by `render_thumbnail`; and a human can *tune* them in the editor with a form beside a live swatch that writes the same source file the cook reads.

**Not in scope:** the real renderer's look (textures, normal maps, IBL, parallax, clearcoat: the swatch uses the scalar parameters and the base colour texture's average only; the exact look needs a device, see `assessments/gpu-gereken-isler.md`), a node-based material graph (an explicit "Won't"), particle editing beyond the preset's own fields.

## Steps
| Step | What | Modules | Model |
|---|---|---|---|
| 1. Material swatch | `thumb_from_material` (a lit sphere from base colour, metallic, roughness, emissive; blend shows the checker) and a Material branch in `render_thumbnail`; `asset_thumbnail` and the Assets grid get it for free | editor core | Sonnet |
| 2. Particle swatch | `thumb_from_particles`: the preset's emitter run for a fixed number of ticks with a fixed seed and its particles drawn as dots; a Particle preset branch in `render_thumbnail` | editor core | Sonnet |
| 3. Material panel | an editor panel for a `.mcmat` (opens on double click in Assets): fields for the parameters and the five texture slots, a live swatch, saved through the same source-file write the cook reads | editor layer | Sonnet |
| 4. Particle panel, docs, close | the same for a `.mcpart`; guides 03, 05 and 14; the milestone closed | editor layer, docs | Sonnet |

## Gates
- Every step: `local_ci.sh --task` green. The swatches have pinned tests on what does not depend on float rounding (the silhouette, which of two materials is brighter, which channel dominates), not on single pixel values.
- No new component, no change to a wide header (`importer.h`, `formats.h`, `handle.h`, `container.h`, `kit3d.h`); new code sits in `asset_thumb.h` (small) and new panels.
- The panels are UI like their peers; they compile and are covered by model tests, and their look needs a display check later.

## Step status
| Step | Status | PR | Notes |
|---|---|---|---|
| 1 | done (this PR) | | `MaterialSwatch` and `thumb_from_material` in `mce/editor/asset_thumb.h`: a sphere of radius 0.42 of the picture, one key light, Blinn-Phong with a GGX-like exponent from the roughness, F0 from the metallic, a sky and ground ambient, emissive added, `unlit` shows the base colour; the base colour texture's average multiplies the base colour; `"alpha_mode": "blend"` lets the checker through. `render_thumbnail` reads the `.mcmat` source |
| 2 | planned | | needs 1 |
| 3 | planned | | needs 1 |
| 4 | planned | | |
