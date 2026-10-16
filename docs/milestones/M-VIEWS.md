# M-VIEWS — Several views and local players (milestone record)

**Status:** active (added 2026-10-16; design note written; step 2 in progress). Plan: ROADMAP §M-VIEWS; Linear SBA-161 … SBA-166.

**Why:** one camera draws into the window. `Renderer::record_3d(cmd, scene, target)` already renders to a
texture, but only a C++ host can use it. Input has no notion of a second player. A game cannot show a
minimap, a mirror, a monitor in the world, picture-in-picture or a split screen, and cannot be played by
two people on one machine, from scene data or over MCP.

**Not in scope:** online play (M-NET2/3), VR/stereo, portals with recursion, ray-traced mirrors, per-view
global illumination (the probe volume is shared by all views).

## Design note (interfaces; render steps wait for the coordinator's model decision)

- **View.** One view = camera entity + normalized `viewport` (x, y, w, h in 0..1) + `target` (`window` or a
  `RenderTexture` entity). The extract step builds a `ViewList` sorted by `(target, order, entity id)`;
  the order is part of the sim-independent render state, never of the sim hash.
- **Per-view state.** Cull, cascades, post chain and TAA history are per view. Shared across views: mesh and
  material uploads, the shadow atlas budget (split by screen area), the GI volume, the particle sim.
- **RenderTexture.** Component `{size, format, update_hz, camera}`; allocated from the renderer's transient
  pool, shown by a `TextureRef` that `Sprite`, a `MeshRef` material slot and a `mce_ui` image accept. A
  texture is drawn before the views that sample it; a cycle is a `project_check` error.
- **Fallback.** No `RenderTexture` and one camera takes today's path unchanged (golden-image gate).
- **LocalPlayer.** Component `{slot, device}`; `InputMap` evaluates per slot. `run_headless` takes
  `player=<slot>` on input scripts. Keyboard/mouse is slot 0; pads join by `join_action`.
- **Model split.** Steps 2–4 change the render pipeline and per-frame GPU resources: Opus. Steps 1, 5 and 6
  follow existing patterns (input maps, kit components, MCP tools): Sonnet.

## Interfaces as built (steps 2–4; steps 5–6 build on these)

**Step 2 — viewports, several cameras a frame** (render/forward3d.h, render/renderer.h, kit/views.h):
- `kit::CameraView {viewport vec4 (x, y, w, h; 0..1, top-left), target Entity, order i32, enabled u32}` on a
  camera entity (Camera3DComponent or Camera2DComponent). No enabled CameraView anywhere = today's path,
  untouched (the golden gate). With one: every camera with an enabled CameraView is a view, plus each
  primary camera without one (full window, order 0), so a minimap needs only its own CameraView.
- `kit::collect_views(world, window, out, target_size?)` -> `ViewEntry {camera, target, rect (pixels),
  order, is_3d}` sorted by (texture targets first, target, order, 3D before 2D, entity index). Rects round
  their edges, so split halves share an edge exactly.
- `kit::record_views(cmd, world, views, ViewRecordContext)`: clears the target once, then per view: 3D =
  its own `Scene3D` (`Kit3DExtract::extract` for the first view, `extract_view` for the rest: no second
  clock step, TAA previous transforms kept) recorded with `Forward3D::ViewOutput {slot, rect, first}`; 2D =
  a sprite batch in a pass whose viewport is the rect, cleared to its camera's color unless a 3D view
  covering its rect was drawn before it (a HUD camera). The player and the editor's play view both use it.
- `Forward3D` view slots (`kMaxViews = 8`): each slot owns its size-dependent targets and temporal state
  (TAA/SSR history, velocity previous view, auto exposure buffer, occlusion pyramid, volumetric froxels,
  post-fx ping-pong). Slot 0 is the members themselves (the single-view path); another slot is swapped
  into them for its record (handles only, no allocation). Shared: pipelines, meshes, IBL, probes, GI
  volume (rolling update on the frame's first view only), shadow atlas, particle rings (simulated on the
  first view only), instance batches (freed when no view drew them the previous frame).
- `Renderer::view_slot(key)` / `end_views()` / `release_views()`: a stable slot per view key (the camera
  entity's bits); a slot no key asked for since the last `end_views()` is released (its textures retired
  by the RHI past the frames in flight). The player calls `end_views()` every frame; the editor releases
  every slot when play stops.
- Known limits (step 2): views into textures are skipped until step 3; 2D post (`Post2D`) is off in the
  multi-view path; GPU timestamps and `last_stats()` describe the first 3D view; per-entity LOD
  hysteresis is shared by the views; a particle emitter only the later views see is not simulated.

## Steps

| # | Step | Linear | Model | State |
|---|---|---|---|---|
| 1 | Local players and per-slot input | SBA-161 | Sonnet | not started |
| 2 | Camera viewports and targets, several cameras per frame | SBA-162 | Opus | not started |
| 3 | `RenderTexture`: minimap, mirror, monitor, picture-in-picture | SBA-163 | Opus | not started |
| 4 | Split-screen: layouts, per-view HUD, listener, budgets | SBA-164 | Opus | not started |
| 5 | The co-op kit and `samples/09_coop` | SBA-165 | Sonnet | not started |
| 6 | MCP `view_*` / `player_*`, `render_frame` view argument, guide and gates | SBA-166 | Sonnet | not started |

**Gates:**
- One camera and no `RenderTexture` draws the same pixels as before (GL and SDL GPU goldens).
- The sim hash is independent of the number of views and of the split layout.
- Single-player input and the existing samples behave as before.
- `local_ci.sh` green on every step.
