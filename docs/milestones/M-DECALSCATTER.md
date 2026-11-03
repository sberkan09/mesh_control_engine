# M-DECALSCATTER — Decal scatter: stains, cracks and litter spread over a ground patch from one component, so a large surface stops looking like one flat colour (milestone record)

**Status:** planned (2026-11-03), steps 35a to 35d. PRs: this plan. **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-VISUAL 9 (`Decal3D`, `render::Decal`, the projected decal pass) and of the 3D visual assessment (`assessments/3d-gorsellik-degerlendirmesi.md` gap 9, and the GTA quality note "grass is one flat colour, use decals for stains and road edges"). Today one decal is one entity, so dressing a plaza with forty stains or cracks means forty hand-placed entities and a scene that hides the pattern nowhere. No Linear issues (free issue limit, Linear unreachable); this record is the tracker.

**Naming:** the component is `DecalScatter3D` (header `decal_scatter.h`); `projects/`, `src/`, `docs/` and `tests/` have no type or word of that name (checked 2026-11-03).

**Goal.** An opt-in kit component `DecalScatter3D` spreads `count` decals over a rectangle at its entity (x, z half extents, the decals project down local -Y like `Decal3D`) deterministically from a `seed`. Each decal gets a position, a yaw, a size between `size_min` and `size_max`, a colour tint and one cell of an atlas texture (`atlas_cols` x `atlas_rows`, so one texture gives variety and the 3-texture limit is never an issue). Pure helpers compute decal `i` from `(seed, i)` only, with fixed evaluation order and no `rand`. `Kit3DExtract` adds each as a **local `render::Decal`** through `scene.add_decal`, the same path as `Decal3D`, nearest first up to a budget, and fades them out towards `fade_distance_m` so far decals cost nothing. No shader, pass, `Scene3D` field or system state; nothing is written or hashed. A scene with no `DecalScatter3D` (or `enabled` 0, `count` 0, `strength` 0) is bit-identical.

**Not in scope:** a shader or a new `render::` field, decals that follow terrain height (they project onto whatever is under the box, as `Decal3D` does), road-aligned placement (roads belong to M-OPENWORLD), skid marks from vehicles (gameplay-driven), procedurally generated textures (the author supplies the atlas), raising `kMaxDecals` or `kMaxDecalTextures`.

## Design

- **Opt-in component, no system, no state.** `DecalScatter3D` (`mce/kit/decal_scatter.h`, any number of entities, all read, registered by `install_stock` next to `Decal3D`). Authoring (all `Save`, with a `Range`): `enabled` 1; `texture` (atlas, rgb colour, a coverage); `atlas_cols`, `atlas_rows` 1..8; `count` 0..256; `area` (x, z half extents in metres); `depth_m` projection depth; `size_min`, `size_max` (metres); `seed`; `color` and `tint_jitter` 0..1; `roughness`, `metallic`, `material_weight`; `angle_fade`, `edge_fade`; `fade_distance_m`; `strength` 0..1 (opacity scale); zeroed `pad_`.
- **Pure helpers (35a).** `decal_scatter_item(const DecalScatter3D&, u32 i) -> ScatterDecal` (local offset, yaw, size, atlas cell, tint) from an integer hash of `(seed, i)`; `decal_scatter_fade(distance, fade_distance_m) -> 0..1` (smooth, 1 near, 0 at the limit); NaN and inverted ranges fall back to the defaults.
- **Extract (35b).** After the `Decal3D` loop in `kit3d_extract.cpp`: for each scatter, compute the camera distance of the patch, skip it when beyond `fade_distance_m` plus the patch radius, else emit up to `scaled_budget(count)` items (the Low tier halves it), nearest to the camera first so the 64-decal cap drops the far ones; each item's opacity is `color.a * fade * strength`. Counted in `stats_.decals`.
- **Luau (35c).** `scene3d`-style access is not needed: `decal_scatter` has no runtime state, so 35c adds the `samples/` placement and a headless pin on the `decals` stat of one sample whose ground is bare (chosen then, outside modules other threads own), plus the guide. If no sample fits without crossing an owner, 35c becomes a test-only step.
- **Determinism.** Decal `i` depends on `(seed, i)` and the component only, so adding or removing a scatter never changes another; the same scene gives the same stains on every machine.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 35a. Component and pure helpers | `DecalScatter3D`, registration, zeroed pad and `test_padding` entry, the three `engine_components.cpp` includes, the `capabilities` row and the `components` bump in the MCP transcript golden, guide paragraph; `tests/kit/test_decal_scatter.cpp` | kit | Sonnet |
| 35b. Extract feed | the local decals with the distance fade, nearest-first budget, Low tier; `tests/kit/test_decal_scatter_extract.cpp` | kit | Sonnet |
| 35c. Sample and pins | one bare-ground sample scene gets a scatter and a pin on the `decals` stat; if none fits without crossing an owner, a test-only step | samples or tests | Sonnet |
| 35d. Docs and close | the `08-3d` paragraph, milestone status, follow-ups, ROADMAP row, changelog | docs | Sonnet |

No step needs Opus: nothing writes a shader; the decal pass already exists.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 35a | planned | | |
| 35b | planned | | |
| 35c | planned | | |
| 35d | planned | | |

## Follow-ups

- Terrain-height aware placement once M-TERRAIN exposes a height query.
- A road-edge preset once M-OPENWORLD's road network settles.
- A real GPU/windowed look at stain density and the fade distance.
