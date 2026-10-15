# Marble Garden

A small 3D game made with Mesh Control Engine through its MCP server, with no human in the loop.

## What it is

You roll a glass marble around a walled garden diorama that sits on a wooden table. The garden has
lawns, five hedges, four trees, flowers, two stone terraces reached by ramps, and a pond in the
middle. Five glowing, spinning gems (ruby, topaz, amethyst, sapphire, aqua) are spread across the
garden. Two of them are up on the terraces. Collect all five before the 90-second timer runs out.
If you roll into the pond there is a splash and the marble goes back to the start, but the clock
keeps running.

- **Title screen** (`scenes/main.mcscene`): the garden in afternoon light, the title and how to start.
- **Garden** (`scenes/garden.mcscene`): the round. The HUD shows the time left and the gems collected.
- **Win screen** (`scenes/win.mcscene`): the five gems on show, with "All 5 gems with N seconds to spare".
- **Lose screen** (`scenes/lose.mcscene`): a dimmer sunset, with "You found X of 5 gems".
- **Look and sound**: a warm sun with soft shadows, a warm sky and fog, and coloured point lights on the gems.
  The music is a gentle 84 bpm pentatonic loop with no drums, and each gem plays a sine-wave chime.

## How to play

- **Space** or **Enter**: start from the title screen, or go back to it from the win and lose screens.
- **WASD** or the **arrow keys**: roll the marble, relative to the camera, which follows from above and to the south.
- Go around the pond, not across it. Take the ramps up to the terraces.

## How it is built

- **Game code** (`games/marble_garden/module/`):
  - Components: `Marble`, `Gem`, `GameRules`, `HudText`, `ScreenAdvance`, `Spin` and `Bob`.
  - Systems:
    - The marble is pushed by impulses on a `RigidBody3D` sphere.
    - A fall below `kill_y` respawns it at the start.
    - Gems are collected within a set distance of the marble.
    - A countdown sends you to the win or lose scene with `scene::request_change`, and a `LastRound` resource carries the result to that screen.
    - The HUD text is filled each tick.
- **Scenes**: every scene and asset was authored through MCP tools. The meshes come from `gen_mesh`,
  tinted per instance. The ramps use `MeshCollider3D` and the other blocks use `BoxCollider3D`. The
  camera uses `CameraFollow3D`. Music and sounds come from `gen_music` and `gen_sfx`. The fonts are
  Crimson Pro from the content library (OFL).
- **Tests**: 8 `.mctest` files, all passing.
  - Title to garden.
  - Pond respawn.
  - Timeout to the lose screen.
  - A `walk_to` bot collects the Ruby, Topaz, Sapphire and Aqua gems (one test each).
  - A steered input route collects all five gems and reaches the win screen.
- **Goldens**: `goldens/{title,garden,win,lose}.png` for `image_diff`.
- **Release**: `project_check` passes with no errors, warnings or infos. `ship_build` made
  `dist/MarbleGarden-1.0.0-linux-x64.zip`.

## What I could not do

- **No bot route to the Amethyst gem.** The built-in `walk_to` bot heads straight at one target, and
  its straight line to the Amethyst gem crosses the pond. There is no bot that follows waypoints or
  collects several targets. The full win is tested with a scripted input array that I computed
  offline with a steering loop over `run_headless`.
- **Renders don't match headless runs.** `render_frame` and `render_sheet` with that long scripted
  input do not play the same as `run_headless`: the rendered run falls behind and never wins. So
  the goldens only cover screens with no input, and gameplay is checked with `test_run`.
- **Gamepad and touch controls are not tuned**, and there is no pause menu. The engine's default
  input map is used.
- **No GPU check.** All rendering was checked on the software (Mesa llvmpipe) renderer.
