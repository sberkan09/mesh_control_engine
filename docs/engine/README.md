# Engine reference

One page per engine subsystem: what the library is for, its public surface, its dependencies and walls,
how it is tested, and its known gaps. The pages describe the code as built. Where the code and an
architecture doc (`docs/architecture/`) or ADR (`docs/adr/`) differ, the page says what the code does and
points to the record of the difference.

Ordered by layer, bottom up:

| Page | Library | What it covers |
|---|---|---|
| [core](core.md) | `mce_core` (L0) | memory, containers, math, errors, logging, convars, crash reports, the main loop |
| [refl](refl.md) | `mce_refl` (L1) | reflected structs and enums, property paths |
| [ecs](ecs.md) | `mce_ecs` (L1) + `mce_ecs_scene` (L2) | World, phases, Commands, events, change ticks, hierarchy, prefabs, saves |
| [platform](platform.md) | `mce_platform` | SDL3 window, events, clock, gamepads, audio device, OS services |
| [input](input.md) | `mce_input` | tick-quantized sampling, action maps, rebinding |
| [asset](asset.md) | `mce_asset` + `mce_cook` | `.mce` containers, GUIDs, chunk formats, the cook |
| [rhi](rhi.md) | `mce_rhi` + backends | backend-neutral RHI; GL, null and Metal backends |
| [render](render.md) | `mce_render` | 2D sprite path, 3D forward path (PBR, Forward+, shadows, post) |
| [audio](audio.md) | `mce_audio` | float mixer, voices, buses, streams, the ECS bridge |
| [physics2d](physics2d.md) | `mce_physics2d` | hand-written 2D rigid bodies, character controller, queries |
| [physics3d](physics3d.md) | `mce_physics3d` | `World3D` over embedded Jolt 5 |
| [scene](scene.md) | `mce_scene` | cooked scene chunk → entities |
| [kit](kit.md) | `mce_kit` | starter-kit components, particles, sequences, FSM, 2D extract |
| [ui](ui.md) | `mce_ui` | retained widget documents, the shipped developer console |
| [modules](modules.md) | `mce_engine` + module host | the shared engine library, game modules, the player runtime |
| [editor](editor.md) | `mce_editor_*` | scene document, inspector, play-in-editor, hot reload, MCP server |

Decisions behind these pages are indexed in [`docs/adr/README.md`](../adr/README.md). Milestone records
(what each milestone built, with evidence) are in [`docs/milestones/`](../milestones/).
