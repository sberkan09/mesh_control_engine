# SUMMARY: level

Agent **level** on Marble Garden (team-3d). The game is shipped: `dist/MarbleGarden-1.0.0-linux-x64.zip`.

## What I did
- **Plan (task 1).** Wrote `tasks.mctasks`: 14 tasks with role prefixes, `depends` and `files`, plus a shared contract in task 1's notes (scene paths, entity names, layout, input).
- **Garden blockout (task 2).** `scenes/garden.mcscene`, a 24 x 24 m walled diorama built from `gen_mesh` blocks: ground with a pond basin (water plane, `Pond` Trigger3D), stone walls, NW and SE raised terraces with ramps (MeshCollider3D), five hedges, Environment3D and a CameraFollow3D camera.
- **Gameplay placement (task 10).** Marble with MarbleController (respawns at `Start`); 5 gems (two on terraces, two in corner pockets, one past the pond); Hazard on the pond; a `Game` entity with GardenGame; the HUD (timer and gem labels on art's panels).
- **Screens (task 11).** Title (`main.mcscene`, keeps the start GUID), `win.mcscene` and `lose.mcscene`: a 3D plinth diorama with spinning gems and a dark backdrop so the text has contrast, a 2D text overlay, a MenuScreen that goes to the garden, and a ResultLabel on win and lose.
- **Tests and goldens (task 12).** 10 `.mctest` files, all passing:
  - `collect_all_gems`: a scripted route that takes all 5 gems and reaches the win screen at tick 1700. I built the route with a closed loop: replay `run_headless` with `inspect` and steer towards waypoints.
  - `reach_gem_1/2/3/5` (walk_to bot), `pond_respawn`, `time_runs_out`, `title_to_garden`, `win_replay` and `lose_retry`.
  - Goldens: title, garden, garden_overview, win and lose.
- **Final (task 14).** Tests 10/10 after the polish pass. Moved the result text off the plinth and re-baselined the goldens. `project_check` passes with 0 errors and 0 warnings (2 infos about unused assets). Set the title to "Marble Garden", then `ship_build` succeeded.

## Friction notes filed (6)
1. Entity references in component values need a numeric `"$<id>"`; a name is refused.
2. A `render_frame` free camera with a negative x position silently falls back to the game camera.
3. The walk_to bot takes one target and has no waypoints.
4. `run_headless` resolves `inspect.entities` names in the open scene, not in the `scene` argument.
5. `.mctest` event checks need the namespaced name; an unknown name silently counts 0.
6. The project_check contrast check ignores the panel sprite behind HUD text (a false positive).

## Issues and fixes
- **Shared scratchpad.** My first helper scripts were in a scratchpad shared with another agent's session (solo-3d), whose `mce.py` replaced mine. For a few calls my edits went to that project and replaced its untouched template `main.mcscene`; that agent had already re-saved it when I found it. I then moved my scripts into `agents/level/work/`.
- **Found for code.** The round-over log line printed its raw printf format (task 15); code fixed it.
- Nothing blocked me.
