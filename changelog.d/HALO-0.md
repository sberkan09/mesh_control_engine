- M-HALO plan and seam (step 54a): `docs/milestones/M-HALO.md` plans ice-crystal halo optics on the cirrus layer: a
  22 degree ring and two sun dogs round the sun and a fainter ring round the moon, visible only through the cirrus
  and drawn in the sky shader. This step adds only the data: `render::Halo` (`strength`, `sun_dogs`, `moon`,
  `dispersion`, `enabled`; strength 0 = off) with `Scene3D::set_halo()` / `halo()`, the quality field
  `QualitySettings::halo` (Low off), the saved convar `r.halo` and `apply_quality(quality, halo)` with clamping.
  Nothing draws or fills it yet; every frame is as before.
