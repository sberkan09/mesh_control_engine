# 16. Command-line reference

Every executable built on the engine's app loop parses `-name value` pairs into convars, so each
flag below is also a console variable you can print or set at run time (`name` alone prints,
`name value` sets). Booleans take `0`/`1` or `false`/`true`.

## 16.1 The player and every project host

`mce_player`, `my_game`, the samples built on the player runtime.

| Flag | Default | Meaning |
|---|---|---|
| `-pack <file or dir>` | `assets.mce` beside the executable, else the build tree's pack | the cooked pack, or a `cooked/` directory |
| `-module <file>` | the host's own module | the game module to install |
| `-scene <guid or path>` | the `.mceproj` start scene, else the first scene by path | which scene to play |
| `-headless 1` | off | run without a window, renderer or audio device; prints the state hash at the end |
| `-ticks N` | 600 | headless: fixed ticks to simulate |
| `-frames N` | 0 | windowed: quit after N rendered frames |
| `-capture <path.ppm>` | | with `-frames`: write the last frame as a PPM (absolute path) |
| `-save <slot>` | off | load the slot at start if it exists; write it on a clean quit |
| `-save_dir <dir>` | the per-user save folder | where slots live |
| `-console 0` | 1 | disable the developer console |
| `-console_open 1` | 0 | start with the console open |
| `-pause_on_focus_loss 0` | 1 | keep simulating when the window loses focus |
| `-r.post2d 0` | 1 | start with the camera's 2D post effects off (F7 toggles) |
| `-r.vsync 0` | 1 (saved) | present without vsync |
| `-r.gl_debug 0` | 1 | disable the GL validation callback (restart-only) |
| `-time_scale x` | 1 | sim speed (a cheat; scales how many ticks run, never `dt`) |
| `-sim.max_catchup N` | 5 | max fixed ticks per rendered frame |
| `-audio.enabled 0` | 1 | run silent |
| `-audio.master_db`, `-audio.music_db`, `-audio.sfx_db`, `-audio.ui_db` | 0 | bus gains in dB (saved) |
| `-platform.video_driver <name>` | | SDL video driver override (`dummy`, `offscreen`, `x11`, ...) |
| `-platform.error_box 0` | 1 | no message box on boot failure |
| `-version` | | print the version line and exit |
| `-crash_reports 0` | 1 | do not write crash reports |
| `-crash_dir <dir>` | the per-user crashes folder | where crash reports go |
| `-crash_test 1` | | crash on purpose after start-up |

In-game keys: Esc pauses (the pause overlay), backtick or F1 toggles the console, F7 toggles 2D
post effects.

## 16.2 The editor

`mce_editor`, `my_game_editor`. The player flags that make sense for an editor apply too
(`-r.vsync`, `-r.gl_debug`, `-platform.*`).

| Flag | Default | Meaning |
|---|---|---|
| `-project <dir>` | | the project folder (holds `assets/`) |
| `-scene <path under assets/>` | the first scene | the scene to open |
| `-editor.panel <name>` | | bring a panel's tab to the front at start-up (`Profiler`, `Play`, ...) |
| `-editor.select <name>` | | select the entity with this name after opening |
| `-editor.play 1` | 0 | enter play-in-editor after opening |
| `-editor.player <path>` | beside the editor / the build tree | the player executable for Ctrl+F5 |
| `-editor.ini <file>` | `mce_editor.ini` | dock layout file (empty: no persistence) |
| `-editor.autosave_interval s` | 300 | seconds of unsaved changes before an autosave (0: only on focus loss) |
| `-editor.hot_reload_interval s` | 0.5 | seconds between polls of `assets/` (0: off) |
| `-frames N` | 0 | quit after N frames (smoke tests) |

## 16.3 The MCP server

`mce_mcp`, `my_game_mcp`. Plain arguments, not convars:

```
my_game_mcp --project <dir> [--scene <path under assets/>] [--allow-cheats]
```

## 16.4 The cook

```
my_game_cook <project-dir> [--pack] [--force] [--adopt] [--quiet]
```

Exit 0 clean, 1 cook errors, 2 usage. `mce_cook` has the same interface for engine-only content.

## 16.5 Scripts

```
python3 scripts/new_project.py <Name> [--game id] [--title "Title"] [--dest dir] [--seed N] [--force]
python3 scripts/ship.py --project projects/<Name> [--preset p] [--version v] [--out dir] [--no-build] [--keep-symbols] [--smoke] [--smoke-headless]
python3 scripts/ship.py platformer | diorama | all
python3 scripts/symbolicate.py <report.crash> --symbols <dir>...
scripts/ci/local_ci.sh [--quick] [stage ...]        # stages: walls linux-gcc-debug linux-clang-release linux-gcc-release linux-asan tidy determinism
scripts/ci/setup_env.sh                               # Ubuntu 24.04 packages
```

## 16.6 Samples with their own flags

`sample_03_platformer` (`-level N`, `-autoplay 1`), `sample_04_cubes` (`-walk 1`, `-r.msaa N`,
`-lights N`, `-r.cascades N`, `-night 1`, `-cam.yaw`, `-cam.pitch`, `-cam.dist`, `-cam.auto 0`),
`sample_05_ashen_court` (`-autoplay 1`), `sample_02_boids` (`-boids N`), `sprites_2d` (`-sprites N`),
`ui_gallery` (`-page title|options|levels|console`). Each sample's `main.cpp` header lists them.
