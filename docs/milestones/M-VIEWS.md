# M-VIEWS — Several views and local players (milestone record)

**Status:** done 2026-10-16 (added 2026-10-16; design note written; steps 1-6 done). Plan: ROADMAP §M-VIEWS; Linear SBA-161 … SBA-166.

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
- **Local players (as built, step 1).** A map flagged for slot N is always active and publishes
  `player_action_id(N, name)`; gamepad bindings read the slot's assigned pad; scripts spell `P2/Move`.
  The `LocalPlayer` component `{slot}` arrives with the co-op kit (step 5).
- **Model split.** Steps 2–4 change the render pipeline and per-frame GPU resources: Opus. Steps 1, 5 and 6
  follow existing patterns (input maps, kit components, MCP tools): Sonnet.

## Interfaces as built (steps 2–4; steps 5–6 build on these)

**Step 2 — viewports, several cameras a frame** (render/forward3d.h, render/renderer.h, kit/views.h):
- `kit::CameraView {viewport vec4 (x, y, w, h; 0..1, top-left), target Entity, order i32, enabled u32}` on a
  camera entity (Camera3DComponent or Camera2DComponent). No enabled CameraView anywhere = today's path,
  untouched (the golden gate). With one: every camera with an enabled CameraView is a view, plus each
  primary camera without one (full window, order 0), so a minimap needs only its own CameraView.
- `kit::collect_views(world, window, out, target_size?)` -> `ViewEntry {camera, target, rect (pixels),
  order, is_3d}` sorted by (texture targets first, target, order, 3D before 2D, split player (step 4), entity index). Rects round
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
- Known limits (step 2): 2D post (`Post2D`) is off in the
  multi-view path; GPU timestamps and `last_stats()` describe the first 3D view; per-entity LOD
  hysteresis is shared by the views; a particle emitter only the later views see is not simulated.

**Step 3 — render textures** (kit/render_textures.h; changes from the design note marked *):
- `kit::RenderTexture {width, height, update_hz, enabled, ui_image}` on its own entity. *No `format` (always
  linear RGBA16F) and no `camera` field: a camera points at the texture with `CameraView.target`, so one
  texture can hold several cameras' rects.
- *`kit::ShowRenderTexture {source, slot, intensity}` instead of a `TextureRef` type: on a `Sprite` it
  replaces the sprite's texture; on a `MeshRef` it replaces submesh 0's material (base color for slot 0,
  emission times `intensity` for slot 1) on every submesh. A `.mcui` image widget whose name equals
  `RenderTexture.ui_image` shows it (`ui::Document::set_image_texture`, `player::UiScreens::bind_render_textures`).
- `kit::RenderTextures` (host-owned, like `VideoTextures`): `sync(world, frame_dt)` once a frame makes,
  re-makes (size change) and frees the textures and decides which are due (`update_hz`); `release()` on the
  editor's play stop, `shutdown()` with the host. *Textures are made on the device and adopted by the
  `TextureStore` under a local key (`TextureStore::adopt`), not taken from a transient pool; freeing goes
  through `release(key)`, which the RHI defers past the frames in flight. Creation and resize allocate;
  steady frames do not.
- Drawing: `collect_views(..., &kit::render_texture_size, &rts)` sorts texture views first;
  `record_views` clears a due texture once (black), draws its views with `ViewOutput::linear_output` /
  `record_2d(..., linear_output)` (no sRGB encode; a 2D camera's clear color is linearized so it shows as
  in the window), then the window's views. A texture no view has drawn yet is cleared black once
  (`RenderTextures::clear_new`, which the single-view paths call too).
- Orientation (fixed in step 4): a render texture is written upright, its top row at v = 0 like uploaded
  art, so sprites, materials (every map of the screen material) and UI sample it unchanged. A drawn frame
  lands with its top row at v = 1 on both backends, so `Renderer::record_3d` with `linear_output` draws
  the 3D view into the view slot's staging texture and copies it into the rect with a v-flipped quad (one
  RGBA16F staging per slot, freed with the slot); a 2D view into a texture mirrors its clip-space y.
- *No `project_check` cycle error: a view never samples the texture it draws (that surface falls back to
  its own look), and two textures that show each other show each other's previous frame.
- Known limits (step 3): the anti-aliasing filter of a 3D view into a texture runs on linear values (edges
  differ by a few levels from the window, about 1 % of the pixels); on the single-view debug paths (`-view.*` overrides, `-ui_preview`) textures keep their last
  picture; the editor counts `update_hz` at 60 frames a second.

**Step 4 — split-screen** (kit/views.h, audio/components.h, player/ui_screens.h):
- `CameraView.split` (u32, 1..4; the component grows to 48 bytes with explicit zeroed padding): the
  window view of player n. The cells are the distinct `split` values in use, ascending, so the layout
  follows the players present (1 and 3 present: halves; one leaves: the rest reflow). `viewport` is a
  part of the cell, so a per-player HUD or minimap camera shares the player's `split` with a higher
  `order`. `ViewEntry` gains `split` and `cell`; views sort by split player after 3D-before-2D.
- `kit::SplitScreen {layout (Auto, Columns, Rows, Grid), divider px}`, one per scene; `kit::split_cell
  (layout, count, index, aspect)`. Auto: two side by side (stacked on a portrait window), three with
  player 1 on top, four in a 2 x 2 grid.
- Budgets: `kit::scale_view_quality(scene, area)` runs in `record_views` (ViewRecordContext::scale_quality,
  on by default): a view of at most half the window draws two shadow cascades at most; at most about a
  third also drops SSR, contact shadows, volumetrics, DOF and motion blur. Full-window views are untouched
  (the golden gate). On llvmpipe a four-way split of kit3d_tour took 1.3x the wall time of one view for
  120 frames (startup included); a real-GPU budget waits on a device session (untested).
- Per-view HUD: `kit::UiScreenView {camera}` beside a `UiScreen`. The player collects the views before the
  UI update (`UiScreens::set_views`), lays the document out in that camera's window rect at
  `compute_ui_scale(rect)`, keeps the safe insets only on window edges, moves the pointer into the rect
  (outside it reaches nothing) and draws each such screen in its own pass under the window-wide overlay.
- Audio: `audio::AudioListenerMix {mix}` with an `AudioListener` per player camera: `ListenerMix::Nearest`
  spatializes each 3D voice against the nearest listener, `Mixed` takes the loudest gain and the
  gain-weighted pan; `AudioDevice::set_listeners` (up to `kMaxListeners` = 4). No component (or mix 0)
  keeps one listener and the old command stream.
- Known limits (step 4): the editor's play view lays UiScreenView screens over the whole view (the
  editor does not draw UiScreens per view); Doppler and occlusion follow the first listener; three
  players in the Grid layout leave the fourth cell to the window's clear color.

**Step 5 — the co-op kit** (kit/coop.h, kit/src/coop.cpp; changes from the design note marked *):
- `CoopGame {player (prefab), join/leave/move/jump_action, max_players, auto_join, tint_players, enabled}` (one per
  scene), `PlayerSpawn {player}`, `LocalPlayer {player, drive, speed}`, `PlayerCamera {player, offset, smoothing}`,
  `SharedCamera2D {margin, min_width, min_height, smoothing}`; `install_coop` is part of `install_stock`.
  *`LocalPlayer.player` is 1..4 (the script name `P2/…`), not a 0-based `slot`.
- `kit_coop` (FixedUpdate_Pre): leave, join (the first `auto_join` slots once, the rest on their `Join` edge), spawn at
  the player's `PlayerSpawn` (else any free spawn, else a row). The prefab is instantiated with overrides on
  `LocalPlayer.player` and `LocalTransform.position`; without a prefab a bare `LocalPlayer` + `Sprite` is made.
  Drive: `CharacterController3D.move` / `jump` from the player's Move / Jump, or `World2D::set_linear_velocity` for
  a non-static `RigidBody2D`. `kit_coop_cameras` (FixedUpdate_Post): `PlayerCamera` follows and toggles its
  `CameraView.enabled`; `SharedCamera2D` frames the players' box (both fits set).
- `.mcinput` maps take `"player": 1..4` (`map_player_flags(player-1)`); the player calls
  `input.set_join_slots(0, kit::coop_join_slots(world))` each tick, so a pad's Start joins any free slot that the
  scene allows.
- Components are in `mce_cook`'s and the editors' `engine_components.cpp` (a scene that names an unregistered
  component loads without it, silently: register new components there).
- `samples/09_coop`: data only; four-way split, prefab players, a 10 Hz minimap `RenderTexture` on a monitor sprite;
  headless `coop_sample_player` runs `input.txt` (P2..P4 join, P3 leaves) twice with the same hash. The headless
  entity count is the scene's (18), not the live world's.
- Known limits: a 3D player needs a `Camera3DComponent` per `PlayerCamera` (the offset is yours); two players on one
  pad is not possible; the player count is not part of any resource hash (the entities are).

**Step 6 — MCP and gates** (editor/mcp/src/tools_views.cpp, tools_render.cpp):
- `view_list` (camera views, `SplitScreen`, textures with their cameras and the surfaces that show them, `problems`),
  `view_add` (a `RenderTexture`, its camera and an optional `ShowRenderTexture` in one call), `player_list` (`CoopGame`,
  per player spawns, cameras and the `.mcinput` maps with `"player"`, `problems`), `player_setup` (a whole co-op rig).
  They edit the open scene through `EditorScene` (undoable, like `entity_create`), read the project's `.mcinput` files for
  the map check, and need no GPU.
- `render_frame` (and `render_sheet`, `image_diff`'s `render`) take `view`: `{"player": n}` draws the camera of local
  player n alone and full size (the entity with a `PlayerCamera` for them, else the `CameraView` whose `split` is n),
  `{"texture": entity}` the camera that fills that `RenderTexture`; it is the existing `camera` override resolved from the
  scene (not with `camera`). Without `view` the frame is the window as the players see it, split cells included. Join the
  players through `input` (`"5 P2/Join"`): their cameras follow their players from the tick they join.
- Found by looking at the demo frame (software render_frame): a 2D view that did not own its whole target cleared it, so in
  a split screen only the last cell survived (GL clears ignore the viewport; SDL GPU clears the attachment). `record_views` now
  loads for such a view (a split cell, a second camera into a texture) and fills its rect with its camera's clear color
  (`SpriteExtract::add_fill`, a solid quad under everything; a clear color with alpha 0 is an overlay camera and fills nothing).
  `tests/kit/test_kit_gl.cpp` has the GL case; `tests/mcp/test_mcp_agent.cpp` checks the four cells of `samples/09_coop`.
- Also fixed: the co-op spawn is deferred inside a system phase, so the player's tint is a prefab override, not an edit of the
  spawned entity (which did not exist yet).
- Gates: the MCP transcript golden and `docs/engine/capabilities.md` regenerated; user guide 8.4, 11.3, 14.2, 18 updated.
  GPU frame budgets of a four-way split wait on a device session (the `coop_sample_player_gpu` test is the hook).

## Steps

| # | Step | Linear | Model | State |
|---|---|---|---|---|
| 1 | Local players and per-slot input: slot maps (`MapFlag` bits 8..11), per-slot pads, join by button, `P2/` script names (the `LocalPlayer` component moves to step 5, where something uses it) | SBA-161 | Sonnet | done |
| 2 | Camera viewports and targets, several cameras per frame | SBA-162 | Opus | done |
| 3 | `RenderTexture`: minimap, mirror, monitor, picture-in-picture | SBA-163 | Opus | done |
| 4 | Split-screen: layouts, per-view HUD, listener, budgets | SBA-164 | Opus | done |
| 5 | The co-op kit and `samples/09_coop` | SBA-165 | Sonnet | done |
| 6 | MCP `view_*` / `player_*`, `render_frame` view argument, guide and gates | SBA-166 | Sonnet | done |

**Gates:**
- One camera and no `RenderTexture` draws the same pixels as before (GL and SDL GPU goldens).
- The sim hash is independent of the number of views and of the split layout.
- Single-player input and the existing samples behave as before.
- `local_ci.sh` green on every step.
