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
| `-capture <path.ppm>` | | with `-frames` or `-capture_tick`: write the last frame as a PPM (the directory is created) |
| `-capture_tick N` | 0 | windowed: quit after N simulation ticks (frame-rate independent, unlike `-frames`) |
| `-input_map <guid or path>` | | the cooked `.mcinput` to load (default: the first in the pack, else the starter-kit map; chapter 11) |
| `-input <file>` | | play a scripted input file instead of the keyboard (`<from>[-<to>] Action[=value]` per line; chapter 11) |
| `-save <slot>` | off | load the slot at start if it exists; write it on a clean quit |
| `-mods 0` | 1 | games that take mods (chapter 19): start without them |
| `-mods_dir <dir>` | the game's `mods/` | games that take mods: read mods from this folder instead |
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

Every executable also takes `-help` (`--help`, `-h`): the usage line plus every convar with its
type, default, help and flags, and every console function. The list below is the useful subset.

## 16.2 The editor

`mce_editor`, `my_game_editor`. The player flags that make sense for an editor apply too
(`-r.vsync`, `-r.gl_debug`, `-platform.*`).

| Flag | Default | Meaning |
|---|---|---|
| `-project <dir>` | | the project folder (holds `assets/`) |
| `-module <file>` | | the game module to load (`lib<game>.so`): its components in the inspector and the cook, its `install()` in play-in-editor (`my_game_editor` has them compiled in) |
| `-scene <path under assets/>` | the first scene | the scene to open |
| `-editor.panel <name>` | | bring a panel's tab to the front at start-up (`Profiler`, `Play`, ...) |
| `-editor.select <name>` | | select the entity with this name after opening |
| `-editor.play 1` | 0 | enter play-in-editor after opening |
| `-editor.player <path>` | beside the editor / the build tree | the player executable for Ctrl+F5 |
| `-editor.ini <file>` | `mce_editor.ini` | dock layout file (empty: no persistence) |
| `-editor.autosave_interval s` | 300 | seconds of unsaved changes before an autosave (0: only on focus loss) |
| `-editor.hot_reload_interval s` | 0.5 | seconds between polls of `assets/` (0: off) |
| `-frames N` | 0 | quit after N frames (smoke tests) |
| `-capture <file.ppm>` | | with `-frames`: write the last frame |
| `-selftest` | | with `-frames`: check the viewport and shell pixels, exit 3 on failure |

## 16.3 The MCP server

`mce_mcp`, `my_game_mcp`. Plain arguments, not convars:

```
my_game_mcp --project <dir> [--scene <path under assets/>] [--allow-cheats]
mce_mcp --project <dir> --module <lib<game>.so> [--scene <path under assets/>] [--allow-cheats]
```

`--module` loads the game module (the SDK's way; `my_game_mcp` has the game compiled in): its
components become known to the tools and its systems run in `run_headless`.

## 16.4 The cook

```
my_game_cook <project-dir> [--base <pack>]... [--pack] [--force] [--adopt] [--quiet]
mce_cook <project-dir> [--module <lib<game>.so>] [--base <pack>]... [--pack] [--force] [--adopt] [--quiet]
```

Exit 0 clean, 1 cook errors (a module that fails to load among them), 2 usage. `mce_cook`
without `--module` knows engine components only. `--base` names a game pack the project builds
on (a mod, chapter 19): references may name its assets and same-GUID assets override them.

## 16.5 Scripts

```
python3 scripts/new_project.py <Name> [--game id] [--title "Title"] [--dest dir] [--seed N] [--force]
python3 scripts/ship.py --project projects/<Name> [--preset p] [--version v] [--out dir] [--no-build] [--keep-symbols] [--smoke] [--smoke-headless]
python3 scripts/ship.py platformer | diorama | all
python3 scripts/symbolicate.py <report.crash> --symbols <dir>...
./install.sh [archive.tar.gz | --release [tag]] [--prefix dir] [--no-path] [--yes]   # install the SDK (section 1.9)
scripts/release_sdk.sh [preset]                     # dist/mce-sdk-<version>-<os>-<arch>.tar.gz for install.sh
scripts/ci/local_ci.sh [--quick] [stage ...]        # stages: walls linux-gcc-debug linux-clang-release linux-gcc-release linux-asan tidy determinism
scripts/ci/local_ci_macos.sh [--quick] [stage ...]  # on a Mac; stages: walls macos-debug macos-release macos-asan validation samples determinism ship
scripts/ci/setup_env.sh                               # Ubuntu 24.04 packages
```

## 16.6 Samples with their own flags

`sample_03_platformer` (`-level N`, `-autoplay 1`), `sample_04_cubes` (`-walk 1`, `-skin 0`, `-r.bloom 0`, `-r.fxaa 0`, `-r.sky_texture 1`, `-r.msaa N`,
`-lights N`, `-r.cascades N`, `-night 1`, `-cam.yaw`, `-cam.pitch`, `-cam.dist`, `-cam.auto 0`),
`sample_05_ashen_court` (`-autoplay 1`), `sample_02_boids` (`-boids N`), `sprites_2d` (`-sprites N`),
`ui_gallery` (`-page title|options|levels|console`). Each sample's `main.cpp` header lists them.

## 16.7 The `mce` command (installed SDK)

`<sdk>/bin/mce`, Python 3. `<project>` is a folder with one `<Name>.mceproj`; its build is
`<project>/build`.

```
mce new <Name> [--game id] [--title "Title"] [--dest dir] [--force]
mce example [<Name>] [--dest dir] [--force]
mce build <project> [--config Debug|Release] [-D<var>=<value> ...]
mce play <project> [player flags ...]
mce headless <project> [--ticks N] [player flags ...]
mce editor <project> [editor flags ...]
mce mcp <project> [--scene <path>] [--allow-cheats]
mce cook <project> [--pack] [--force] [--adopt] [--quiet]
mce ship <project> [--out dir] [--version v]
mce update [<project> ...] [--release [tag]] [--no-pull]
mce version
mce --help
```

Each subcommand prints the command it runs and exits with that tool's code (2 for a usage error).
`mce example` with no name lists the SDK's example projects (`share/mce/examples/`); with one it
copies that project to `<dir>/<Name>`, ready for `mce build`.
`mce editor`, `mce mcp` and `mce cook` run the SDK's `mce_editor`, `mce_mcp` and `mce_cook` with
`-module <project>/build/lib<game>.so`; `mce ship` writes `<project>/dist/<Name>-<version>-<os>.zip`
holding the host, the module, `libmce_engine`, `assets.mce`, `VERSION.txt` and `README.txt`.
