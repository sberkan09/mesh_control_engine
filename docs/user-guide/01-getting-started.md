# 1. Getting started

By the end of this chapter you will have built the engine, run a sample, created a project of
your own, played it, and opened it in its editor.

> **Two ways to use the engine.** From its **source checkout** (sections 1.2 to 1.8): your project
> is a folder under `projects/` and the engine's CMake builds it together with the engine. Or from
> the **installed SDK** (section 1.9): a prefix with the prebuilt tools, `libmce_engine`, the headers
> and a CMake package; your project lives anywhere, builds with `find_package(MCE)` or the `mce`
> command, and never sees the engine's source. The rest of the guide applies to both; where a
> command differs, the SDK form is given beside the checkout form.

## 1.1 Requirements

| | Linux | Windows | macOS |
|---|---|---|---|
| Build tools | CMake ≥ 3.28, Ninja, gcc 13 or clang 18 | CMake ≥ 3.28, Ninja, Visual Studio 2022 (MSVC) | CMake ≥ 3.28, Ninja, Xcode toolchain (AppleClang 17) |
| Graphics | OpenGL 4.5 core driver (Mesa llvmpipe works, slowly) | OpenGL 4.5 driver | Metal (built in) |
| Python | 3.x for the helper scripts (`new_project.py`, `ship.py`) | same | same |
| Presets | `linux-gcc-debug`, `linux-gcc-release`, `linux-clang-debug`, `linux-clang-release`, `linux-asan` | `msvc-debug`, `msvc-release`, `msvc-asan` | `macos-debug`, `macos-release`, `macos-asan` |

Third-party dependencies (SDL3, Jolt, ImGui, spdlog, Tracy, glslang, stb, meshoptimizer, Box2D for
tests) are fetched at configure time; no package manager is needed for them. The first configure
downloads them once into `build/_fetch/`.

On Ubuntu 24.04, one script installs everything the Linux build needs (compilers, X11 and GL
development headers, Xvfb for display-less test runs, ccache):

```sh
scripts/ci/setup_env.sh
```

Other distributions need the equivalent of: `cmake ninja-build gcc g++ python3 libgl-dev libegl-dev
libx11-dev libxext-dev libxcursor-dev libxrandr-dev libxi-dev libxss-dev libxtst-dev libxfixes-dev
libxkbcommon-dev`.

## 1.2 Build the engine

```sh
git clone https://github.com/sberkan09/mesh_control_engine.git
cd mesh_control_engine
cmake --preset linux-gcc-debug          # configure (fetches dependencies the first time)
cmake --build --preset linux-gcc-debug  # build everything: engine, editor, tools, samples, tests
ctest --preset linux-gcc-debug          # optional: the unit tests that need no display
```

Everything lands under `build/<preset>/`. The executables you will use most:

| Executable | Path under `build/<preset>/` | What it is |
|---|---|---|
| `mce_editor` | `src/editor/app/mce_editor` | the scene editor; `-module <lib<game>.so>` loads a game's components and systems |
| `mce_player` | `src/player/mce_player` | the stock player: runs a cooked pack with the engine systems (and `-module`) |
| `mce_cook` | `tools/cook/mce_cook` | the asset cook; `--module <lib<game>.so>` for scenes that use a game's components |
| `mce_mcp` | `src/editor/mcp/mce_mcp` | the editor's MCP server for AI agents; `--module` as above |
| samples | `samples/<name>/<target>` | the sample ladder, for example `samples/03_platformer/sample_03_platformer` |

Without a module the tools know only the engine's components; once your game defines its own,
either hand them the game module (`-module` / `--module`, how the SDK works) or use the
per-project editor, cook and MCP server that the checkout build also makes (section 1.4). On
Windows, where game modules are not built yet, only the per-project tools know your components.

Useful configure options (pass them as `-D<name>=ON/OFF` after the preset):

| Option | Default | Effect |
|---|---|---|
| `MCE_BUILD_SAMPLES` | ON | build the sample ladder |
| `MCE_BUILD_EDITOR` | ON | build the editor and the MCP server |
| `MCE_BUILD_TESTS` | ON | build the unit tests |
| `MCE_GPU_TESTS` | OFF | also register tests that need a display and a GL 4.5 driver |
| `MCE_PHYSICS_ORACLE` | ON | download Box2D as the 2D physics test oracle (turn off to skip it) |
| `MCE_TEMPLATE_PROBE` | ON | stamp and build a throwaway project from the template, as a self-test |

## 1.3 Run a sample

The samples are small complete programs, one per engine milestone. Two good first runs:

```sh
build/linux-gcc-debug/samples/03_platformer/sample_03_platformer   # Ember Step, a 2D platformer
build/linux-gcc-debug/samples/04_cubes/sample_04_cubes             # a lit 3D scene with shadows
```

Esc pauses or quits, and the backtick key (or F1) opens the developer console in any player-based
program. Chapter 18 lists every sample and what it demonstrates.

## 1.4 Create a project

```sh
python3 scripts/new_project.py MyGame                  # -> projects/MyGame, game id my_game
python3 scripts/new_project.py MyGame --title "My Game" --game hero   # custom title and game id
```

The generator stamps `templates/project` into `projects/MyGame/` with fresh asset GUIDs. The
project is a tiny but complete 2D game: a camera, a ground, a spinning block, a bobbing block and a
crate that falls under 2D physics, plus the two gameplay components that drive the spinner and the
bobber. Chapter 2 walks through the folder.

Reconfigure so CMake sees the new folder, then build the project's targets:

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target my_game my_game_editor my_game_mcp
```

| Target | Output | Role |
|---|---|---|
| `my_game` | `build/<preset>/projects/MyGame/my_game` | the game: the engine's player runtime with your module and your cooked pack |
| `my_game_game` | `libmy_game.so` beside it | your gameplay code as a game module (linked into the host on Windows) |
| `my_game_cook` | `my_game_cook` | the cook that knows your components; the build runs it to produce the pack |
| `my_game_editor` | `my_game_editor` | the editor with your components in the inspector and your systems in play-in-editor |
| `my_game_mcp` | `my_game_mcp` | the MCP server with your components, for an AI agent |

Building `my_game` also cooks `projects/MyGame/assets/` into
`build/<preset>/projects/MyGame/project/assets.mce`, the pack the game loads. The three
per-project tools are conveniences of the checkout build: the stock `mce_editor -module
build/<preset>/projects/MyGame/libmy_game.so -project projects/MyGame` (and `mce_mcp --module`,
`mce_cook --module`) do the same by loading the module, which is how the SDK's tools work.

## 1.5 Play it

```sh
build/linux-gcc-debug/projects/MyGame/my_game
```

WASD or the arrows move, Space jumps, J fires, Esc pauses, backtick opens the console. The template
game does not read input, so for now just watch the crate land on the ground.

Headless, for a quick check that does not need a window:

```sh
build/linux-gcc-debug/projects/MyGame/my_game -headless 1 -ticks 600
```

This simulates 600 fixed ticks (10 seconds at 60 Hz) and prints the World state hash, the number
you will later use to check that a change did what you meant (chapter 14).

## 1.6 Open it in the editor

```sh
build/linux-gcc-debug/projects/MyGame/my_game_editor -project projects/MyGame
# or the stock editor with the module loaded:
build/linux-gcc-debug/src/editor/app/mce_editor -module build/linux-gcc-debug/projects/MyGame/libmy_game.so -project projects/MyGame
```

The editor opens the project's start scene (`assets/scenes/main.mcscene`). Select the Spinner in the
Hierarchy, change `degrees_per_second` in the Inspector, press F5 to play, F5 again to stop,
Ctrl+S to save. Chapter 3 covers the editor in full.

## 1.7 The edit loop at a glance

```
edit assets/ (editor, MCP agent or a text editor)
        │
        ▼
cook  ──► cooked/ (dev) or assets.mce (pack)      # F7 in the editor; automatic in the build
        │
        ▼
play  ──► my_game, my_game -headless 1, F5 in the editor
        │
        ▼
ship  ──► python3 scripts/ship.py --project projects/MyGame   # dist/<Name>-<version>-<os>.zip
```

- **Content** is text and source files under `assets/`, each with a GUID. Everything the runtime
  loads is a cooked binary; the runtime never parses text.
- **Gameplay** is C++ in `games/<game>/module/`: plain reflected components and systems, loaded as
  a module by a thin host.
- **The editor and an MCP agent make the same edits** through the same command funnel, so a scene
  authored by an agent is an ordinary `.mcscene` you can open, diff and undo.

## 1.8 Before merging engine changes

If you also change the engine (not just your project), run the local CI before merging. There is
no hosted CI.

```sh
scripts/ci/local_ci.sh --quick     # walls + gcc debug build with GPU tests under Xvfb
scripts/ci/local_ci.sh             # every stage: walls, four Linux presets, clang-tidy, determinism
```

## 1.9 The installed SDK

The SDK is the engine as a normal, installed engine: no source checkout, no engine build on your
side. It exists for Linux and macOS (the platforms with game modules; on Windows use the checkout).

### Install it

From an engine checkout, once (a release build is the one to install):

```sh
cmake --preset linux-gcc-release
cmake --build --preset linux-gcc-release
cmake --install build/linux-gcc-release --prefix ~/mce-sdk
```

`~/mce-sdk` then holds:

| Path | What |
|---|---|
| `bin/` | `mce` (the command below), `mce_editor`, `mce_mcp`, `mce_cook`, `mce_player`, `mce_devhost` |
| `lib/` | `libmce_engine.so` (the engine; every tool and every game module shares it), `libmce_player_runtime.a` (what your host links) |
| `include/` | the engine headers (`mce/gameplay.h` is the one a game module includes), `mce/engine_version.h` (this engine's id) |
| `lib/cmake/MCE/` | `MCEConfig.cmake` for `find_package(MCE)`, and the `mce_add_project` / `mce_add_game_module` helpers |
| `share/mce/` | the project template, `new_project.py`, `VERSION` |

Put `~/mce-sdk/bin` on your `PATH`, or call `~/mce-sdk/bin/mce` by path. The SDK needs the same
tools a project build needs (CMake 3.28+, Ninja, a C++20 compiler of the same family the engine
was built with: `mce version` prints the engine's build stamp) and Python 3 for `mce` itself.

### Make, build, play, edit, ship a project

```sh
mce new MyGame                 # -> ./MyGame (anywhere; --dest picks the parent folder)
mce build MyGame               # cmake -S MyGame -B MyGame/build -DCMAKE_PREFIX_PATH=<sdk> + build (--config Release for a release build)
mce play MyGame                # the game; flags after the project pass through (chapter 16)
mce headless MyGame --ticks 600   # no window; prints the state hash
mce editor MyGame              # mce_editor -module MyGame/build/libmy_game.so -project MyGame
mce mcp MyGame                 # mce_mcp --module ... --project MyGame, over stdio (chapter 14)
mce cook MyGame --pack         # mce_cook --module ... MyGame --pack (the build cooks too)
mce ship MyGame --version 1.0  # MyGame/dist/MyGame-1.0-linux.zip: host, module, libmce_engine, pack
```

The project is the same folder the checkout build uses (chapter 2); its `CMakeLists.txt` begins
with `find_package(MCE)` when it is built on its own, so plain CMake works as well:

```sh
cmake -S MyGame -B MyGame/build -G Ninja -DCMAKE_PREFIX_PATH=~/mce-sdk
cmake --build MyGame/build
MyGame/build/my_game
```

Against the SDK the build makes `my_game` (the host), `libmy_game.so` (the module) and the pack;
the cook, the editor and the MCP server are the installed ones with the module loaded, so there
are no `my_game_cook`, `my_game_editor` or `my_game_mcp` targets.

A module only loads into the engine it was built against: `include/mce/engine_version.h` stamps
your module with the SDK's engine id, and a module from another SDK is refused with
`EngineIdMismatch` (rebuild it). The same stamp is the second line of `share/mce/VERSION`.
