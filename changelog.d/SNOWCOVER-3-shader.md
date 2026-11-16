- Snow cover draws, M-SNOWCOVER step 6b: `Scene3D::set_snow_cover()` with a cover above 0 whitens up-facing
  surfaces (ground, roofs, box tops, terrain, the top of a sphere) under a world-anchored noise mask (the puddles'
  integer-hash value noise, periodic over 256 m, so a floating-origin rebase keeps the patches in place): albedo goes
  toward 0.9 white, roughness toward 0.8, metallic toward 0, and the normal map flattens. `coverage` is the share of
  the ground that whitens at full cover, `noise_scale` the patch cell in meters, and `wind_bias` favors slopes facing
  downwind (+x, the way the weather's snow drifts) over windward ones. Snow wins over puddles and the wet darkening
  under it. The Low tier (`noise_enabled` false) draws an even layer of the same mean cover instead of the patches.
  Walls stay bare; unlit materials are untouched; reflection probes and the irradiance volume capture the snowless
  scene. Cover 0 draws byte-identical frames (every existing golden unchanged); new golden `snow_plaza`.
