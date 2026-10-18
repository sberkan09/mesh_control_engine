- Wet surfaces draw, M-WETSURFACE step 5b: `Scene3D::set_wet_surfaces()` with a wetness above 0 darkens porous
  (rough, non-metal) albedo, lowers dielectric roughness toward `WetSurfaces::roughness` and, unless the tier drops
  them (Low), collects puddles on up-facing surfaces from world-anchored value noise (integer hashing, periodic over
  256 m so a floating-origin rebase keeps them in place): darker, roughness 0.03, the normal map flattened. Terrain
  takes the same path, unlit materials and metals' albedo do not; reflection probes and the irradiance volume capture
  the dry scene. Wetness 0 draws byte-identical frames (every existing golden unchanged); new golden `wet_plaza`.
  `apply_quality(q, wet)` now clamps wetness, puddles, darken and roughness to 0..1 and the puddle cell to 0.25..256 m.
  Nothing sets it from the weather yet (the `WetSurfaces` kit component is step 5c).
