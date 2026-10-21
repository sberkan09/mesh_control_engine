# Friction backlog

Collected by mce unknown from 1 project(s).

## major (2)

### playtest / test_run bots (1)

- walk_to bot takes one target and no waypoints, so it cannot finish a level whose straight line crosses a hazard or needs several pickups (MarbleGarden, solo, playtest)
  - expected: a bot that follows a list of targets/waypoints (or a path around kill volumes), so one .mctest can collect all five gems and win
  - actual: walk_to heads straight for one target; toward the Amethyst gem it rolled into the pond 11 times; a full win needs five targets
  - workaround: wrote an offline closed-loop steering script that calls run_headless every 6 ticks (about 250 runs) and froze the result as a scripted input array in tests/win_all_gems.mctest; one walk_to test per reachable gem
  - details: also: run_headless trace caps at 1000 rows silently (values just stop), which made my steering read a stale position until I set every to 6

### render_frame / determinism (1)

- render_frame/render_sheet with the same scripted input diverge from run_headless/test_run in a physics scene (MarbleGarden, solo, render_sheet)
  - expected: the rendered run plays the same ticks as run_headless for the same scene and input (so a frame shows the moment a test reached)
  - actual: with a 252-segment fractional Move array, run_headless and test_run collect all 5 gems and win at tick 1507; render_sheet of the same scene and input shows 3/5 gems at tick 1560 with the marble back near the start. Before a CameraFollow3D pitch/offset change the same render did reach the win screen
  - workaround: treat test_run as the truth for gameplay; take goldens only from screens without input or with short inputs
  - details: game code reads only CameraFollow3D.yaw (unchanged); possibly the input array is rounded when handed to mce_player, or the player steps differently

## minor (4)

### asset_delete / project_check credits (1)

- asset_delete leaves the deleted file's CREDITS.md row, and project_check does not flag credits for files that no longer exist (MarbleGarden, solo, asset_delete)
  - expected: asset_delete drops (or project_check warns about) the CREDITS.md row of a file that is gone
  - actual: after deleting ProggyClean.ttf, cone.glb and the template textures, their rows stayed and shipped in the zip
  - workaround: removed the rows from CREDITS.md by hand and re-ran ship_build

### docs / log.h (1)

- MCE_LOG_INFO is spdlog/fmt style ({}), but nothing says so and printf-style %u compiles silently and logs the literal (MarbleGarden, solo, module_build)
  - expected: the game-code chapter or log.h comment states the format syntax, or a compile-time check rejects %-specifiers
  - actual: MCE_LOG_INFO("... (%u)", n) built without a warning and logged "(%u)"
  - workaround: switched to {} placeholders after reading log.h (#define MCE_LOG_INFO(...) SPDLOG_INFO(__VA_ARGS__))

### gen_mesh (1)

- gen_mesh does not say which way the ramp shape rises (MarbleGarden, solo, gen_mesh)
  - expected: the tool help or the result names the ramp orientation (e.g. low at -X, high at +X) and the preview shows it from a side
  - actual: help says only "ramp"; the preview is an unlabelled silhouette; I parsed the .glb vertices to learn the high edge is at +X
  - workaround: read the vertex positions out of the saved .glb, then rotated the entity about Y

### project_watch / scene_save (1)

- scenes/main.mcscene was saved under my agent name by a write I did not make, leaving references to meshes that do not exist; the next cook failed on it (MarbleGarden, solo, project_watch)
  - expected: every write in .mce/events.jsonl attributed to agent solo comes from my own calls, or a second client on the same agent name is flagged
  - actual: event seq 15 scene_save scenes/main.mcscene by agent solo at a moment I was calling module_build; the file now holds a 3D title diorama with 4+ missing mesh GUIDs (bbc1f6e2-...), so render_frame of my garden scene refused with cook errors in an unrelated scene
  - workaround: scene_open main, scene_clear and re-author it as my title scene
  - details: render_frame cooks the whole project, so one broken unrelated scene blocks looking at any scene; a per-scene cook or a warning would let work continue
