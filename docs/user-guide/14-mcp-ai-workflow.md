# 14. Working with an AI agent (MCP)

Every project builds an MCP (Model Context Protocol) server, `<game>_mcp`, that gives an AI agent
the same scene-editing commands a person has in the editor, plus cook and headless verification.
Three games in the repository were authored entirely this way (Kindle's level, Prism Break and
Undertow), and a fourth (Lamplighter) by an agent that had never seen the engine source. This
chapter is the workflow that worked and the pitfalls it found.

## 14.1 Starting the server

```sh
build/<preset>/projects/MyGame/my_game_mcp --project projects/MyGame
build/<preset>/projects/MyGame/my_game_mcp --project projects/MyGame --scene scenes/main.mcscene
build/<preset>/projects/MyGame/my_game_mcp --project projects/MyGame --allow-cheats
```

The server speaks JSON-RPC 2.0 over stdin and stdout, one message per line (the MCP stdio
transport); it opens no socket. Use the **project's** server: the stock `mce_mcp` knows only the
engine's components and refuses scenes that use yours. After a rebuild that adds components,
restart the server.

Registering it with a client is the client's business. For Claude Code, a `.mcp.json` in the
project root:

```json
{ "mcpServers": { "my_game": {
    "command": "build/linux-gcc-debug/projects/MyGame/my_game_mcp",
    "args": ["--project", "projects/MyGame"] } } }
```

A script can drive it directly: `projects/Kindle/tools/author_level_mcp.py` is a complete
client (initialize, `notifications/initialized`, then `tools/call` with `{"name", "arguments"}`;
each result's `content[0].text` is the tool's JSON).

## 14.2 The tools

| Group | Tool | Arguments | Notes |
|---|---|---|---|
| Discover | `refl_list_components` | | every component the server knows |
| | `refl_describe_component` | `name` | fields with kind, ranges, enum values, flags, help |
| Read | `scene_info` | | name, GUID, path, dirty flag, entity count |
| | `scene_list_entities` | `component?` | id, name, component types, in file order |
| | `scene_get_entity` | `entity` | the entity in `.mcscene` form; defaults omitted |
| | `asset_list` | `type?` | `guid:<uuid>`, path, type |
| | `asset_set_import_settings` | `path` or `guid`, `settings` | rewrites a sidecar's settings |
| Edit | `entity_create` | `name`, `components?`, `at?`, `transform?` | returns the id |
| | `entity_delete`, `entity_duplicate` | `entity` | |
| | `scene_clear` | | every entity, one undo step; keeps GUID and path |
| | `component_add` | `entity`, `component`, `value?` | |
| | `component_remove` | `entity`, `component` | |
| | `component_set` | `entity`, `component`, `value`, `path?` | without `path`, `value` is an object of fields; with a property path (`position.x`, `shape.half`, `items[2]`) it is that property |
| Undo | `scene_undo`, `scene_redo` | | one `tools/call` is one step; shared with the editor's history |
| Persist | `scene_new` | `name`, `force?` | fresh GUID, unsaved |
| | `scene_open` | `path`, `force?` | path under `assets/` |
| | `scene_save` | `path?`, `force?` | atomic; refuses to overwrite a file changed on disk unless `force` |
| Build | `cook_run` | `pack?`, `adopt?` | incremental; `adopt` writes sidecars for new source files |
| | `cook_errors` | | last diagnostics with file, line, col |
| Verify | `run_headless` | `ticks` (default 300), `log_lines`, `inspect?` | cooks, plays the **saved** scene with the engine systems and the game's `install()`, returns `state_hash`, metrics and the log tail |
| Console | `console_list` | `prefix?` | convars and functions |
| | `console_exec` | `line` | cheat and restart-only convars refused without `--allow-cheats` |

Values use the `.mcscene` spelling (chapter 4): vectors as arrays, assets as `"guid:<uuid>"` (from
`asset_list`), enums by name, nested structs as objects, entity references as `"$<id>"`.

`run_headless` with `inspect`:

```json
{ "ticks": 600,
  "inspect": { "entities": [3, 7], "component": "GameState", "show": ["GameState", "PlayerController"] } }
```

reports, after the last tick, the live values (`Transient` fields included) of the listed scene
entities and of every entity carrying `GameState`, spawned ones included, up to 64. This is how an
agent checks "did the player reach the flag" without a window.

## 14.3 A workflow that works

1. **Build first.** `cmake --build --preset <preset> --target my_game my_game_mcp`; the server
   needs the module's components compiled in.
2. **Discover.** `refl_list_components`, then `refl_describe_component` for the ones you will
   author (your own and `Sprite`, `RigidBody2D`, `Collider2D`, `TextLabel`, `Camera2DComponent`).
3. **Content.** Copy art and sounds under `assets/`, `cook_run {"adopt": true}` to give them
   sidecars, `asset_list` to get their GUIDs, `asset_set_import_settings` for normal maps and LUTs.
4. **Author.** `scene_open`, then `entity_create` with a components object per entity;
   `entity_duplicate` plus `component_set LocalTransform position` to lay out repeated tiles;
   `component_set` to tune. Name entities.
5. **Save and check.** `scene_save`, `cook_run`, `run_headless` with `inspect`. Compare the state
   hash between runs to confirm a change changed something (or nothing).
6. **Iterate on code.** Edit the module, rebuild, restart the server (its component table is
   compiled in), repeat.
7. **Look.** Build and run `my_game -frames 120 -capture shot.ppm` (an absolute path) to grab a
   frame, or open the project editor; MCP has no `render_frame` yet.

Headless runs feed no input, so a game needs an **autoplay** switch to be verifiable headless: a
flag on a component (or a second scene with a scripted pilot entity) that drives the character from
code. Every MCP-authored game so far shipped one.

## 14.4 Pitfalls

| Pitfall | What happens | Do this |
|---|---|---|
| Entity ids are per session | ids restart from 1 after `scene_open`; a batch written against stale ids edits the wrong entities | re-list with `scene_list_entities` after every `scene_open`; prefer names in your own notes; `scene_undo` repairs a bad batch |
| `run_headless` plays the saved file | unsaved edits are refused | `scene_save` first |
| The MCP cook writes `cooked/`, not the pack | the game executable still plays the old pack | `cook_run {"pack": true}`, or rebuild `my_game` (the build cooks the pack) |
| Stock server vs project server | "unknown component type" for your components | run `<game>_mcp`, and restart it after adding components |
| `scene_new` makes a new GUID | a `.mceproj` that names the old scene points at nothing if you save over it | to re-author the start scene, `scene_open` it and `scene_clear` it |
| `scene_save` with `force` over another file | that file gets the open scene's GUID | save as a new path, or clear and re-author the existing scene |
| Adopted textures default to sRGB | normal maps and LUTs come out wrong | `asset_set_import_settings {"settings": {"srgb": false}}` |
| `-frames` is ignored headless | a headless run uses `-ticks` | `-headless 1 -ticks N` |
| `-capture` with a relative path fails | "cannot write" | pass an absolute path |
| No tilemap tool | the floor becomes one stretched PNG | one `Sprite` per tile via `entity_duplicate`, or write a `.mctilemap` (section 5.7) |
| One scene per server | trying variants means save-as and reopen | keep variants as separate files and open the one to test |

## 14.5 What an agent cannot do yet

Paint tilemaps, render a frame, feed recorded input to a headless run, delete assets, attach to a
running editor, or cancel a long tool call. The friction logs in `docs/research/` list these with
their cost in practice.
