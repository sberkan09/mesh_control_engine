# Mesh Control Engine

A from-scratch 2D/3D game engine with an integrated editor. The design lives in `docs/`:
[REQUIREMENTS](docs/REQUIREMENTS.md), [ARCHITECTURE](docs/ARCHITECTURE.md), [ROADMAP](docs/ROADMAP.md).

## Build

Requires CMake ≥ 3.28, Ninja, and MSVC 2022 (Windows) or gcc 13 / clang 18 (Linux). Dependencies are fetched
at configure time (ADR-0017).

```sh
cmake --preset msvc-debug        # or linux-gcc-debug, linux-clang-release, ...
cmake --build --preset msvc-debug
ctest --preset msvc-debug
```

Run the headless harness: `build/<preset>/tests/devhost/mce_devhost -ticks 10000`.
Run the first sample: `build/<preset>/samples/01_triangle/01_triangle` (needs a GL 4.5 driver; Esc quits).

On Linux, SDL3 builds against the X11 headers: `sudo apt-get install libx11-dev libxext-dev libxcursor-dev
libxrandr-dev libxi-dev libxss-dev libxtst-dev libxfixes-dev libxkbcommon-dev libgl-dev`. Tests that need a display
and a GL driver register with `-DMCE_GPU_TESTS=ON` (the local CI script runs them under Xvfb + Mesa llvmpipe).
`test_physics2d` also steps its oracle scenes in Box2D v3 (ADR-0016); `-DMCE_PHYSICS_ORACLE=OFF` skips that
download when you only need the in-house tests.

Cook a project (`<project>/assets/` → `<project>/cooked/`, plus `assets.mce` with `--pack`):
`build/<preset>/tools/cook/mce_cook <project> [--pack] [--force] [--adopt]` — exit 0 ok, 1 cook errors, 2 usage.

## CI

There is no hosted CI. Before merging a PR, run `scripts/ci/local_ci.sh` on Linux (all stages) or
`scripts/ci/local_ci.sh --quick` (walls + gcc debug with GPU tests); it prints a pass/fail summary and keeps logs
in `build/local-ci-logs/`. Windows/MSVC builds are checked by hand on a Windows machine.

## Layout

| Path | What |
|---|---|
| `src/engine/core` | `mce_core` (L0): allocators, containers, math, `Result`, log/assert, convars, jobs, the app loop |
| `src/engine/asset` | `mce_asset` (L2): `.mce` container, GUIDs, cooked chunk formats, runtime asset registry (handles, refcount, hot-reload commit) |
| `src/engine/asset/cook` | `mce_asset_cook`: cook library — scan, sidecars, importers (PNG/JPG, WAV, TTF, glTF/GLB, `.mcmat`, `.mcscene`), dependency graph, cook DB, pack |
| `tools/cook` | `mce_cook` CLI |
| `src/engine/platform` | `mce_platform` (L1): SDL3 window + event pump, clock, boot-failure message box (the SDL wall) |
| `src/engine/rhi` | `mce_rhi` (L2): backend-neutral handles, `RenderDevice`, recorded `CommandBuffer` |
| `src/engine/rhi_gl` | `mce_rhi_gl` (L2): the OpenGL 4.5 core backend (DSA, persistent-mapped rings, fences) — the only GL code |
| `samples/` | the sample ladder: `01_triangle`, `02_boids`, `sprites_2d`, `03_platformer` (Artifact A), `04_diorama` (Artifact B: the lit 3D diorama of Kenney CC0 kit meshes, cooked at build time) |
| `third_party/glad` | generated glad2 GL 4.5 core loader (private to `mce_rhi_gl`) |
| `tests/` | doctest unit tests (`test_<module>`) and `mce_devhost` |
| `scripts/ci/local_ci.sh` | local CI (no GitHub Actions): walls, Linux presets + tests, clang-tidy, determinism |
| `scripts/ci/check_walls.py` | GL / SDL / ImGui / JPH / Box2D / stb / cook grep walls and the engine container ban |
| `cmake/` | compile policy (`MceTarget.cmake`) and pinned dependencies (`Dependencies.cmake`) |

## Status

M0 in progress: `mce_core`, the headless loop, `mce_platform` (SDL3), `mce_rhi` + `mce_rhi_gl` and `01_triangle`
are in; the editor shell is next. See ROADMAP §2.
