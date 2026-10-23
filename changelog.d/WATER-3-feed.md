- Water feed, M-WATER step 15b (kit half): `Kit3DExtract` fills `render::WaterSurface` from the first `Water`: the authored
  level, extent, waves, colours and foam, `time` as the value `set_time` gets in the same extract wrapped with
  `water_wrap_time` in f64, and the main direction from `wind_dir_deg` or, when `wind_follow` is above 0 and the scene has a
  `WindDirection`, from its heading; the result goes through `apply_quality` (Low flat and foamless, `r.water 0` no plane).
  Nothing is set without a `Water`, when disabled or with a non-finite level; `Environment3D`, fog, sky and the light are
  never touched. Nothing draws the plane yet (step 15c).
