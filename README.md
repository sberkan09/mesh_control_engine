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

## Layout

| Path | What |
|---|---|
| `src/engine/core` | `mce_core` (L0): allocators, containers, math, `Result`, log/assert, convars, jobs, the app loop |
| `tests/` | doctest unit tests (`test_<module>`) and `mce_devhost` |
| `scripts/ci/check_walls.py` | GL / SDL / ImGui / JPH / Box2D grep walls and the engine container ban |
| `cmake/` | compile policy (`MceTarget.cmake`) and pinned dependencies (`Dependencies.cmake`) |

## Status

M0 in progress: `mce_core` and the headless loop are in; platform (SDL3), RHI/GL triangle and the editor shell
are next. See ROADMAP §2.
