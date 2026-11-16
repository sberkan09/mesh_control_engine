- M-CAUSTICS plan and seam (step 52a): `docs/milestones/M-CAUSTICS.md` plans animated sun caustics, the moving web of
  refracted light on whatever lies below a water plane, as a post pass on the HDR frame (own shader, the lit shaders are
  not touched). This step adds only the data: `render::Caustics` in `PostFx` (off by default), the
  `Forward3DStats::caustics` fields, `QualitySettings::caustics` / `caustics_octaves` (off on Low, 2 octaves on Medium,
  3 on High), the saved cvar `r.caustics` and the `apply_quality` clamps. Nothing draws yet; every frame is as before.
