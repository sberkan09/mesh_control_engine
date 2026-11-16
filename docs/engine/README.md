# Engine reference

One page per engine subsystem: what the library is for, its public surface, its dependencies and walls,
how it is tested, and its known gaps. The pages describe the code as built. Where the code and an
architecture doc (`docs/architecture/`) or ADR (`docs/adr/`) differ, the page says what the code does and
points to the record of the difference.

Ordered by layer, bottom up (the L column is the layer each library's `CMakeLists.txt` header declares):

| Page | L | Library | What it covers |
|---|---|---|---|
| [core](core.md) | L0 | `mce_core` | memory, containers, math, errors, logging, convars, crash reports, the main loop |
| [refl](refl.md) | L1 | `mce_refl` | reflected structs and enums, property paths |
| [ecs](ecs.md) | L1 / L2 | `mce_ecs` (L1) + `mce_ecs_scene` (L2) | World, phases, Commands, events, change ticks, hierarchy, prefabs, saves |
| [platform](platform.md) | L1 | `mce_platform` | SDL3 window, events, clock, gamepads, audio device, OS services |
| [input](input.md) | L2 | `mce_input` | tick-quantized sampling, action maps, rebinding |
| [asset](asset.md) | L2 | `mce_asset` + `mce_asset_cook` (`mce_cook` CLI) | `.mce` containers, GUIDs, chunk formats, the cook, data mods, async loads |
| [rhi](rhi.md) | L2 | `mce_rhi` + backends | backend-neutral RHI; GL and Metal backends, `mce_rhi_default` selector |
| [audio](audio.md) | L2 | `mce_audio` | float mixer, voices, buses, streams, the ECS bridge |
| [physics2d](physics2d.md) | L2 | `mce_physics2d` | hand-written 2D rigid bodies, character controller, queries |
| [physics3d](physics3d.md) | L2 | `mce_physics3d` | `World3D` over embedded Jolt 5 |
| [scene](scene.md) | L2 | `mce_scene` | cooked scene chunk → entities, chunk streaming (`streaming.h`) |
| [render](render.md) | L3 | `mce_render` | 2D sprite path, 3D forward path (PBR, Forward+, shadows, post, skinning — M-SKIN) |
| [kit](kit.md) | L3 | `mce_gameplay_kit` | starter-kit components, particles, sequences, FSM, 2D extract |
| [script](script.md) | L3 | `mce_script` + `mce_script_compiler` | Luau scripts as systems, the `mce` API and `mce.d.luau`, the limits |
| [ui](ui.md) | L4 | `mce_ui` | retained widget documents, the shipped developer console |
| [net](net.md) | L3 | `mce_net` (optional) | opt-in multiplayer: UDP hosts, the message codec, snapshot replication |
| [modules](modules.md) | — | `mce_engine` + module host | the shared engine library, game modules, the player runtime (`mce_player`, L6) |
| [editor](editor.md) | L5 / L6 | `mce_editor_*`, `mce_mcp_core` / `mce_mcp` | scene document, inspector, play-in-editor, hot reload, MCP server |

Decisions behind these pages are indexed in `docs/adr/README.md`. Milestone records
(what each milestone built, with evidence) are in `docs/milestones/`.
