- Wavefront OBJ import, M-IMPORT3D step 2 (GH-393): a `.obj` under `assets/` cooks to a mesh like a glTF: `v` / `vt` / `vn` / `f`
  (quads and n-gons fan-triangulated, negative indices), `o` / `g` nodes, `usemtl` submeshes, and the `.mtl` it names
  (`Kd`, `d` / `Tr`, `Ns`, `Pr`, `Pm`, `Ke`, `map_Kd`, `map_Bump`, `map_Ke`, `map_Pr` + `map_Pm`) as inline materials with
  textures resolved to GUIDs. Adopt the `.mtl` too (importer `mtl`). Sidecar `"up": "y"|"z"`, `"scale"`, `"max_triangles"`,
  `"lod_levels"`. Lines, points, curves, smoothing groups and vertex colours warn; the file is untrusted (finite numbers,
  bounds-checked indices, size caps naming the limit, errors as `file:line`). User guide 8.4, `docs/engine/asset.md`.
