# M-KIT3D — 3D kit + a 3D path in the stock player (milestone record)

**Status:** in progress (started 2026-10-12). Plan: ROADMAP §M-KIT3D; issues #129 (kit + player 3D path), #158 (mesh LOD
chains), #126 (relative mouse).
**Why:** 3D in the SDK is code-driven. With no 3D components and a stock player that draws only the 2D kit, every 3D
project copies SolsticeReach's mini kit (`MeshRef`, `Light3D`, `BoxCollider3D`, `Scatter`, animation selection) and
its ≈ 1,000-line host, and loses the stock player's `-capture_tick`, `-save`, `-input` and `-pause_on_focus_loss`.

## Steps

Each step is one PR into `dev_claude`, verified with `scripts/ci/local_ci.sh` before merge.

| # | Step | Builds |
|---|---|---|
| 1 | Kit components + render extract | `mce/kit/kit3d.h`: `MeshRef`, `PointLight3D`, `SpotLight3D`, `Camera3DComponent`, `Animator`; the `Environment3D` world resource (sun, sky, fog, post, shadows, exposure); `kit::Kit3DExtract` filling a `Scene3D` from them; registered in the cook, the editor and the MCP server |
| 2 | Kit physics | `BoxCollider3D` / `MeshCollider3D` backed by a `kit::Physics3D` resource (`World3D`) built at Startup; `kit::install_kit3d` |
| 3 | Stock player 3D path | a scene with a `Camera3DComponent` runs `Renderer::init_3d`, uploads the pack's textures and meshes, extracts the kit every frame and draws the 2D kit and `mce_ui` on top; `-capture_tick`, `-save`, `-input` on 3D games; relative mouse for look (#126), released while the console or the pause overlay is open |
| 4 | Editor viewport | kit3d scenes drawn in the editor viewport, so MCP-authored 3D scenes are visible |
| 5 | Mesh LOD chains (#158) | the cook builds LODs with meshoptimizer `simplify` (per-asset levels and target error) or imports authored `_LOD<n>` meshes; the renderer picks a level by screen size with hysteresis, shadows use a coarser one; triangles per level in the stats |
| 6 | SolsticeReach on the kit | its host reduced to a thin `PlayerDesc`, the kit replacing its own components, its terrain and determinism test unchanged and passing; a 3D example built and run headless by `tests/sdk` |
| 7 | Docs | user guide ch. 8 rewritten around the kit; this record completed; ROADMAP status |

## Gates (ROADMAP)

- a 3D SDK project needs only its module;
- a far-view capture of the 3D example draws at least 5× fewer triangles with LOD on;
- `tests/sdk` builds and runs the 3D example headless;
- GL validation shows zero errors on the stock player's 3D path.

## What shipped

Filled in as each step merges.
