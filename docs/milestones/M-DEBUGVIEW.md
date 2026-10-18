# M-DEBUGVIEW — Seeing the gameplay systems: one debug-line layer (milestone record)

**Status:** planned 2026-10-18. **Owner: the "Geliştirme rutini 3" thread** (taken after M-FUZZ, at the coordinator's request: pick an unowned, device-free gap from the assessments). No Linear issues (Linear is unreachable and the workspace is at its free issue limit); this record is the tracker.
Source: `assessments/unreal-kiyaslama-2026-10-16.md` ("Hata ayıklama ve ölçüm: Kısmen — Unreal Insights, Gameplay Debugger"). The AI-agent side is the engine's edge, and an agent cannot see a collider, a joint, a waypoint chain or a nav path: `run_headless` `inspect` gives numbers and `render_frame` draws the meshes, not the systems.

**What exists.** Three unrelated line producers: `perception_debug_lines` (`kit/perception.h`, the editor overlay and an MCP tool), `road_debug_lines` (`kit/roads.h`, the editor overlay), and `debug_draw_grid` / `debug_draw_agents` (`kit/pathfinding.h`, 2D, into a physics `DebugDrawSink`). 3D physics (colliders, joints, vehicles, characters, nav agents, waypoint paths) has none.

**Goal.** One data call, `gameplay_debug_lines(world, categories)`, returns the lines of every gameplay system in a scene, by category, as plain data. An agent reads them over MCP as a list and as a picture (a CPU top-down or side raster, no GPU); the editor viewport draws them with the overlay path it already has for perception and roads; a game can switch categories on with a convar. Scenes and hashes do not change: it only reads.

**Not in scope:** a GPU line pipeline in the renderer (the editor overlay path stays the drawing route), 2D physics shapes beyond what `DebugDrawSink` already draws, profiler overlays (M-PERF owns them), editing through the debug view.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 1. Line producers | `mce/kit/debug_lines.h`: `GameplayLine {a, b, color, category}` and `gameplay_debug_lines(World&, mask, Array<GameplayLine>&)` for box and mesh colliders, `RigidBody3D` shapes, `Trigger3D` boxes and spheres (lit when `inside`), `Joint3D` anchors and axes, `CharacterController3D` capsules (crouched height, grounded colour), `Vehicle3D` wheel positions and skidding wheels, `Waypoint3D` chains with `VehicleDriver3D.target`, `NavAgent3D` paths. Numbers in headless tests | kit | Sonnet |
| 2. MCP | `debug_lines` tool (the list, per category counts, `bounds`) and `debug_view` (a PNG: top-down or side projection of the lines over the scene bounds, CPU raster, lines only) so an agent sees what the systems are doing; `inspect`-style entity filter | editor/mcp | Sonnet |
| 3. Editor and player overlay | the viewport overlay draws the categories (the perception / road overlay path), a "Gameplay debug" panel toggles them; `-r.debug_gameplay <mask>` for the player when the build has a line path | editor/layer, player | Sonnet |
| 4. Docs, demo, close | guide chapter, `samples/` reuse (the 07 and 08 scenes' debug views as golden pictures), the milestone closed | docs | Sonnet |

## Gates
- Every step: `local_ci.sh --task` green; each producer has a headless test with exact numbers (a box collider gives 12 edges at its half extents, a trigger lights when something is inside, a waypoint chain closes its loop).
- The call only reads: replay hashes of every sample are unchanged; no new component and no change to `kit3d.h` (116 TUs); the new header is small and included by few files.
- The picture is a pure function of the world (same scene, same bytes), so `debug_view` has a golden.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 1 | in progress | | |
| 2 | planned | | needs 1 |
| 3 | planned | | needs 1 |
| 4 | planned | | |
