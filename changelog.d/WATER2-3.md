- M-WATER2 49e-49f: the water can flow. A new opt-in kit component `WaterFlow` (`mce/kit/water_flow.h`: `enabled`,
  `dir_deg`, `speed` 0..5 m/s, registered by `install_weather`) gives the scene a uniform current: `Kit3DExtract` puts
  its velocity on `WaterSurface::flow` (scaled by the freeze look's speed, so a frozen river stops), and the renderer
  Doppler-shifts every wave and both detail ripples by it, rounded to whole cycles per 64 s so the waves still repeat
  exactly (no shader change). Kit helpers `water_flow_velocity`, `water_flow_cycles`, `water_wave_set_flow` and
  `water_height_at` / `water_normal_at` overloads taking a `const WaterFlow*` keep gameplay on the drawn surface. A
  scene without a `WaterFlow` hashes, saves and draws exactly as before.
