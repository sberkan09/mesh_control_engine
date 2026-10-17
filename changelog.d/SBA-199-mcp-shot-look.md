- Per-shot camera look over MCP, M-VISUAL3 step 3c (SBA-199): `render_frame` / `render_sheet` take `camera.focus` (meters
  or `"target"`), `camera.exposure` and an `environment` object of `Environment3D` overrides, validated against the
  reflected fields. The player reads them as `r.dof_focus`, `r.exposure_scale` and `r.env_override` (parsed once per
  change). Nothing in the saved scene changes.
