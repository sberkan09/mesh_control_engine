- **Impostors for far areas (M-OPENWORLD A3).** `hlod_bake` with `impostor: true` also bakes a far stand-in per
  `SceneChunk`: each root entity of the area becomes a box with CPU-rendered albedo pictures on its four sides and top
  (tall, thin groups such as trees become crossed cards), packed into one atlas (`chunk_<id>_far.png`) beside the mesh
  (`chunk_<id>_far.mchlod`), and `ChunkLod3D.impostor` points at it. Deterministic, so re-bakes are byte-identical;
  `texels_per_metre` sets the sharpness. New `cook::bake_impostor` (`mce/cook/impostor.h`) and
  `kit::collect_chunk_sources`, which also places child entities correctly in the HLOD bake.
