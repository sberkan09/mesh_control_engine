# ADR-0040 — 3D navigation over embedded Recast & Detour

**Status:** Accepted (2026-10-15). **Milestone:** M-GAMEPLAY3D step 1 (off-tier, opt-in). **Plan:**
`docs/milestones/M-GAMEPLAY3D.md`. **Reference:** `src/engine/physics3d/include/mce/physics3d/navmesh3d.h`.

**Context.** The only pathfinding in MCE is the 2D grid A* of `kit/pathfinding.h` (issue #152). A 3D game needs
paths over arbitrary walkable geometry (ramps, stairs, decks over floors), agents that steer around each other,
areas that cost more to cross, links the mesh does not connect (jumps, ladders), and doors or crates that block
the way at run time. Every engine MCE was compared with (Godot 4, Unity 6, Unreal 5) ships this, and two of the
three ship it on Recast & Detour.

**Options weighed.**

| Option | For | Against |
|---|---|---|
| **Recast & Detour** (`v1.6.0`, zlib) | The industry standard (Unreal, Godot, many others); voxel bake from any triangles, tiled meshes, layered tiles with carved obstacles (DetourTileCache), crowds with velocity-obstacle avoidance (DetourCrowd); about 25 k lines of plain C++ with no dependencies, no exceptions, no RTTI, no threads | Floats throughout, so the bake and the crowd are deterministic per platform and binary, not across platforms (the same contract as Jolt, ADR-0003 §8) |
| Our own navmesh on top of the grid A* | No new dependency | A voxelizer, a region partition, a polygon mesh, a funnel, a crowd: months of work to reach what Recast already does well |
| A 3D grid (voxels) with A* | Simple, deterministic in integers | Memory grows with the volume; no smooth paths, no crowd; large levels do not fit |

## Decisions

1. **Recast & Detour, pinned `v1.6.0`, compiled into `mce_physics3d`.** Its Recast, Detour, DetourCrowd and
   DetourTileCache sources build as an object library (`mce_recast`, `cmake/Dependencies.cmake`) with warnings off
   and `-ffp-contract=off` (no fused multiply-add, ADR-0007), folded into the `mce_physics3d` archive like ENet into
   `mce_net`. No new library: the navmesh sits beside `World3D`, which is what the scene's colliders feed.
   DebugUtils and the demo are not built.
2. **Walls.** Recast and Detour headers and symbols only in `src/engine/physics3d/src/` (the "Recast wall"), and
   `mce_recast` only linked from `src/engine/physics3d/` ("Recast link wall"). `navmesh3d.h` names no Recast or
   Detour type: callers see MCE math types, area ids and agent indices.
3. **Tiled, layered bake; obstacles carve tiles.** The bake follows Recast's "temporary obstacles" pipeline: per
   tile (default 48 cells), rasterize the triangles touching it, filter, erode by the agent radius, mark area
   volumes, split into heightfield layers (one per storey) and store each layer. At load a `dtTileCache` builds the
   `dtNavMesh` tiles from the layers and rebuilds a tile when a box obstacle over it is added or removed. So
   doors, crates and drawbridges block paths without a re-bake, and a re-bake is never needed at run time.
4. **The bake is a byte blob (`.mcnav`).** Little-endian fields written one by one (no struct padding reaches the
   bytes; the tile-cache layer header is zeroed before it is filled for the same reason), layers compressed with
   PackBits run-length coding. Same geometry and settings give the same bytes on one platform and binary; a test
   bakes twice and compares. The blob is what a scene's navmesh asset holds, and what a runtime bake keeps in
   memory. Since blob version 2 (M-FUZZ 2b) the last 8 bytes are an FNV-1a 64 of every byte before them: Detour's
   tile cache trusts the layers, and a damaged byte made its contour builder divide by zero, so `navmesh_blob_valid`,
   `NavMesh3D::load` and the `.mcnav` importer refuse a blob whose hash does not match (a version-1 file is re-baked).
   The hash only catches damage, so `navmesh_blob_valid` also decodes every layer and refuses one Detour's builder
   cannot walk safely (bad layer header or tile coordinates, an area id above 63, a connection bit that leaves the grid, is not mutual or joins a non-walkable cell).
5. **Areas and costs.** Polygon areas are ids 0..31: 0 is not walkable, 1 is the default ground, 2.. are the game's
   with a cost per area (`set_area_cost`), shared by queries and the crowd. Bake inputs mark areas per triangle and
   with axis-aligned volumes; a volume of area 0 cuts a hole.
6. **Determinism.** The crowd and the tile cache run on the caller's thread with a fixed dt; the tile cache is
   brought fully up to date before every crowd step, so the order of obstacle requests never depends on timing.
   `NavMesh3D::hash_state` folds agent positions, velocities and states for replay checks.
7. **Not now.** Sliced (time-budgeted) path queries, per-agent filters beyond one crowd filter, cylinder obstacles,
   64-bit poly refs (worlds over 4096 tiles: raise `tile_size` or `cell_size`), and streaming tiles in and out.
