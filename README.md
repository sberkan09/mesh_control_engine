# Mesh Control Engine

A from-scratch 2D/3D game engine with an integrated editor. The design lives in `docs/`:
[REQUIREMENTS](docs/REQUIREMENTS.md), [ARCHITECTURE](docs/ARCHITECTURE.md), [ROADMAP](docs/ROADMAP.md).
Making a game with it starts at the [user guide](docs/user-guide/README.md); the per-subsystem reference is
[`docs/engine/`](docs/engine/README.md).

## Build

Requires CMake ≥ 3.28, Ninja, and MSVC 2022 (Windows), gcc 13 / clang 18 (Linux) or AppleClang from Xcode
(macOS, Apple silicon, Metal backend). Dependencies are fetched at configure time (ADR-0017).

```sh
cmake --preset linux-gcc-debug        # or msvc-debug, macos-debug, linux-clang-release, ... (CMakePresets.json)
cmake --build --preset linux-gcc-debug
ctest --preset linux-gcc-debug
```

Run the headless harness: `build/<preset>/tests/devhost/mce_devhost -ticks 10000`.
Run the first sample: `build/<preset>/samples/01_triangle/01_triangle` (GL 4.5 on Linux/Windows, Metal on
macOS; Esc quits).

On Linux, SDL3 builds against the X11 headers: `sudo apt-get install libgl-dev libegl-dev libx11-dev libxext-dev
libxcursor-dev libxrandr-dev libxi-dev libxss-dev libxtst-dev libxfixes-dev libxkbcommon-dev`. Tests that need a
display and a GPU driver register with `-DMCE_GPU_TESTS=ON` (the local CI script runs them under Xvfb + Mesa
llvmpipe). `test_physics2d` also steps its oracle scenes in Box2D v3 (ADR-0016); `-DMCE_PHYSICS_ORACLE=OFF` skips
that download when you only need the in-house tests.

Cook a project (`<project>/assets/` → `<project>/cooked/`, plus `assets.mce` with `--pack`):
`build/<preset>/tools/cook/mce_cook <project> [--pack] [--force] [--adopt]` — exit 0 ok, 1 cook errors, 2 usage.
The full flag list (`--module`, `--base`, `--quiet`, ...) is in
[user guide chapter 16](docs/user-guide/16-command-line.md).

## CI

There is no hosted CI. Before merging a PR, run `scripts/ci/local_ci.sh` on Linux (all stages) or
`scripts/ci/local_ci.sh --quick` (walls + gcc debug with GPU tests); it prints a pass/fail summary and keeps logs
in `build/local-ci-logs/`. On a Mac, `scripts/ci/local_ci_macos.sh` (or `--quick`) does the same for the
`macos-*` presets. Windows/MSVC builds are checked by hand on a Windows machine.
`scripts/ci/setup_env.sh` installs its Ubuntu 24.04 packages (cloud sessions run it on start through
`.claude/settings.json`). With ccache installed, rebuilds of an emptied `build/` are cached.

## Layout

| Path | What |
|---|---|
| `src/engine/core` | `mce_core` (L0): allocators, containers, math, `Result`, log/assert, convars, jobs, crash reports, the app loop |
| `src/engine/refl` | `mce_refl`: reflected structs and enums, property paths |
| `src/engine/ecs` | `mce_ecs`: World, phases, Commands, events, hierarchy, prefabs, saves |
| `src/engine/platform` | `mce_platform`: SDL3 window + events, clock, gamepads, audio device, OS services (the SDL wall) |
| `src/engine/input` | `mce_input`: tick-quantized sampling, action maps, rebinding |
| `src/engine/asset` | `mce_asset`: `.mce` containers, GUIDs, chunk formats, runtime registry; `asset/cook` is the cook library (importers, dependency graph, pack) |
| `src/engine/rhi` | `mce_rhi`: backend-neutral handles, `RenderDevice`, recorded `CommandBuffer` |
| `src/engine/rhi_gl`, `rhi_metal` | the OpenGL 4.5 core backend (Linux, Windows) and the Metal backend (macOS) |
| `src/engine/rhi_default` | picks the native backend for a host (`rhi::create_default_device`) |
| `src/engine/render` | `mce_render`: 2D sprite path, 3D forward path (PBR, Forward+, shadows, skinning, post) |
| `src/engine/audio` | `mce_audio`: mixer, voices, buses, streams, the ECS bridge |
| `src/engine/physics2d`, `physics3d` | the in-house 2D physics, and `World3D` over Jolt |
| `src/engine/scene` | `mce_scene`: cooked scene → entities, scene streaming |
| `src/engine/kit` | `mce_kit`: starter-kit components, particles, sequences, FSM |
| `src/engine/ui` | `mce_ui`: retained widget documents, the developer console |
| `src/module` | `mce_engine` (the shared engine library) and the game-module host |
| `src/player` | the player runtime and `mce_player` |
| `src/editor` | the editor (`mce_editor`) and its MCP server (`mce_mcp`) |
| `tools/cook` | the `mce_cook` CLI |
| `tools/mce_cli` | `mce`, the SDK's project command (new, build, play, editor, ship, ...) |
| `samples/` | the sample ladder; see [user guide chapter 18](docs/user-guide/18-samples.md) |
| `projects/` | complete games made from the template ([projects/README.md](projects/README.md)) |
| `templates/project` | the project template stamped by `scripts/new_project.py` / `mce new` |
| `third_party/glad` | generated glad2 GL 4.5 core loader (private to `mce_rhi_gl`) |
| `tests/` | doctest unit tests (`test_<module>`) and `mce_devhost` |
| `scripts/` | `new_project.py`, `ship.py`, and `ci/` (local CI for Linux and macOS, the grep walls, determinism) |
| `cmake/` | compile policy (`MceTarget.cmake`), pinned dependencies (`Dependencies.cmake`), SDK install, macOS bundling |

The per-subsystem reference for each engine library is in [`docs/engine/`](docs/engine/README.md).

## Reporting bugs and requests

Open a GitHub Issue; see [CONTRIBUTING.md](CONTRIBUTING.md) for the `gh issue create` recipe and how issues are triaged.

## Status

Milestones M0–M9 are built, along with the M4.5 editor MCP server, the M-MAC macOS port (Metal backend),
optional skinned animation, scene streaming and data mods. What each milestone delivered is recorded in
[`docs/milestones/`](docs/milestones/); the plan and what remains deferred are in [ROADMAP](docs/ROADMAP.md).
