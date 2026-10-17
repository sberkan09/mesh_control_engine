- Foliage variation and wind, M-VISUAL3 step 6 (SBA-199), renderer: `render::InstanceBatch` gains `tint_variation` (a
  copy's colour is `tint` x a mix from white toward it by a hash of the copy's origin) and `wind_ext` (wind height,
  gust, flutter); `Foliage3D` / `TerrainFoliage3D` feed them from `tint_variation`, `wind_height`, `wind_gust` and
  `wind_flutter`. With `wind_height` H the sway profile is `(h / H)^2`, so a 5 m tree no longer bends 25 times a 1 m
  bush; gusts travel along `wind_direction` at 3 m/s; flutter is 4-6 Hz leaf jitter per vertex. `Foliage3D.mesh_1..3`
  with `mesh_weights` scatter up to four kinds at once, one batch per kind (`scatter_foliage` itself is unchanged and
  the split is a hash of each copy's origin, deterministic in `seed`). Every vertex shader that feeds the main
  fragment shader writes a `v_tint` varying (white outside batches). All defaults take today's exact expressions: every
  existing golden is byte-identical. New golden `foliage_variation`.
