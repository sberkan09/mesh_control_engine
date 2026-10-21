- Wind direction in the extract, M-WINDDIR step 8d: `Kit3DExtract` sets `render::SnowCover::wind_dir` from the scene's
  `WindDirection` and rotates each `Foliage3D` / `TerrainFoliage3D` batch's `wind_direction` by the heading relative to
  +x when the scene has a `Weather` and a `WindDirection`. Batch fields only (no re-scatter); a scene without a
  `WindDirection` is byte-identical. Snowfall particles still fall as before.
