- M-CIRRUS plan and seam (step 53a): `docs/milestones/M-CIRRUS.md` plans a second, high, thin cloud layer of cirrus
  streaks above the M-CLOUDS layer, drawn in the sky shader. This step adds only the data: `render::Cirrus`
  (`coverage`, `density`, `scale_m`, `stretch`, `speed`, `wind_dir`, `height_m`, `enabled`; coverage 0 = off) with
  `Scene3D::set_cirrus()` / `cirrus()`, the quality field `QualitySettings::cirrus` (Low off), the saved convar
  `r.cirrus` and `apply_quality(quality, cirrus)` with clamping. Nothing draws or fills it yet; every frame is as before.
