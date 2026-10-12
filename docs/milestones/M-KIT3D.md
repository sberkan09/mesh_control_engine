# M-KIT3D — 3D kit + a 3D path in the stock player (milestone record)

**Status:** done (2026-10-12). Plan: ROADMAP §M-KIT3D; issues #129 (kit + player 3D path), #158 (mesh LOD
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

| # | Step | As built |
|---|---|---|
| 1 | Kit components + render extract | `mce/kit/kit3d.h`: `MeshRef`, `PointLight3D`, `SpotLight3D`, `Camera3DComponent` (with `mouse_look`), `Animator` (named clips, cross-fades, kit-advanced time), `Environment3D` as a scene component and the world resource; `kit::Kit3DExtract` (`kit3d_extract.h`) fills a `Scene3D`, interpolating `interpolate` meshes and the camera between ticks; registered in the cook, the editor and the MCP server |
| 2 | Kit physics | `BoxCollider3D`, `MeshCollider3D` (`kit::mesh_triangles` reads a cooked mesh's rest pose) built at Startup into `kit::Physics3D` (`auto_step`); `kit::StaticInstances3D` for scattered copies drawn by cells; `kit::install_kit3d` (idempotent) |
| 3 | Stock player 3D path | a scene with a `Camera3DComponent` starts the forward renderer (`-r.msaa`, `-r.preload3d`), draws the kit, then the 2D kit, the console and the pause overlay over it; relative mouse for look (#126) released while paused, in the console and unfocused (`-mouse_look`); `-capture_tick`, `-input`, `-save` on 3D games; the exit line reports the last frame's triangles; `projects/Room3D` is the reference project (walk, save/resume and LOD GPU tests) |
| 4 | Editor viewport | `PreviewRenderer::record_3d`: kit3d scenes draw in the orbit view (which frames the scene's camera) and in play-in-editor through the game camera; the editor selftest checks a 3D frame was drawn |
| 5 | Mesh LOD chains (#158) | mesh format minor 2: a per-submesh LOD table; the glTF cooker generates levels with meshoptimizer `simplify` (`lod_levels`, `lod_ratio`, `lod_error`) or attaches authored `<name>_LOD<n>` nodes with their measured error; `Scene3D` picks a level per instance from projected error with hysteresis, shadows one level coarser; `-r.lod`, `-r.lod_pixels`, `-r.lod_force`; triangles per level in the stats line. Room3D's wide view: 453,876 triangles with LOD off, 27,264 on (16.6×) |
| 6 | SolsticeReach on the kit | the 1,059-line host is a `PlayerDesc`; the module's `present.cpp` writes the camera, the hour's `Environment3D`, glows, beams, the Keeper's body, gulls, a `TextLabel` HUD and `PlaySfx` sounds; scatter goes to `StaticInstances3D`, the sim runs on `Physics3D`; the scene was re-authored through MCP; the terrain and determinism test are unchanged and pass. Room3D ships as the SDK's 3D example and `tests/sdk` builds and plays it headless |
| 7 | Docs | user guide chapter 8 rewritten around the kit; chapters 1, 5, 9 and 18 updated; this record; ROADMAP status |

Gates: a 3D SDK project needs only its module (Room3D, SolsticeReach); the far view draws 16.6×
fewer triangles with LOD on (`room3d_lod_gpu` requires 5×); `sdk_install` builds and runs Room3D
headless; the stock player's 3D runs report zero GL validation errors (`room3d_walk_gpu`,
`solstice_reach_walk_gpu`). Found on the way: GPU tests that play to a tick need
`-pause_on_focus_loss 0` under `ctest -j` (they share one X display), and a 3D frame with
thousands of draws needs more than 1 MiB of uniform ring (`-r.uniform_ring_mb`, default 4).

Not done, left for later: dynamic rigid bodies as components (games create them on `Physics3D`
themselves); LODs for skinned meshes; SolsticeReach's photo mode and frame-budget table, which
lived in its host (the stock player's `-capture_tick` and `-r.stats` cover captures and counts);
module-declared convars (modules may not declare them, so SolsticeReach's attract mode starts
from an idle title instead of `-autoplay`).
