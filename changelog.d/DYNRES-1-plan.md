- M-DYNRES plan: dynamic resolution scaling (a controller that moves `render_scale` for the window views from the
  measured GPU frame time: 1/16 grid, cooldown, headroom, the tier's scale as the ceiling, TAA history kept over a step),
  `render::DynamicResolution`, `r.dynres`, `r.dynres.target_ms`, an opt-in `DynamicResolution3D` component with
  `kit3d.h` untouched (steps 46a to 46f); nothing is built yet.
