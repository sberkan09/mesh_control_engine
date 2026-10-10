# ADR-0022 — glTF static import and the `CHNK_MESH` format, as built (M4)

**Status:** Accepted (2026-10-08).

**Context.** ROADMAP M4 opens with "glTF 2.0 static import (meshoptimizer vertex-cache/fetch at cook, quantized
streams)". `assets.md` §5.2 sketches the mesh stage (meshopt optimize → snorm16 positions, octahedral normals,
unorm16 UVs → `CHNK_MESH` with bounds, sub-mesh ranges and a material) and §2 lists node-transform clips as
`CHNK_ANIM` for the diorama's animated prop. This ADR records what was built and where it departs.

**Decisions.**

1. **One `gltf` importer for `.glb` and `.gltf`; one `CHNK_MESH` per file.** The chunk holds the whole default scene:
   the node tree (depth-first, parents first, TRS rest pose), one submesh per triangle primitive (node-local
   geometry plus the node that places it) and the file's animation clips. The renderer composes node world matrices
   with the instance transform, so a static kit piece and an animated prop are the same asset type.
2. **Clips ride in `CHNK_MESH`, not a separate `CHNK_ANIM`.** At MVP a clip only animates nodes of the file it came
   from; a separate chunk would need a second asset per file or secondary-chunk resolution in the asset system for
   no gain. Channels are translation/rotation/scale with LINEAR or STEP keys; CUBICSPLINE imports as LINEAR over its
   values with a warning. When retargeting or shared clips arrive (v0.5) clips move to `CHNK_ANIM` and this layout
   becomes the in-file special case.
3. **Materials are inline in each submesh** (the 112-byte `MaterialChunk` from `.mcmat`): factors, alpha mode,
   double-sided, unlit, and the base color texture by GUID. `assets.md` §5.2 has glTF import *extract* `.mcmat`
   files on first import; that writes into `assets/` from inside a pure cook stage, so it moves to the editor's
   import action (M6, with the asset browser). Normal/ORM/emissive maps are v0.5 (PBR, M5a) and warn.
4. **Buffers must live in the file** (GLB binary chunk or base64 `data:` URIs). An importer only sees its own
   source bytes (cook purity, FR-AST-030); a sibling `.bin` would be an untracked input. The error says to export
   `.glb`. **Images must be external, registered texture assets**: the URI resolves relative to the glTF to a path
   under `assets/`, the path to the texture's GUID (`IAssetLookup::guid_for_path`), and the GUID becomes a typed
   dependency (cook order + cache key). Embedded images are an error.
5. **Quantization.** Positions are snorm16 relative to each submesh's own bounds (`p = center + q·extent`), normals
   octahedral snorm16 (4 bytes; 256× finer steps than snorm8), UVs unorm16 over the
   submesh's UV rectangle; the vertex is 16 bytes. Indices are u16 unless a submesh exceeds 65,535 vertices.
   `KHR_texture_transform` is baked into the UVs at cook.
6. **meshoptimizer v0.25** (FetchContent, ADR-0017) compiles only `indexgenerator`, `vcacheoptimizer` and
   `vfetchoptimizer` into `mce_meshopt`, linked privately by `mce_asset_cook`; the "meshopt wall" in
   `check_walls.py` keeps it out of runtime targets (FR-AST-010).
7. **Scope lock (`assets.md` risk 2).** Supported extensions: `KHR_texture_transform`, `KHR_materials_unlit`,
   `KHR_materials_emissive_strength`. Any other *required* extension is an error naming it; merely *used* ones warn.
   Skins, morph targets, cameras and lights are ignored with a warning. Every error names the file and the JSON
   pointer of the offending element (FR-AST-040's "file + line" for a binary-ish format).
8. **`MeshView::read` validates everything** — offsets, node parent order, channel ranges, and every index against
   its submesh's vertex range — because the payload is untrusted bytes and its indices reach the GPU.

**Consequences.** The Kenney CC0 kit `.glb` files (UnityGLTF exports with an external `Textures/colormap.png`) cook
unmodified: one sidecar per model and one for the shared colormap. Cook of the 17-model diorama kit takes ~0.3 s.

## Amendment 2026-10-11

§7 is superseded in part by [ADR-0036](0036-skeletal-skinning.md): skins are imported (glTF importer v3, the
`CHNK_MESH` minor-1 skin section). Morph targets, cameras and lights are still ignored with a warning.
