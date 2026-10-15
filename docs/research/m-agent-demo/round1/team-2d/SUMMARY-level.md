# Glowmoth: summary from the level agent

## What I did
- Wrote the shared plan: 14 tasks, with dependencies, the files each task touches, and a contract naming the entities and components (Player, Lantern 1-3, Moon Gate, Bat N, Game, scene paths). A 15th task was added later for the scene-cycle problem.
- **Level 1** (`scenes/level1.mcscene`, easy, 72 m): a tilemap over static box colliders, one-way platforms, a shallow ditch and a 1 m hill, 3 lanterns on the main route, 2 slow bats and the moon gate leading to level 2. A walk_to bot finishes it in 16 s with 0 deaths.
- **Level 2** (`scenes/level2.mcscene`, harder, 96 m): 4 deadly gaps, step-ups that need a jump plus a flutter, lanterns on raised platforms off the main path, and 5 faster bats (one low enough that you must jump it). The moon gate leads to the win screen. The same bot loses all three lives here. A scripted route finishes it in about 1300 ticks with no hits.
- **Title** (`main.mcscene`; the start-scene GUID is kept), **win** and **lose** screens with a MenuScreen. The loop runs title -> level 1 -> level 2 -> win -> title, and level -> lose -> title. I checked it with a render_sheet.
- **8 .mctest scenarios** in `tests/`, all passing:
  - level1_walk
  - level2_route
  - level2_bot_struggles
  - bat_hit
  - three_hits
  - title_starts_game
  - win_back_to_title
  - lose_back_to_title
- **5 goldens** in `goldens/` (title, level 1, level 2, win, lose), each re-diffed with a score of 0.
- **Final project_check:** pass is true (3 warnings for the decorative title shadows, 2 info notes).
- **ship_build 1.0.0:** ok, and the 300-tick smoke run exits 0. The zip is `dist/Glowmoth-1.0.0-linux-x64.zip`.

## Tasks I finished
#8, #9, #10, #12, #13 and #14, plus the closing note on #15. All 15 tasks are done.

## Friction reports filed (8)
1. The features list in engine_capabilities contradicts its own component list.
2. asset_schema with a type killed the MCP session (stdio framing broke).
3. The same crash with type "Tilemap".
4. module_build in a second agent's server leaves the new components unknown; only a server restart helps.
5. Scene AssetRef loops fail the whole cook as a dependency cycle. Code fixed it by making scene fields strings.
6. Game-module log lines never reach the log tail of run_headless.
7. project_check flags the deliberate drop-shadow text for low contrast.
8. project_check reports the tileset as unreferenced although the tilemaps use it.

## What blocked or cost time
- Restarting the server: once after the asset_schema crashes and three times for stale components.
- The scene cycle: until code's string-scene fix landed, the win and lose screens had no way back to the title.
