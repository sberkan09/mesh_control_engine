# 1. Getting started

By the end of this chapter you will have built the engine, run a sample, created a project of
your own, played it, and opened it in its editor.

> **Where the engine lives.** Today the engine is used from its source checkout: your project is a
> folder under `projects/` and the engine's CMake builds it together with the engine. An
> installable SDK is being built in parallel; until it lands, follow this chapter.

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
| `mce_editor` | `src/editor/app/mce_editor` | the stock scene editor (engine components only) |
| `mce_player` | `src/player/mce_player` | the stock player: runs a cooked pack with the engine systems |
| `mce_cook` | `tools/cook/mce_cook` | the asset cook (engine components only) |
| `mce_mcp` | `src/editor/mcp/mce_mcp` | the editor's MCP server for AI agents (engine components only) |
| samples | `samples/<name>/<target>` | the sample ladder, for example `samples/03_platformer/sample_03_platformer` |

"Engine components only" matters: once your game defines its own components, you use the
per-project editor, cook and MCP server that the build makes for you (section 1.4), not the stock
ones.

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
`build/<preset>/projects/MyGame/project/assets.mce`, the pack the game loads.

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
