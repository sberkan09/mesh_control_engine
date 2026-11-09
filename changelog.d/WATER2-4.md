- M-WATER2 49g-49h: the current in Luau, and a river to see it all. The `water` table gains `water.set_flow{enabled=,
  dir=, speed=}` (clamped, named errors, nothing written on error, never adds the component), the getters
  `water.flow_enabled()`, `water.flow_dir()`, `water.flow_speed()` and the read-only `water.flow_at(x, z)` (the velocity
  `vx, vz` in m/s; `0, 0` with a `Water` and no `WaterFlow`); `water.height_at`, `normal_at` and `submerged` follow the
  flowing waves when the scene has a `WaterFlow` (bit for bit the kit's flowing helpers). New `kit3d_tour` scene
  `tour_river.mcscene` (from `samples/kit3d_tour/gen_river.py`, no new assets): a channel with a 1.2 m/s current,
  coloured pillars and an arch mirrored in the water and a striped seabed seen through it; tests
  `sample_kit3d_tour_river_headless` and `sample_kit3d_tour_river_gpu` (gpu, slow). The user guide documents
  `r.water.reflect`, `r.water.refract`, the reflection, refraction and current of the water (8.3, 8.6, 9.19l, 9.19m,
  27.3). M-WATER2 is closed.
