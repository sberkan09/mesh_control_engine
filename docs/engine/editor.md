# editor — the scene editor: document, mutation funnel, inspector, play-in-editor, hot reload, MCP server

The editor lives in `src/editor/` and is built when `MCE_BUILD_EDITOR` is on. It has five targets
(`src/editor/CMakeLists.txt` and the per-directory CMakeLists):

| Target | Layer | Dependencies | Role |
|---|---|---|---|
| `mce_editor_core` | L5 | `mce_core`, `mce_rhi`, `mce_render`, `mce_scene`, `mce_gameplay_kit`, `mce_asset_cook`, `mce_physics2d`, `mce_physics3d`, `mce_audio` | headless editor logic, no ImGui, no window; sees the RHI interface only |
| `mce_editor_layer` | L5 | public `mce_editor_core`, `mce_platform`; private `imgui`, `SDL3-static` | the only ImGui consumer: backends, dock shell, panels |
| `mce_editor` | L6 | `mce_editor_layer`, `mce_rhi_default`, `mce_module_host` | the executable, a thin `main()` in `app/main.cpp` |
| `mce_mcp_core` | L5 | public `mce_editor_core`, `mce_input` | MCP JSON-RPC protocol, tool table, value codec, headless run |
| `mce_mcp` | L6 | `mce_mcp_core`, `mce_module_host` | the MCP stdio transport executable |

Where game modules are supported the editor, the MCP server, the cook and their libraries are
`UMBRELLA` targets (modules.md): they link the `mce_engine` umbrella instead of the static engine
libraries, so `mce_editor -module <lib<game>.so> -project <dir>` loads a project's game module in
process (`open_game_module` at startup, kept mapped until the shell is gone): its components show in
the inspector and cook, and its `install` runs in play-in-editor through
`editor::set_play_game_install`. `mce_add_project` also builds a per-project `<game>_editor` from
`app/main.cpp` plus the game sources, which registers `install` the same way; on Windows (no
umbrella) that is the only way.

## Public surface

`mce_editor_core` (`core/include/mce/editor/`):

- `SceneDocument` (`scene_document.h`): the canonical, serialized scene or prefab (`DocKind`). Entities
  have a persistent `EntityId` and a sorted list of `DocComponent`s (raw bytes of the reflected
  type). `parse` / `serialize` use the cook's `.mcscene` codec (`mce/cook/scene_text.h`), so what
  the editor saves is what the cook accepts. Field-mask helpers serve prefab overrides.
- `EditorScene` (`editor_scene.h`): the mutation funnel. It owns one `SceneDocument`, the derived
  preview `World`, the `SelectionSet` and the `EntityId` to `Entity` map. Commands:
  `create_entity`, `create_prefab_instance`, `destroy_entity`, `rename_entity`, `move_entity`,
  `add_component`, `remove_component`, `set_component`, `set_parent`, copy/paste/duplicate,
  `revert_override`, `apply_override`. Undo: `begin_edit` / `end_edit`, `seal_undo`, `undo`,
  `redo`, `reload_text`. Preview: `set_assets`, `refresh_prefabs`, `rebuild_world`,
  `verify_world`. Files: `open`, `save` (temp file plus rename), `recover`.
- `DrawCtx` and `draw_inspector` (`inspector.h`): the reflection-driven inspector. It walks
  `mce_refl` field records and dispatches widgets by `FieldKind` through `DrawCtx`, the editor's only
  widget seam. Tests drive it with a scripted `DrawCtx`; `layer/src/imgui_draw_ctx.cpp`
  (`ImGuiDrawCtx`) is the ImGui adapter. `override_mark` and `override_actions` carry prefab
  override display, revert and apply.
- `EditorSession` (`editor_session.h`): the open project plus the open scene: new, open, save,
  save as, cook, autosave and recovery, `enter_play`, `tick_hot_reload`, `apply_to_prefab`,
  `rename_asset`.
- `EditorProject` (`project.h`): project folder, GUID catalog, synchronous cook through
  `CookService`, fresh mount of `cooked/` after every cook. For a mod project (ADR-0034),
  `base_roots()` lists the game packs named by `mod.json` "base", mounted after the cooked tree so the
  mod's scenes see the game's assets (empty for an ordinary project).
- `PlaySession` (`play_session.h`): play-in-editor. `enter` cooks the document in memory
  (`cook::cook_document`) and `scene::instantiate`s it into a fresh World with physics2d and the
  audio bridge on a Silent device, as `mce_player -headless` does. `advance`, `pause`, `resume`,
  `step`, `set_time_scale`, `state_hash`, `enter_hash`.
- `AssetWatcher` and `HotReloadResult` (`hot_reload.h`): polls `assets/` for changes.
- `AssetBrowserModel`, `drop_asset` (`asset_browser.h`); `TransformGizmo` (`gizmo.h`, move, rotate,
  scale, hand-rolled); `EditorCamera2D`, `pick_entity` (`viewport2d.h`, CPU picking);
  `PreviewRenderer` (`preview_renderer.h`, the kit extract plus overlays); `OrbitCamera`,
  `OrbitColliders` (`orbit_camera.h`, the 3D orbit view gated by `World3D::camera_sweep`);
  `TilemapDocument`, `TilemapEditor` (`tilemap_editor.h`); `ConsoleModel` (`console_model.h`);
  `PerfMonitor` (`perf_monitor.h`, the Profiler panel's data); `StubWorld` (`stub_world.h`, the M0
  viewport content shown when no project is open).

`mce_editor_layer` (`layer/include/mce/editor/`): `EditorShell` and `IEditorPanel` (`editor_shell.h`,
ImGui-free header: docking shell, panel registry, menus, per-frame `build_ui` and `record`),
`add_default_panels`, and `draw_clip.h` (CPU clipping of ImGui triangles, since `mce_rhi` has no
scissor state). ImGui platform and renderer backends are in `layer/src/` and render through
`mce_rhi`.

`mce_mcp_core` (`mcp/include/mce/mcp/`): `Server` (`server.h`: `open_project`, `handle(line)`,
`notify`), `run_headless` (`headless_run.h`), `JsonWriter` (`json_writer.h`) and the value codec
(`values.h`). Tools (`mcp/src/tools.cpp`): `refl_list_components`, `refl_describe_component`,
`scene_info`, `scene_list_entities`, `scene_get_entity`, `asset_list`, `asset_set_import_settings`, `asset_delete`,
`entity_create`, `entity_delete`, `entity_duplicate`, `scene_clear`, `component_add`, `component_remove`, `component_set`,
`scene_new`, `scene_open`, `scene_save`, `scene_undo`, `scene_redo`, `cook_run`, `cook_errors`,
`run_headless`, `console_list`, `console_exec`.

Project MCP server (M9): a scene that uses a game's components cannot be opened or edited without
them (`unknown component type`). `mce_mcp --module <lib<game>.so> --project projects/<Name>
[--scene scenes/main.mcscene]` loads the project's game module (`Server::load_module`, before
`open_project` so the first cook knows the components) and hands its `install` to
`editor::set_play_game_install`, so `run_headless` runs the game's systems and hashes like
`<game> -headless 1`. `mce_add_project` also builds `<game>_mcp`: `mce_mcp`'s `app/main.cpp` and
`engine_components.cpp` plus the project's `TYPES` and `GAME_SOURCES` linked against `mce_mcp_core`,
with a generated hook registering `install`; the only route on Windows. The client speaks JSON-RPC 2.0,
one message per line, on its stdin/stdout (`initialize`, `notifications/initialized`, then
`tools/call` with `{"name", "arguments"}`; each result's `content[0].text` is the tool's JSON).
Values use the `.mcscene` spelling: vectors as arrays, assets as `"guid:<uuid>"` (take them from
`asset_list`), enums by name, nested structs as objects (`{"shape": {"type": "Circle", "radius":
0.3}}`). `entity_duplicate` inserts the copy right after its source, and `scene_new` makes a fresh
GUID, so to re-author a scene a `.mceproj` names, `scene_open` it and `scene_clear` it (one undo step)
instead. Entity arguments take an id or a unique name; ids are written into the `.mcscene` (`"id"`) on
save, so they name the same entities after a `scene_open`. `scene_save` with `path` gives a new file a
fresh GUID and keeps the GUID of a file overwritten with `force`; `asset_delete` removes a source file
and its sidecar. `run_headless` takes `scene` (another saved scene by path), `input` (scripted actions,
`mce/input/script.h`) and `inspect` (`{"entities": [ids or names], "component": "Type", "show": ["Type"]}`):
after the last tick it reports the live component values, Transient runtime fields included, of
scene entities by id and of every entity with a component (also ones the run spawned), at most 64.
New source files under `assets/` (a PNG with no `.mceasset`) are a cook error until `cook_run` with
`"adopt": true` writes their sidecars (with empty settings: a normal map or a LUT then needs
`asset_set_import_settings` with `{"srgb": false}`, which rewrites the sidecar's `settings` and rescans). `projects/Kindle/tools/author_level_mcp.py` is a complete client.

## Rules and invariants

- Two-world model (editor-tools.md §3.1): the document is the truth; the preview world is derived,
  runs no systems, and mirrors every write the same frame. `verify_world()` is the drift check.
- Every authoring write goes through an `EditorScene` command: inspector, gizmo, paste, asset drop
  and every MCP tool. Each command mutates the document, mirrors into the preview
  world, bumps the revision and marks the scene dirty.
- Undo (ADR-0030 §1–§3): the funnel records before and after snapshots of the touched entities;
  there are no per-command inverses. Edits with the same (entity, component) key coalesce until
  `seal_undo()`. History is bounded to 256 entries and 16 MiB and is cleared when a scene is opened,
  recovered or started. Each MCP `tools/call` is one edit group, so editor and MCP edits share one
  history.
- Prefab overrides (M8): `DocComponent::overrides` is a top-level field mask on instance components;
  only overridden fields are saved; `LocalTransform` is always the instance's own; overrides target
  the instance root only. Applying to the prefab writes the `.mcprefab` outside the undo history.
- Play-in-editor never writes the document or the preview world; Stop destroys the play world.
  Pause, step and time scale change the accumulator inflow, never `dt` (fixed 1/60 s, at most 5
  ticks per advance), so the same scene and tick count give the same `state_hash` as `mce_player`.
- Hot reload (ADR-0029 §2): polling, incremental re-cook, swap through
  `AssetSystem::commit_reload`; a failed re-cook keeps the previous output.
- Walls (`scripts/ci/check_walls.py`): ImGui only under `src/editor/layer/`; SDL3 headers only in
  the layer among editor code; the cook headers `mce/cook/` are allowed under `src/editor/`; the
  MCP socket wall forbids socket APIs under `src/editor/mcp/`; the MCP link wall allows
  `mce_mcp_core` only under `src/editor/mcp/` and `tests/mcp/`.
- MCP safety (ADR-0027): stdio only, stdout carries protocol messages only; paths are confined to
  `assets/`; `scene_save` refuses to overwrite a file changed on disk unless `force`;
  `console_exec` refuses `Cheat` and `RequiresRestart` convars unless `--allow-cheats`;
  `run_headless` plays the saved file and refuses unsaved changes.
- `run_headless` installs what `mce_player` installs, in the same order (input, physics2d, audio,
  particles, sequences, the game through `editor::play_game_install`, then `scene::install_streaming`
  with `async` off, `scene::load` of the scene, and one `Settle` pump): keep `headless_run.cpp` and
  `player.cpp` in step.

## How it is tested

ctest `test_editor` (headless, no GPU, links `mce_editor_layer` for its ImGui-free pieces;
`tests/editor/`): `test_scene_editing.cpp` ("SceneDocument: parse -> serialize -> parse is
byte-stable", "EditorScene: commands keep the preview world in sync with the document",
"inspector: reflection widgets round-trip edits through the funnel (zero per-type code)", "UC-C
rehearsal: new scene -> author -> save -> cook --pack -> the player runs it"), `test_undo.cpp`
("undo fuzz: N random commands, undo all -> byte-identical; redo all -> the edited scene"),
`test_prefab_overrides.cpp`, `test_prefab_preview.cpp`, `test_hierarchy.cpp`,
`test_play_session.cpp` ("play: enter builds the player's world from the document; same hash as
mce_player", "play: Play -> Stop -> Play gives the same first-tick state and the same run"),
`test_hot_reload.cpp`, `test_transform_gizmo.cpp`, `test_asset_browser.cpp`, `test_tilemap.cpp`,
`test_orbit_camera.cpp`, `test_perf_monitor.cpp`, `test_console_model.cpp`, `test_draw_clip.cpp`,
`test_stub_world.cpp`, `test_mod_project.cpp` ("mod project: the editor mounts the game's pack behind
the mod"). It depends on `mce_player` being built.

MCP (`tests/mcp/`): ctest `test_mcp` (JSON-RPC framing, tools through the funnel, undo/redo,
persistence rails, cheats refused, the game install and `inspect`, `cook_run` adopt, "FR-EDT-160 gate: MCP edit == SceneCommand edit (same .mcscene
bytes, same run_headless hash, same as mce_player)", and a fuzz case); `mcp_transcript` (drives the
real `mce_mcp` over pipes against `transcripts/demo_edit.golden.jsonl`); `mcp_symbol_wall` (no
socket symbols in `mce_mcp_core`, no ImGui symbols in `mce_mcp_core` or `mce_mcp`; not on Windows).

GPU (label `gpu`, `MCE_GPU_TESTS`): `editor_smoke` (default layout over the stub world),
`editor_scene_smoke` and `editor_play_smoke` (over the cooked `samples/scene_demo`), and
`template_probe_editor_gpu` from the project template.

## Design records

- [Editor and tools architecture](../architecture/editor-tools.md): §2.3 scene document, §2.4 undo,
  §2.6 inspector, §2.8 play session, §3.1 two-world model, §3.2 funnel invariants, §3.5
  editor/runtime separation.
- [Editor MCP architecture](../architecture/editor-mcp.md).
- ADRs: 0025 editor scene authoring (`docs/adr/0025-editor-scene-authoring.md`),
  0027 editor MCP server (`docs/adr/0027-editor-mcp-server.md`),
  0029 play-in-editor (`docs/adr/0029-play-in-editor.md`),
  0030 M6 editor authoring (`docs/adr/0030-m6-editor-authoring.md`).
- Milestones: editor catch-up (`docs/milestones/editor-catch-up.md`), M4.5 (`docs/milestones/M4.5.md`),
  M5b (`docs/milestones/M5b.md`), M6 editor (`docs/milestones/M6-editor.md`),
  M7 editor perf (`docs/milestones/M7-editor-perf.md`), M8 editor (`docs/milestones/M8-editor.md`),
  M8 project template (`docs/milestones/M8-project-template.md`).

## Known gaps

- Not built (M8 editor): the timeline / curve editor (FR-EDT-130 / FR-ENT-110), asset thumbnails,
  multi-scene tabs (one open document; a prefab opens in the same slot).
- Prefab overrides target the instance root only; per-child overrides need a stable child id in the
  prefab format (M8 editor, "Model limits").
- Tilemaps: no palette thumbnails, one layer per asset, no auto-tiling (ADR-0030 §8.5).
- MCP (M4.5): no cancellation of a running tool, no attach-to-running-editor and no `render_frame`.
  `run_headless` takes scripted input (`input`, an `InputScript`) but no recorded replay stream.
- Play-in-editor runs the game's `install()` but feeds no input: the module sees an `InputState`
  only if it installs one itself, and it stays idle, so a player character cannot be driven in the
  editor viewport; play the game from the host (`<game>`) instead. The play view looks through the
  scene's primary camera (View > "Game camera while playing") at the viewport panel's size, so HUD
  text pinned to the screen (Parallax 0) for the 1280×720 window can sit outside a smaller
  viewport (M9).
- The orbit view collides only with `Collider2D` boxes extruded along Z; 3D colliders join when the
  physics3d ECS bridge lands (comment in `orbit_camera.h`).
