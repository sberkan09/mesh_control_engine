- **HLOD city sample (M-OPENWORLD A4).** `samples/hlod_city` builds a procedural city of streamed areas and extracts it
  without a GPU twice: every area loaded, then only the areas near the camera with `ChunkLod3D` proxies for the rest.
  It prints draws, instances, triangles and CPU time (8 x 8 areas: 0.21x the draws, 0.12x the extract + build time)
  and `ctest -R sample_hlod_city` fails unless HLOD submits under half the draws.
