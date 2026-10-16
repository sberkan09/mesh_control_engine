# M-VIEWS — Several views and local players (milestone record)

**Status:** active (added 2026-10-16; design note written). Plan: ROADMAP §M-VIEWS; Linear SBA-161 … SBA-166.

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

## Steps

| # | Step | Linear | Model | State |
|---|---|---|---|---|
| 1 | Local players and per-slot input: slot maps (`MapFlag` bits 8..11), per-slot pads, join by button, `P2/` script names (the `LocalPlayer` component moves to step 5, where something uses it) | SBA-161 | Sonnet | done |
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
