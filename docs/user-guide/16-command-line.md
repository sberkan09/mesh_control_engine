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
| `-frames N` | 0 | windowed: quit after N rendered frames (how many ticks that is depends on the frame rate: use `-capture_tick` for a moment of the game; 16.8) |
| `-capture <path.ppm>` | | with `-frames`, `-capture_tick` or `-capture_ticks`: write the frame as a PPM (the directory is created); with no display it draws offscreen by itself (16.8) |
| `-access_log <file>` | | write every screen reader announcement to this text file, one per line (with `-access.screen_reader 2` on a desktop without a reader; 24.4) |
| `-window WxH` | 1280x720 | windowed: the initial window size in points, e.g. `1080x2400` to check a phone layout |
| `-fullscreen 1` | 0 (saved) | desktop: borderless full screen; F11, Alt+Enter or Cmd+Ctrl+F toggle it while playing |
| `-capture_tick N` | 0 | windowed: stop on the first frame after N simulation ticks of this run, and with `-capture` write that frame (frame-rate independent, unlike `-frames`; with `-save`, the restored slot's tick does not count) |
| `-capture_ticks a,b,c` | | windowed: capture the first frame after each listed sim tick (ascending) and quit after the last; `-capture`'s path takes `{tick}` (else `_<tick>` goes before the extension) |
| `-view.entity N` | -1 | render from scene entity N's camera (file order, prefab instances flattened) instead of the primary one; render only, the state hash is unchanged |
| `-view.free x,y[,zoom]` or `x,y,z,tx,ty,tz[,fov]` | | render from a free 2D or 3D camera (eye, target, vertical degrees); render only |
| `-safe_area l,t,r,b` | | windowed: pretend the screen has these safe-area insets in pixels (UI and `ViewInfo` lay out inside them) |
| `-platform.video_driver offscreen` | | no window: SDL's offscreen driver (with `LIBGL_ALWAYS_SOFTWARE=1`, Mesa's software GL), for captures on a box without a display |
| `-input_map <guid or path>` | | the cooked `.mcinput` to load (default: the first in the pack, else the starter-kit map; chapter 11) |
| `-input <file>` | | play a scripted input file instead of the keyboard (`<from>[-<to>] Action[=value]` per line; chapter 11) |
| `-save <slot>` | off | load the slot at start if it exists; write it on a clean quit, when a phone goes to the background, and when a desktop window loses focus or minimizes |
| `-save_on_focus_loss 0` | 1 | with `-save`: do not write the slot on focus loss (once per focus loss otherwise) |
| `-mods 0` | 1 | games that take mods (chapter 19): start without them |
| `-mods_dir <dir>` | the game's `mods/` | games that take mods: read mods from this folder instead |
| `-save_dir <dir>` | the per-user save folder (2.5) | where slots live; the player logs the folder it uses once at start (`save folder ...`) |
| `-settings 0` | 1 | neither read nor write `<save folder>/settings.cfg` (the saved options: bus gains, `r.quality`, `fullscreen`, ...) |
| `-console 0` | 1 | disable the developer console |
| `-console_open 1` | 0 | start with the console open |
| `-pause_on_focus_loss 0` | 1 | keep simulating when the window loses focus |
| `-lifecycle_test N,M` | off | rehearse a phone going to the background at app tick N and coming back M ticks later (also headless; the save slot is written on the way out) |
| `-r.post2d 0` | 1 | start with the camera's 2D post effects off (F7 toggles) |
| `-r.frame_report N` | 0 | windowed: print the average, p95 and worst frame time every N frames, and the run's average and worst at exit |
| `-perf_capture <file>` | off | write a `.mcperf` capture of the run at exit: each frame's wall, sim, render and GPU time, sim time per ECS phase, render counters, and per-system totals, with p50/p95/p99/max and the spike frames; prints a one-line summary. Never changes the state hash; works headless (times and systems, no draws) and windowed |
| `-perf_warmup N` | 0 | `-perf_capture`: leave the first N frames out (loading, first uploads) |
| `-r.vsync 0` | 1 (saved) | present without vsync |
| `-r.gl_debug 0` | 1 | disable the GL validation callback (restart-only) |
| `-time_scale x` | 1 | sim speed (a cheat; scales how many ticks run, never `dt`) |
| `-sim.max_catchup N` | 5 | max fixed ticks per rendered frame |
| `-audio.enabled 0` | 1 | run silent |
| `-audio.master_db`, `-audio.music_db`, `-audio.sfx_db`, `-audio.ui_db`, `-audio.voice_db` | 0 | bus gains in dB (saved) |
| `-audio.reverb 0` | 1 (saved) | every reverb unit off (a dry mix) |
| `-audio_record <file.wav>` | | headless: render the mix offline, every tick's share, into a 48 kHz float WAV, and print its length, peak and a hash of its samples |
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
| `-editor.open <asset>` | | open a `.mcui`, `.mctimeline` or `.mcanim` (relative to `assets/`) in its panel at start-up |
| `-editor.panel <name>` | | bring a panel's tab to the front at start-up (`Profiler`, `Play`, ...) |
| `-editor.perf_capture <file>` | | open a `.mcperf` capture in the Profiler's Capture tab at start-up (relative to the project, or absolute) |
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
my_game_mcp --project <dir> [--scene <path under assets/>] [--player <exe>] [--docs <dir>] [--allow-cheats]
mce_mcp --project <dir> --module <lib<game>.so> [--scene <path under assets/>] [--player <exe>] [--docs <dir>] [--allow-cheats]
```

`--module` loads the game module (the SDK's way; `my_game_mcp` has the game compiled in): its
components become known to the tools and its systems run in `run_headless`. `--player` is the
executable `render_frame` draws with (default: `mce_player` beside the server, or the project's host
for `my_game_mcp`). `--docs` is the folder served as `mce://docs/...` (default: the SDK's
`share/doc/mce` beside the server, or the source tree of a build).

## 16.4 The cook

```
my_game_cook <project-dir> [--base <pack>]... [--pack] [--force] [--adopt] [--only <path>]... [--skip-errors] [--quiet]
mce_cook <project-dir> [--module <lib<game>.so>] [--base <pack>]... [--pack] [--force] [--adopt] [--only <path>]... [--skip-errors] [--quiet]
         [--textures rgba8|bc|astc] [--max-texture-size N]
```

Exit 0 clean, 1 cook errors (a module that fails to load among them), 2 usage. `mce_cook`
without `--module` knows engine components only. `--base` names a game pack the project builds
on (a mod, chapter 19): references may name its assets and same-GUID assets override them.
`--only <path>` (repeatable) cooks just that file or folder under `assets/` and what it depends on, into `cooked/` only (never the pack); `--skip-errors` lists every failing file at the end (section 5.4). A source file with no sidecar is a cook error until `--adopt` registers it. `mce play` and `mce headless` run the build's pack and print a warning when it is older than `assets/`: `mce build` refreshes it.

`--max-texture-size N` caps the longest side of PNG and JPEG textures at N pixels (a power of two from 1 to 16384; bigger images are box-halved before their mips; default no limit), applied as the smaller of N and the texture's sidecar `max_size` (section 5.2). Any other value is an error, exit 1, that says what to change. Unless `--quiet`, the cook ends with a size report after the summary line: `mce_cook: size: textures …, meshes …, audio …, scenes …, other …, total … bytes`, `mce_cook: textures: N, D downscaled by max_size / --max-texture-size`, and the five largest assets with their bytes.

`--textures rgba8|bc|astc` picks the texture storage: `rgba8` (default), `bc` (BC7, BC5, BC4 or BC1 blocks by the sidecar's `compress`, for desktop packs) or `astc` (phones); `mce cook` passes it through, and `mce ship --textures rgba8|bc` re-cooks the pack with it (section 5.2). An unknown value is exit 1 with the allowed ones.

## 16.5 Scripts

```
python3 scripts/new_project.py <Name> [--game id] [--title "Title"] [--dest dir] [--seed N] [--force]
python3 scripts/ship.py --project projects/<Name> [--preset p] [--version v] [--out dir] [--no-build] [--keep-symbols] [--smoke] [--smoke-headless] [--max-texture-size N] [--textures rgba8|bc]
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
mce new <Name> [--game id] [--title "Title"] [--template project|novel] [--dest dir] [--force]
mce example [<Name>] [--dest dir] [--force]
mce build <project> [--config Debug|Release] [-D<var>=<value> ...]
mce play <project> [player flags ...]
mce headless <project> [--ticks N] [player flags ...]
mce editor <project> [editor flags ...]
mce mcp <project> [--scene <path>] [--allow-cheats] [--agent <name>]
mce mcp-config <project> [--desktop] [--remove]
mce cook <project> [--pack] [--force] [--adopt] [--only <path>]... [--skip-errors] [--quiet] [--max-texture-size N] [--textures rgba8|bc|astc]
mce story compile [<project>] [--check] [--werror] [--out header] [--report md]
mce import aseprite <export.json> [--out f.mcsheet] [--name n] [--werror]
mce import tiled <map.tmj> [--out-dir d] [--name n] [--tile-size m] [--werror]
mce import ldtk <project.ldtk> [--level L] [--out-dir d] [--name n] [--tile-size m] [--werror]
mce import spine|dragonbones <export.json> [--out f.mcskel2d] [--name n] [--ppm 100] [--atlas f] [--texture f.png] [--strict]
mce content list|info|check|fetch|add|verify|credits ...    (open-licence content ledger, chapter 5.3)
mce retarget <source.glb>... --onto <character.glb> [--out f.glb | --inplace] [--clips a,b] [--prefix p] [--map f.json]
             [--in-place-motion] [--no-translation] [--scale auto|N] [--strict]    (chapter 8.9)
mce ship <project> [--target desktop|web] [--out dir] [--version v] [--max-texture-size N] [--textures rgba8|bc]
mce friction [<project> ...] [--out file] [--json]
mce update [<project> ...] [--release [tag]] [--no-pull]
mce version
mce --help
```

Each subcommand prints the command it runs (`mce mcp` on stderr, since its stdout is the MCP
protocol) and exits with that tool's code (2 for a usage error).
`mce example` with no name lists the SDK's example projects (`share/mce/examples/`); with one it
copies that project to `<dir>/<Name>`, ready for `mce build`.
`mce new --template novel` stamps a visual novel on the novel kit; `mce story compile` compiles
its `story/*.story` (chapter 21; `mce build` also does when the project says `STORY`).
`mce new` and `mce example` also write `<project>/.mcp.json`, registering `mce mcp <project>` as
the server `mce_<game>` for Claude Code; `mce mcp-config` rewrites it, and with `--desktop` merges the
same entry into Claude Desktop's `claude_desktop_config.json` (chapter 14.1).
`mce editor`, `mce mcp` and `mce cook` run the SDK's `mce_editor`, `mce_mcp` and `mce_cook` with
`-module <project>/build/lib<game>.so`; `mce ship` writes `<project>/dist/<Name>-<version>-<os>.zip`
holding the host, the module, `libmce_engine`, `assets.mce`, `CREDITS.md` (when the project has one),
`VERSION.txt` and `README.txt`. `mce friction` gathers the agents' notes from each project's `friction.jsonl`
(written by the MCP tool `report_friction`, chapter 14) into one Markdown backlog, or JSON with `--json`.

## 16.8 Screenshots without a display (a container, CI, SSH)

A frame of the real game, without a window on a screen. This was run in a Linux container with no `DISPLAY`
and no GPU (`mce_player` is the player from the build tree; a project's own host, `my_game`, takes the same
flags, and so does `mce play <project> ...`):

```
mce_player -pack assets.mce -capture_tick 120 -capture shots/a.ppm
mce_player -pack assets.mce -capture_ticks 60,120 -capture "shots/t_{tick}.ppm"
mce_player -pack assets.mce -input moves.txt -capture_tick 300 -capture shots/play.ppm
```

When a capture is asked for (`-capture`, `-capture_tick`, `-capture_ticks`) and SDL cannot open a video device
(no `DISPLAY`, no `WAYLAND_DISPLAY`: "No available video device"), the player switches by itself to SDL's
offscreen driver with Mesa's software GL (`LIBGL_ALWAYS_SOFTWARE=1`, unless you set it) and prints one line:
`no display for the capture: drawing offscreen with software GL`. A machine with a display draws in a window as
before. Naming a driver yourself (`-platform.video_driver x11`) is never replaced: if it fails, the run fails.
The two ways that need no fallback, and also worked here:

```
LIBGL_ALWAYS_SOFTWARE=1 mce_player -pack assets.mce -platform.video_driver offscreen -frames 30 -capture shots/c.ppm
xvfb-run -a -s "-screen 0 1280x720x24" env LIBGL_ALWAYS_SOFTWARE=1 mce_player -pack assets.mce -capture_tick 60 -capture shots/d.ppm
```

The file is a binary PPM (`P6`) of the window's size (`-window WxH`, 1280x720 by default), 3 bytes a pixel. Convert it
with any image tool (`convert shots/a.ppm shots/a.png`), or take frames as PNG over MCP (`render_frame`, chapter 14).

**Which frame you get.**

| Flag | What it captures |
|---|---|
| `-capture_tick N` | the first frame drawn after simulation tick N of this run; the sim holds on that tick for the frame (no interpolation), so the same pack and input give the same bytes on every run and every machine speed, then the player quits. With `-save`, the restored slot's ticks do not count. |
| `-capture_ticks a,b,c` | the same, once per listed tick (ascending, at most 64), then quit; `{tick}` in `-capture`'s path, else `_<tick>` before the extension |
| `-frames N` with `-capture` | the Nth rendered frame, whichever tick the sim is on then. A frame is not a tick: the same `-frames 30` is tick 2 on one machine and tick 40 on another, and drawing offscreen runs many frames per tick. Use it for a smoke test (the renderer draws something), never for a moment of the game |

**A game with a title menu.** A capture shows what is on screen at that tick. A game that starts on its own title
menu (a menu the game's code or its UI document draws) shows the menu at tick 300 as it did at tick 1 unless
the input presses it away. Give the capture a script that does what a player does, and pick the tick after it:

```
# moves.txt: one line per input, <from>[-<to>] Action[=value], by sim tick (chapter 11)
10 Confirm
mce_player -pack assets.mce -input moves.txt -capture_tick 120 -capture shots/level1.ppm
```

Over MCP, `render_frame` and `timeline_preview` take the same `input` (and `convars`). `timeline_preview` plays the
project's scene with the real player, so a title menu that the game itself draws over its start scene is drawn over
the cutscene frame as well; the tool cannot tell a title menu from a HUD. Press the menu away with `input`, or set
the game's own convar that skips it, or preview the cutscene from a scene that does not carry the menu.

