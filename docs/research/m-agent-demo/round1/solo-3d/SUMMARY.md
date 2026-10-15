# Marble Garden

A small 3D game made with Mesh Control Engine (MCE) through its MCP server, with no person in the loop.

## What the game is

You roll a blue-and-cream marble around a walled garden diorama on a warm late afternoon. The
garden has low stone walls, four hedges, a pond, a sandy path, trees, flower beds, and two raised
stone terraces, each with a ramp up to it. Five glowing gems (ruby, amber, emerald, sapphire,
amethyst) are spread around it: one on each terrace, one in a nook between a hedge and the north-east
terrace, one on the narrow strip between the pond and the west wall, and one in the south-east
corner. Collect all five before the 90-second timer runs out. If you roll into the pond, the marble
goes back to the start with a splash.

- **Title screen:** the camera slowly circles the whole garden, with the name, the goal and "Press Space to start".
- **Playing:** a close follow camera, gems collected so far (top left) and the time left (top right, turning red under 10 seconds).
- **Win screen:** "All five gems!" and the seconds left over.
- **Lose screen:** "Time's up" and how many gems you found.
- From the win or lose screen, Space starts a new round. The gems come back and the marble returns to the start.
- **Look:** warm, low sun with long shadows; a hazy blue-to-peach sky gradient; the walled garden sits on a stone plinth in a golden meadow ringed with trees.
- **Sound:** a gentle looping pentatonic theme (84 bpm, no drums) and a rising chime for each gem. There are also a splash, a start blip, and a win and a lose sting.

## How to play

| Input | Action |
|---|---|
| W A S D or the arrow keys | roll the marble (relative to the camera; W is "away from you") |
| Mouse | turn the camera around the marble |
| Space | start a round, or play again after the win or lose screen |
| Esc | pause |

The marble has momentum, so you have to steer against your own speed to stop. Both ramps can be
climbed by rolling straight up them.

## How it is built

- **Scene** (`assets/scenes/main.mcscene`): authored through the MCP tools (`entity_create`, `component_set`, `scene_save`), driven by `tools/build_scene.py`. It uses only engine components:
  - Environment3D, Camera3DComponent and CameraFollow3D
  - BoxCollider3D, plus MeshCollider3D for the ramps
  - a dynamic sphere RigidBody3D for the marble
  - a Trigger3D on each gem, with an SfxTrigger "on TriggerEnter" for the chime
  - PointLight3D for the gem glow
  - TextLabel and Sprite on a pixel 2D camera for the HUD
  - an AudioSource for the music
- **Game code** (`games/marble_garden/module/`): the round's rules in a `GardenState` resource. It handles the title, playing, won and lost phases, the timer, gem pickup and reset, and the pond respawn. It also turns Move into an impulse on the marble relative to the camera, moves the title camera, makes the gems spin and bob, and writes the HUD text. The systems find their entities by the engine components they carry (see "What I could not do").
- **Art:** greybox models from `tools/gen_models.py` (boxes, wedge ramps, a striped marble, a bipyramid gem, a tree, a flower bed and a rock). Textures come from `gen_texture`, the music from `gen_music`, the sounds from `gen_sfx`, and the font (Crimson Pro Bold) from the content library. Credits for all of them are in `CREDITS.md`.
- **Tests:**
  - `module_test` runs four ctest tests that play the real host headless with scripted input: a 300-tick smoke run, a route that collects all five gems and wins, a roll into the pond that respawns once, and a timeout that loses and then starts round 2. The routes were planned by `tools/plan_route.py`, which steers the marble with the host in the loop.
  - `test_run` runs two `.mctest` scenarios: the scene loads with every gem and no log errors or warnings, and the marble settles at its start.
  - `image_diff` keeps goldens of the title, playing, win and lose screens.
- `project_check` passes with no errors, warnings or infos. `ship_build` produced the zip.

## What I could not do

- **New components did not load into the running MCP server.** `module_build` reports that it reloaded, but the server keeps the module it started with, mapped at the same copy path each time. So game-defined components (`Marble`, `Gem`, `GardenGame`, ...) stayed "unknown component type" and could not be authored or cooked. I rewrote the game to use only engine components. The systems identify:
  - the marble as the dynamic sphere body
  - the gems by their Trigger3D
  - the HUD lines by `TextLabel.order`
  - the sounds by the HUD entity they sit on

  This works, but tuning values live in code constants instead of the scene.
- **No walk_to bot playtest of the real gameplay.** For the same reason, `run_headless`, `playtest` and `test_run` play without the game's systems, so the marble never moves under a bot. Gameplay is covered by the ctest tests through the host and by `render_frame` and `render_sheet` with scripted input instead.
- **No primitive meshes.** There is no mesh generator, and the library has no primitives, so the models are written by a Python script as glTF.
- **The sky texture looked wrong.** A generated equirect sky washed out near the sun, so the scene uses the Environment3D sky gradient.
- **Not all audio was checked by ear.** Each sound was checked only by its waveform image.

## Result

- `project_check`: pass, with 0 errors, 0 warnings and 0 infos.
- `ship_build`: ok (full build, a 300-tick headless smoke run, then the zip). The zip is `/tmp/claude-0/demo/solo-3d/MarbleGarden/dist/MarbleGarden-1-linux-x64.zip`. It holds the host, the module, `libmce_engine.so`, `assets.mce`, `CREDITS.md`, `VERSION.txt` and `README.txt`.
- `module_test`: 4 of 4 passed. `test_run`: 2 of 2 passed. The title golden re-checks with a score of 0.
- I filed seven `report_friction` notes (`MarbleGarden/friction.jsonl`):
  1. there is no way to make primitive meshes
  2. `engine_capabilities` is stale about 3D bodies and SfxTrigger
  3. `module_build` does not reload new components (blocking)
  4. the in-process test tools run without the game systems
  5. a teleport from a FixedUpdate_Pre game system is lost
  6. `render_frame` simulates late ticks in real time
  7. `project_set {title}` cleared `start_scene`
