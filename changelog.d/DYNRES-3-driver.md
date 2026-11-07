- Dynamic resolution driver and component, M-DYNRES steps 46c and 46d: `Forward3D` now runs the controller once a frame
  on the frame's first view (`update_dynres`): the sample is the injected cost (`r.dynres.inject`) or the newest
  completed GPU timestamps (the first 3D view's span times the frame's 3D window views), matched in a 4-entry ring with
  the scale its frame drew at; window views draw at the controller's scale, render-texture views keep the tier's fixed
  scale, and a scale-only target change keeps the TAA history. `Forward3DStats::render_scale`, `dynres` and
  `dynres_cost_ms` are filled; each scale change logs one `dynres: scale A -> B (cost C ms, target T ms)` line; a
  malformed `r.dynres.inject` warns once and falls back to the measured time. Opt-in `DynamicResolution3D` component
  (`mce/kit/dynamic_resolution.h`: `enabled`, `target_ms`, `min_scale`, `max_scale`, `headroom`; `kit3d.h` untouched),
  fed into `PostFx::dynres` by the 3D extract (first enabled by entity index, through `apply_quality`). Capture runs of
  the player (`-capture`, `-capture_tick(s)`: goldens, `render_frame`, `render_sheet`) hold the tier's scale unless
  `-r.dynres.inject` is set. Off (no component, `enabled` 0, `r.dynres 0`) every frame is unchanged, and with the
  controller idle at the ceiling too.
- M-DYNRES 46c review: a frame whose GPU timestamps come from a render-texture view (drawn first, at the tier's fixed
  scale) is no longer a dynamic resolution sample; a capture run keeps the controller on only for an
  `r.dynres.inject` schedule that parses, and says so when it overrides `-r.dynres 1`.
