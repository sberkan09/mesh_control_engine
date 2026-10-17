- Decal limit warning, M-VISUAL3 step 6 (SBA-199): when a frame drops visible `Decal3D`s (more than 64, or more than 3
  distinct textures) the player logs one warning naming both limits and the workaround (pack the textures into an atlas
  and give each decal its `uv_rect`): new `Kit3DExtract::note_built(const Scene3D&)`, which also fills
  `Kit3DStats::decals_dropped`. Nothing changes for scenes within the limits.
  `perf_run`, `.mcperf` captures and `perf_compare` inputs carry `decals_dropped` (older captures read 0).
