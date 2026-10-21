# A level that a bot can finish

You want a new level that is provably finishable, and a test that fails the day it stops being so.

1. **Reuse a level's skeleton.** `scene_open` an existing level, `scene_list_entities` to see its
   camera, player, ground and goal, then `scene_save {"path": "scenes/level2.mcscene"}`: a copy with
   its own path. (A fresh `scene_new` gets a new GUID; add it to the game's level list.)
2. **Lay it out.** `entity_duplicate` a platform and `component_set` its
   `LocalTransform.position` for each new one; name them (`Ledge 1`, `Ledge 2`). Keep the player and
   the goal named `Player` and `Goal`. `scene_save`.
3. **Look.** `render_sheet {"ticks": [1, 60, 180]}` shows the level as the player draws it.
4. **Play it.** For a side view:

   ```json
   { "bot": { "kind": "walk_to", "actor": "Player", "target": "Goal", "plane": "x" }, "ticks": 1800 }
   ```

   `playtest` returns the feel report. `finished: true` and a `time_to_finish_s` you expect mean
   the level works. `stuck_spots` say where the bot could not get on (a gap too wide, a wall too
   high): `render_frame` with `camera: {"center": [x, y], "zoom": 1}` at the spot's position shows
   it. Move a ledge and play again.
5. **Keep it.** Save the same arguments as `tests/level2.mctest`, with what must stay true:

   ```json
   {
     "name": "level 2 can be finished",
     "scene": "scenes/level2.mcscene",
     "bot": { "kind": "walk_to", "actor": "Player", "target": "Goal", "plane": "x" },
     "ticks": 1800,
     "assert": [
       { "feel": "finished_tick", ">": 0 },
       { "feel": "finished_tick", "<=": 2400 },
       { "log_errors": 0 }
     ]
   }
   ```

   `finished_tick` counts sim ticks (60 a second): 2400 is the 40 seconds the level may take.
   `test_run` runs every file under `tests/`.
6. **Hunt crashes.** A `masher` bot (`{"kind": "masher", "actor": "Player"}`) presses random chords
   for a few thousand ticks; `{"log_errors": 0}` as its only check finds soft locks and asserts.
