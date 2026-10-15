# M-TERRAIN — Heightmap terrain (milestone record)

**Status:** in progress (steps 1 and 2 done, 2026-10-15). Plan: ROADMAP §M-TERRAIN; Linear SBA-89 … SBA-94.
**Why:** SolsticeReach's island is a script-generated mesh in 16 chunks; there is no terrain an editor or an
agent can shape. Each step is opt-in: a scene without a `Terrain3D` draws exactly as before.

## Steps

| # | Step | Linear | State |
|---|---|---|---|
| 1 | `Terrain3D` + `.mcterrain`, CDLOD-style LOD, normals, skirts | SBA-89 | done: GL and SDL GPU (gated), tier budgets below; Metal untested |
| 2 | Splat layers (8), triplanar, height blend | SBA-90 | done: GL and SDL GPU (gated), tier knobs below; Metal untested |
| 3 | Jolt height-field collision, holes | SBA-91 | not started |
| 4 | Editor brushes with undo, cook round-trip | SBA-92 | not started |
| 5 | Foliage and grass painting | SBA-93 | not started |
| 6 | MCP sculpt/paint, import, generator, navmesh bake; demo | SBA-94 | waits on M-GAMEPLAY3D step 1 (SBA-69) for the navmesh |

## Step 1 — terrain asset, LOD and drawing

**Asset.** `CHNK_TERRAIN` (`asset::Terrain`, `TERRAIN_VERSION` 1, `mce/asset/formats.h`): a 96-byte
`TerrainHeader` (resolution 2^n+1 in 33…4097, chunk quads per node 8…256, size in metres, height range)
and `resolution²` u16 samples, z rows of x columns, centred on the entity. `TerrainView::read` validates
the payload; `height_at` is bilinear and clamped. The source is a native JSON `.mcterrain` (importer
`terrain`): `resolution`, `chunk` (default 32), `size`, `height_range`, then either `heights` (base64 of
little-endian u16) or `fill` (a constant height). Bad documents are cook errors naming the file. MCP
`asset_schema` / `asset_create` know the kind (template: 129 samples over 128 m).

**LOD (`mce/render/terrain.h`).** `TerrainLod` builds a quadtree of grid nodes: each node is `chunk × chunk`
quads at stride 2^level, root first. A node's geometric error is the largest height difference between the
full-resolution surface and the node's triangles (same diagonal split as the mesh), raised to at least each
child's. `select()` walks from the root and splits while `error · k > pixel_error · distance`
(`k = proj[1][1] · viewport_h / 2`), culling nodes outside the frustum unless they are within
`shadow_reach` (so shadow casters behind the camera stay). `build_mesh()` writes a node as a cooked mesh
payload: central-difference normals, UVs at `uv_tile` metres per repeat (or once over the terrain), and
skirts on edges inside the terrain, deep enough to cover a neighbour two levels coarser.

**Drawing.** `Terrain3D` (kit3d, 80 bytes, reflected): terrain, material, tint, pixel_error (0: the quality
tier's `terrain_pixel_error`, Low 4 / Medium 2 / High 1), uv_tile, cast_shadows, enabled. `Kit3DExtract`
keeps one LOD tree per terrain, builds and uploads a node's mesh the first time it is selected
(`MeshStore::allocate_key`, keys from 0x80000000), and evicts nodes unused for 300 extracts; the tree is
rebuilt when the asset generation or uv_tile changes. Stats: `terrain_nodes`, `terrain_triangles`,
`terrain_uploads`, `terrain_resident`; the player's stats line shows `TERRAIN nodes/resident`.
`MeshStore::get` became O(1) (a newest-per-key map) since terrains add many keys.

**Budgets (gate).** 513 samples over 512 m with 3 m bumps every 20 m (a worst case), camera 2 m above a corner looking across it, 1280×720, 60 m shadow reach
(`tests/render/test_terrain.cpp`):

| Tier | pixel error | Budget nodes / triangles | Measured |
|---|---|---|---|
| Low | 4 | ≤ 64 / ≤ 150 K | 52 / 118 K |
| Medium | 2 | ≤ 96 / ≤ 220 K | 83 / 189 K |
| High | 1 | ≤ 144 / ≤ 330 K | 124 / 284 K |

**Tests.** `test_cook` (cook, defaults, nine bad documents, payload validation), `test_render` (tree, errors,
selection coverage, culling and shadow reach, transform invariance, mesh and skirt validity, budgets),
`test_kit` (upload, reuse, rebuild, release, const store), `test_render_gl` / `test_render_sdlgpu`
(mixed-level nodes with T-junctions show no crack from a close oblique view; checked sensitive: 55 crack
pixels with skirts removed), and `terrain_player_gl` / `terrain_player_sdlgpu` (a cooked project drawn
by the stock `mce_player`, 0 GL errors, ≥ 10 K triangles).

## Step 2 — splat layers

**Asset.** `TerrainHeader` gained `layer_count`, `splat_resolution` and the offsets of a `TerrainLayer`
table (a `.mcmat` GUID, tile metres, height strength, triplanar flag) and the weights: two RGBA8 planes
(layers 0–3, 4–7) of `splat_resolution²`, the layout a 2-layer texture array uploads as is. The
`.mcterrain` source adds `layers`, `splat_resolution` (default `min(resolution, 1025)`) and `splat`
(u8 weights per layer per sample); without `splat`, each layer's `auto` rule (height and slope bands with
soft edges) paints it at cook, unclaimed ground going to layer 0. Layer materials are cook dependencies.

**Drawing.** `TerrainShading` (scene3d.h) on a `MeshInstance` switches its draws to `kTerrainFS`: the
forward shader with `MCE_TERRAIN`, whose surface comes from `terrain_surface()` instead of the material.
The layers' maps of one kind are layers of one texture array bound in the material slots (albedo 6, normal 7,
ORM 8), the weights in the emissive slot 13, the per-layer parameters in a `TerrainU` block on bank 2.
Layers tile in the terrain's own xz metres; triplanar layers add the x and z projections where the ground
is steep (whiteout-blended normal maps, no tangents); height blend lets the layer whose albedo brightness
rises highest win at a seam. The layer loop uses explicit gradients and skips layers under 1/500 weight.
`Kit3DExtract::set_texture_store` lets the extract pack each kind with `render::compose_layer_array`
(textures must share format, size and mips; a mismatch is left out with a warning) and upload the weights.

**Tiers.** Low blends by weight alone, Medium adds height blend, High adds triplanar
(`QualitySettings::terrain_height_blend`, `terrain_triplanar`).

**Tests.** `test_cook` (layers, weights, auto rules, ten bad documents), `test_render` (array packing,
mismatches), `test_render_gl` / `test_render_sdlgpu` (two layers split by their weights with and without
height blend, a base color multiplying its layer, a checker on a 3 m cliff: 0 light/dark flips down the
wall projected from above, 9 with triplanar), and the stock-player terrain project (grass on flats, rock on
slopes by `auto` rules).
