# Architecture decision records

ADR-0001 to ADR-0016 were decided with the architecture and are written up in
[`docs/ARCHITECTURE.md` §9](../ARCHITECTURE.md). Only those that
later needed a full record have a file here (0003, 0016). ADR-0017 onward are records of what was built,
written as each milestone merged. All are **Accepted**.

Two numbers were issued twice by parallel tracks (0024, 0025). Both files of each pair stand; cite them by
file name when the number alone is ambiguous.

## Decided with the architecture (ARCHITECTURE §9)

| ADR | Decision |
|---|---|
| 0001 | Two-layer RHI seam (`mce_rhi` + `mce_rhi_gl`), GL confined by CI grep |
| 0002 | No frame graph at MVP; fixed pass list with pooled transients |
| [0003](0003-embed-jolt-for-3d.md) | Embed Jolt 5 for 3D physics; hand-write all 2D physics |
| 0004 | Archetype tables over sparse sets |
| 0005 | One frame-slot constant; fence-owned memory across the RHI seam |
| 0006 | MDI + per-object SSBO; fixed binding banks |
| 0007 | Determinism defined narrowly, enforced by CI from M1 |
| 0008 | Editor process model: two executables, one library wall |
| 0009 | Reflection is a standalone L1 module and the only metadata source |
| 0010 | Native C++20 game module DLL; no scripting VM; copy-and-load |
| 0011 | Content-addressed cooked outputs + GUID sidecars |
| 0012 | Audio crosses threads via one SPSC POD command FIFO |
| 0013 | Input sampled per tick from event timestamps |
| 0014 | `mce_ui` designed now; ImGui never ships |
| 0015 | Editor file watch is a 500 ms poll owned by the editor |
| [0016](0016-box2d-test-oracle.md) | Box2D v3 is a test-time oracle, never a runtime dependency |

## Records of what was built

| ADR | Milestone | Decision |
|---|---|---|
| [0017](0017-fetchcontent-dependencies.md) | Build | Third-party dependencies via CMake FetchContent, pinned tags |
| [0018](0018-portable-build.md) | Build | Linux presets beside MSVC, CI on both |
| [0019](0019-asset-pipeline-m2.md) | M2 | Asset pipeline: stb, hand-written cook JSON, container layout |
| [0020](0020-2d-render-path.md) | M3 | The 2D render path as built |
| [0021](0021-game-module-loader-m3.md) | M3 | Game-module loader and the `mce_engine` shared library |
| [0022](0022-gltf-static-import-m4.md) | M4 | glTF static import and `CHNK_MESH` |
| [0023](0023-3d-render-path-m4.md) | M4 | MVP 3D render path |
| [0024](0024-pbr-forward-plus-csm-m5a.md) | M5a | PBR, Forward+, cascaded shadows, sky and post |
| [0024](0024-runtime-scene-path.md) | M3–M4 | Runtime scene path: reflection-addressed writes, `mce_scene`, starter kit, `mce_player` |
| [0025](0025-ecs-v05-change-ticks-hierarchy-prefabs.md) | M6 | ECS v0.5: change ticks, hierarchy, prefabs, saves, replays |
| [0025](0025-editor-scene-authoring.md) | M2–M4 | Editor scene authoring: 2D gizmo, child-process Play, autosave |
| [0026](0026-m6-subsystems.md) | M6 | Audio v0.5, input persistence + gamepads, physics2d v0.5 |
| [0027](0027-editor-mcp-server.md) | M4.5 | Editor MCP server |
| [0028](0028-metal-backend.md) | M-MAC | Metal backend, GLSL → MSL at pipeline creation |
| [0029](0029-play-in-editor.md) | M5b | Play-in-editor, hot reload, orbit-camera gate |
| [0030](0030-m6-editor-authoring.md) | M6 | Editor authoring: undo, gizmos, hierarchy, prefabs, asset browser, tilemaps |
| [0031](0031-m6-2d-extras.md) | M6 | Particles, parallax, lit 2D, 2D post |
| [0032](0032-sequence-runner.md) | M6 | SequenceRunner action lists |
| [0033](0033-shader-cook.md) | M6 | Shader cook: validation, variants, SPIR-V reflection |
| [0034](0034-data-mods.md) | post-M9 | Data mods, opt-in per game: mod packs, load order, cooking against a game pack |
| [0035](0035-optional-networking.md) | M-NET | Optional networking module `mce_net`: client-server over ENet, snapshot replication |
