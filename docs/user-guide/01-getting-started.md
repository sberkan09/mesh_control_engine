# 1. Getting started

By the end of this chapter you will have built the engine, run a sample, created a project of
your own, played it, and opened it in its editor.

> **Two ways to use the engine.** From its **source checkout** (sections 1.2 to 1.8): your project
> is a folder under `projects/` and the engine's CMake builds it together with the engine. Or from
> the **installed SDK** (section 1.9): a prefix with the prebuilt tools, `libmce_engine`, the headers
> and a CMake package; your project lives anywhere, builds with `find_package(MCE)` or the `mce`
> command, and never sees the engine's source. The rest of the guide applies to both; where a
> command differs, the SDK form is given beside the checkout form.
>
> **On a Mac?** The commands below use the Linux presets and paths; section 1.10 lists what changes
> on macOS.

## 1.1 Requirements

| | Linux | Windows | macOS |
|---|---|---|---|
| Build tools | CMake ≥ 3.28, Ninja, gcc 13 or clang 18 | CMake ≥ 3.28, Ninja, Visual Studio 2022 (MSVC) | CMake ≥ 3.28, Ninja, Xcode toolchain (AppleClang 17) |
| Graphics | OpenGL 4.5 core driver (Mesa llvmpipe works, slowly) | OpenGL 4.5 driver | Metal (built in) |
| Python | 3.x for the helper scripts (`new_project.py`, `ship.py`) | same | same |
| Presets | `linux-gcc-debug`, `linux-gcc-release`, `linux-clang-debug`, `linux-clang-release`, `linux-asan` | `msvc-debug`, `msvc-release`, `msvc-asan` | `macos-debug`, `macos-release`, `macos-asan` (UBSan only: ASan is off because its runtime hangs at start-up on macOS 26.6) |
| OS | | | Apple silicon, macOS 26.0 or later (the presets' deployment target) |

Third-party dependencies (SDL3, Jolt, ImGui, spdlog, Tracy, glslang, stb, meshoptimizer; doctest,
DirectXMath and Box2D for the tests; SPIRV-Cross on macOS) are fetched at configure time; no package
manager is needed for them. Each preset downloads them on its first configure into
`build/<preset>/_deps/` (the local CI script shares one copy between presets under `build/_fetch/`).

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
| `MCE_GPU_TESTS` | OFF | also register tests that need a display and a GPU driver (GL 4.5, or Metal on macOS) |
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
scripts/ci/local_ci_macos.sh --quick   # on a Mac: walls + macos-debug with GPU tests (no flag: every stage)
```

## 1.9 The installed SDK

The SDK is the engine as a normal, installed engine: no source checkout, no engine build on your
side. It exists for Linux and macOS (the platforms with game modules; on Windows use the checkout).

### Install it

The short way, from an engine checkout (macOS or Linux):

```sh
./install.sh                 # checks the build tools, builds the release preset, installs to ~/mce-sdk, adds it to PATH
./install.sh mce-sdk-<version>-macos-arm64.tar.gz   # or unpack a prebuilt SDK archive instead of building
./install.sh --release       # or download the archive for this machine from the GitHub releases (needs `gh`, signed in)
```

`--prefix <dir>` installs elsewhere, `--no-path` leaves your shell profile alone, and `--yes`
installs missing tools (CMake, Ninja) through Homebrew or apt without asking. On a Mac without the
Command Line Tools it opens Apple's installer and asks you to run it again afterwards. The archives
come from `scripts/release_sdk.sh`, which builds the release preset and writes
`dist/mce-sdk-<version>-<os>-<arch>.tar.gz`; upload it to a GitHub release (`gh release create
v<version> dist/mce-sdk-*.tar.gz`) and `./install.sh --release` finds it. An archive is built
for one OS and CPU, so make the macOS one on a Mac.

**Updating.** `mce update` repeats the install the way it was made: for a source install it runs
`git pull --ff-only` in the checkout and `./install.sh` again (the build tree is kept, so only what
changed recompiles); for a `--release` install it downloads the latest release (`--release <tag>`
picks one). A game module only loads into the engine it was built against, so rebuild your
projects afterwards; name them and `mce update` does it:

```sh
mce update ~/Projects/MyGame ~/Projects/OtherGame   # pull, reinstall, rebuild both projects
```

Close the editor and any running game first. An SDK unpacked from an archive file has nowhere to
update from: run `./install.sh` with the newer archive.

The same by hand, once (a release build is the one to install):

```sh
cmake --preset linux-gcc-release
cmake --build --preset linux-gcc-release
cmake --install build/linux-gcc-release --prefix ~/mce-sdk
```

`~/mce-sdk` then holds:

| Path | What |
|---|---|
| `bin/` | `mce` (the command below), `mce_editor`, `mce_mcp`, `mce_cook`, `mce_player`, `mce_devhost` |
| `lib/` | `libmce_engine.so` (`.dylib` on macOS; the engine; every tool and every game module shares it), `libmce_player_runtime.a` (what your host links) |
| `include/` | the engine headers (`mce/gameplay.h` is the one a game module includes), `mce/engine_version.h` (this engine's id) |
| `lib/cmake/MCE/` | `MCEConfig.cmake` for `find_package(MCE)`, and the `mce_add_project` / `mce_add_game_module` helpers |
| `share/mce/` | the project template, `new_project.py`, `VERSION`, and `examples/Kindle` (chapter 18; `mce example Kindle` copies it out) |
| `share/doc/mce/` | this guide (`user-guide/`), the engine and architecture pages, `ARCHITECTURE.md`, `REQUIREMENTS.md` |

Put `~/mce-sdk/bin` on your `PATH`, or call `~/mce-sdk/bin/mce` by path. The SDK needs the same
tools a project build needs (CMake 3.28+, Ninja, a C++20 compiler of the same family the engine
was built with: `mce version` prints the engine's build stamp) and Python 3 for `mce` itself.

### Make, build, play, edit, ship a project

```sh
mce new MyGame                 # -> ./MyGame (anywhere; --dest picks the parent folder)
mce example Kindle             # or start from the example game instead (mce example lists them)
mce build MyGame               # cmake -S MyGame -B MyGame/build -DCMAKE_PREFIX_PATH=<sdk> + build (--config Release for a release build)
mce play MyGame                # the game; flags after the project pass through (chapter 16)
mce headless MyGame --ticks 600   # no window; prints the state hash
mce editor MyGame              # mce_editor -module MyGame/build/libmy_game.so -project MyGame
mce mcp MyGame                 # mce_mcp --module ... --project MyGame, over stdio (chapter 14)
mce cook MyGame --pack         # mce_cook --module ... MyGame --pack (the build cooks too)
mce ship MyGame --version 1.0  # MyGame/dist/MyGame-1.0-linux-x64.zip (-macos.zip on a Mac): host, module, libmce_engine, pack
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

## 1.10 On a Mac

The engine runs on Apple silicon Macs with macOS 26.0 or later, rendering through Metal (ADR-0028,
`docs/milestones/M-MAC.md`). Only Apple silicon has been tested; older macOS versions are not supported (the presets' deployment
target is 26.0).

**Tools, once:**

```sh
xcode-select --install        # Apple's compilers and the Metal framework (the Command Line Tools are enough)
brew install cmake ninja      # or any other source of CMake >= 3.28 and Ninja; python3 comes with the tools above
```

The full Xcode app is only needed to open GPU captures (`MCE_METAL_CAPTURE`, M-MAC.md).

**From the checkout:** every command in sections 1.2 to 1.8 works with `macos-debug` (or
`macos-release`) in place of `linux-gcc-debug`, and `build/macos-debug/` in place of
`build/linux-gcc-debug/`:

```sh
cmake --preset macos-debug
cmake --build --preset macos-debug
build/macos-debug/samples/03_platformer/sample_03_platformer
python3 scripts/new_project.py MyGame && cmake --preset macos-debug
cmake --build --preset macos-debug --target my_game my_game_editor my_game_mcp
build/macos-debug/projects/MyGame/my_game_editor -project projects/MyGame
```

Game modules keep the `.so` suffix on macOS (`libmy_game.so`); only the engine library is a
`.dylib`. The local CI for engine changes is `scripts/ci/local_ci_macos.sh` (section 1.8).

**The SDK:** `./install.sh` in the checkout builds `macos-release`, installs it to `~/mce-sdk` and
adds it to PATH in `~/.zshrc` (section 1.9; given a prebuilt `mce-sdk-…-macos-arm64.tar.gz` it
skips the build). Then use `mce` exactly as in section 1.9:

```sh
./install.sh                  # open a new terminal afterwards so PATH picks up ~/mce-sdk/bin
mce new MyGame && mce build MyGame && mce editor MyGame
```

**Shipping:** `python3 scripts/ship.py --project projects/MyGame` (from the checkout) writes a zip
holding `<Name>.app`, stripped and signed ad hoc (chapter 15, section 15.4). `mce ship` from the SDK
writes a plain folder (host, module, `libmce_engine.dylib`, pack) instead of an `.app`; use
`ship.py` for a build you hand to other people. Neither is Developer ID signed or notarized, so on
another Mac the player right-clicks the app and chooses Open the first time (or removes the
download flag with `xattr -dr com.apple.quarantine <Name>.app`).

**Known limits on macOS:**

- `macos-asan` runs UBSan only: the AddressSanitizer runtime hangs at start-up on macOS 26.6.
- Sound: if the Mac has not granted audio access, every CoreAudio client, the game included, blocks
  for about 15 s and then fails with `AudioQueueStart -66681`; the game keeps running silently.
  Allow audio for the app that starts the game (Terminal, your IDE, or the `.app`) under System
  Settings > Privacy & Security and start it again.
