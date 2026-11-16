- `render::build_merged_mesh`, M-VISUAL3 step 5b (SBA-199): merges many different rigid meshes into one cooked mesh
  payload (`render/static_merge.h`) with one submesh per material, placed by their world matrices, quantized over one
  shared box so the geometry stays watertight across materials, per-level LOD union and 32-bit indices past 65535
  vertices. Blended, transmissive, parallax and skinned sources are left out and counted in `MergeStats`. New golden
  `static_merge_kit`.
