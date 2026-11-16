- Caustics kit component, M-CAUSTICS step 52c: the opt-in `Caustics3D` (`mce/kit/caustics.h`: `enabled`, `strength` 1
  (0..4), `scale` 2 m (0.25..16), `speed` 1 (0..4), `absorption` 0.15 per m (0..4), `max_depth` 30 m (0.5..200);
  explicit pad, no system, nothing stored or hashed), registered next to `CloudShadow3D` in `install_kit3d` and in the
  editor, `mce_mcp` and `mce_cook` component lists. `Kit3DExtract` feeds `PostFx::caustics` from the first enabled one by
  entity index through `apply_quality` (so the Low tier, `r.caustics 0` and the clamps apply, NaN falls back to the
  default), and only in a frame whose extract set an enabled water plane (a `Water`, enabled, with a finite level, and
  `r.water` on); the water plane, the sun and the clock come from the scene itself. A scene without the component, with
  `enabled` 0 or without water renders and hashes as before.
