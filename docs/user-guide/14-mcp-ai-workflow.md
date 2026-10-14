# 14. Working with an AI agent (MCP)

Every project has an MCP (Model Context Protocol) server (`<game>_mcp` from the checkout build, or
the stock `mce_mcp --module <lib<game>.so>`, `mce mcp` with the SDK) that gives an AI agent
the same scene-editing commands a person has in the editor, plus cook and headless verification.
Three games in the repository were authored entirely this way (Kindle's level, Prism Break and
Undertow), and a fourth (Lamplighter) by an agent that had never seen the engine source. This
chapter is the workflow that worked and the pitfalls it found.

## 14.1 Starting the server

```sh
build/<preset>/projects/MyGame/my_game_mcp --project projects/MyGame
build/<preset>/projects/MyGame/my_game_mcp --project projects/MyGame --scene scenes/main.mcscene
build/<preset>/projects/MyGame/my_game_mcp --project projects/MyGame --allow-cheats
build/<preset>/src/editor/mcp/mce_mcp --module build/<preset>/projects/MyGame/libmy_game.so --project projects/MyGame
mce mcp MyGame                                      # the SDK: the line above with its own paths
```

The server speaks JSON-RPC 2.0 over stdin and stdout, one message per line (the MCP stdio
transport); it opens no socket. Stdout carries nothing else: logs go to stderr, and so does the
command line `mce mcp` prints before it starts the server. Use the **project's** server or the stock one **with the module**:
without `--module`, `mce_mcp` knows only the engine's components (the visual-novel kit's included) and refuses scenes that use yours.
After a rebuild that adds components, restart the server.

With the SDK, `mce new` and `mce example` register the server for you: they write a `.mcp.json`
into the project naming the server `mce_<game>` (`mce mcp <project>` with absolute paths, so a
client can start it from anywhere). Claude Code, in a terminal or in Claude Desktop's Code tab,
offers it when you open the project folder; approve it once, and `/mcp` shows it connected. Build
the project first (`mce build`), since the server loads the game module.

```sh
mce mcp-config MyGame              # (re)write MyGame/.mcp.json, e.g. after moving the project or the SDK
mce mcp-config MyGame --desktop    # also add mce_my_game to Claude Desktop's claude_desktop_config.json
mce mcp-config MyGame --desktop --remove
```

Claude Desktop's chats (and chats in its Projects) do not read `.mcp.json`; their local servers
come from `claude_desktop_config.json` (`~/Library/Application Support/Claude/` on macOS), one entry
per game. `--desktop` merges the entry into it, keeps the other servers and a `.bak` of the old
file; quit and reopen Claude Desktop to load it. The `.mcp.json` holds absolute paths, so the
template's `.gitignore` leaves it out of the project's repository.

From a checkout (no SDK), write the `.mcp.json` yourself:

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
| Assets | `asset_schema` | `type?` | without `type`: the JSON source types below; with one (`ParticlePreset` or `.mcpart`, ...): its fields with type, default and help, and the template `asset_create` starts from |
| | `asset_create` | `path`, `content?` | a new `.mcpart`, `.mcseq`, `.mctilemap`, `.mcsheet`, `.mcanim`, `.mcmat`, `.mcinput`, `.mcstore`, `.mcservices`, `.mcui` or `.mcconfig` (the extension picks the type) with a fresh GUID: the template with `content`'s fields over it. Returns `guid:<uuid>` |
| | `asset_write` | `path` or `guid`, `content`, `mode?` | `merge` (default) replaces the named fields and keeps the rest, `replace` writes exactly `content`; the GUID stays |
| | `asset_read` | `path` or `guid` | the parsed document of a JSON source asset (scenes and prefabs too) |
| | `tilemap_paint` | `path` or `guid`, `clear?`, `cells?`, `rects?`, `fills?`, `preview?` | `[x, y, tile]` cells, `[x0, y0, x1, y1, tile]` rectangles and `[x, y, tile]` flood fills, applied in that order; (0, 0) is bottom-left, tile 0 erases, n draws tileset cell n-1. `preview` returns the map as text rows |
| Edit | `entity_create` | `name`, `components?` (object: type name → fields), `transform?` (bool, default true: add a `LocalTransform`), `at?` (int: hierarchy index, default the end) | returns the id; place the entity with `components.LocalTransform`, e.g. `{"name": "Crate", "components": {"LocalTransform": {"position": [0, 1, 0]}}}` |
| | `entity_create` with `prefab` | `name`, `prefab` (`"guid:<uuid>"` of a `.mcprefab`), `components?` | an instance; `components` override the prefab root's fields, `LocalTransform` places it |
| | `entity_delete`, `entity_duplicate` | `entity` | |
| | `scene_clear` | | every entity, one undo step; keeps GUID and path |
| | `component_add` | `entity`, `component`, `value?` | |
| | `component_remove` | `entity`, `component` | |
| | `component_set` | `entity`, `component`, `value`, `path?` | without `path`, `value` is an object of fields; with a property path (`position.x`, `shape.half`, `items[2]`) it is that property |
| Prefabs | `prefab_create_from` | `entity`, `path`, `replace?` | the entity and its `ChildOf` subtree as a new `.mcprefab`; by default the subtree becomes an instance of it (one undo step) |
| | `prefab_apply` | `entity`, `component?` | writes an instance's overrides into its prefab (every instance sees them) |
| | `prefab_revert` | `entity`, `component?` | drops an instance's overrides |
| Project | `project_get`, `project_set` | `values` (set) | the `.mceproj` members (title, start scene, games), checked before the file is written (chapter 2) |
| | `scene_settings_get`, `scene_settings_set` | `values` (set) | the open scene's `WorldSettings` (2D and 3D gravity); set creates the "World Settings" entity when the scene has none |
| Undo | `scene_undo`, `scene_redo` | | one `tools/call` is one step; shared with the editor's history |
| Persist | `scene_new` | `name`, `force?` | fresh GUID, unsaved |
| | `scene_open` | `path`, `force?` | path under `assets/`; a `.mcprefab` opens the prefab itself (entity 0 is its root) |
| | `scene_save` | `path?`, `name?`, `force?` | atomic; a `.mcprefab` path saves a prefab; `name` renames the scene; refuses to overwrite a file changed on disk, or another existing file, unless `force` |
| Build | `cook_run` | `pack?`, `adopt?` | incremental; `adopt` writes sidecars for new source files |
| | `cook_errors` | | last diagnostics with file, line, col |
| | `asset_delete` | `path` | deletes a source file under `assets/` and its `.mceasset` sidecar (not the open scene) |
| Verify | `run_headless` | `ticks` (default 300), `log_lines`, `scene?`, `input?`, `inspect?` | cooks, plays the **saved** scene (or another saved scene named by `scene`) with the engine systems and the game's `install()`, returns `state_hash`, metrics and the log tail. A pack with a `.mcstore` gets the fake store and ad network, one with a `.mcconfig` its cooked defaults, one with a `.mcservices` game services on this device; nothing is saved or sent |
| See | `render_frame` | `tick?` (default 1), `width?`, `height?` (default 960 × 540), `scene?`, `input?`, `camera?`, `safe_area?`, `software?`, `save?` | cooks, draws the **saved** scene with the real player at the first frame after sim tick N and returns the frame as an MCP **image**; `save` also writes the PNG under the project folder |
| | `render_sheet` | `ticks` (1-16), `cameras?` (1-4), `cell_width?`, `cell_height?`, and `render_frame`'s `scene`, `input`, `safe_area`, `software` | one image: ticks in columns, cameras in rows, each cell labelled |
| | `image_diff` | `golden`, `image` or `render`, `threshold?` (8), `max_score?` (0.01), `update?` | compares with a golden PNG in the project: `score` (mean channel difference, 0..1), `changed_fraction`, `pass`, and a heat map image; a missing golden is written |
| Console | `console_list` | `prefix?` | convars and functions |
| | `console_exec` | `line` | cheat and restart-only convars refused without `--allow-cheats` |

`asset_create`, `asset_write` and `tilemap_paint` cook the document in memory before they write it, with
the same checks `cook_run` makes (field values, unknown keys, references to missing or wrongly typed
assets): a refused call leaves the file as it was and the error names the field. The
`mce://assets/schema` resource holds every type's description at once.

Values use the `.mcscene` spelling (chapter 4): vectors as arrays, assets as `"guid:<uuid>"` (from
`asset_list`), enums by name, nested structs as objects, entity references as `"$<id>"`. Every `entity` argument
(and `inspect.entities`) takes an id or, when it is unique, the entity's name.

`input` scripts the actions the game reads from `InputState`, either as a string in the
`mce_player -input` form (section 11.5), `"10-70 Move=1,0 Jump\n100 Fire"`, or as an array of
segments, `[{"from": 10, "to": 70, "actions": {"Move": [1, 0], "Jump": true}}]`. The same scene,
ticks and input give the same hash. A scene with `SceneChunk`s streams during the run, with chunk
loads completed synchronously inside each pump so the hash stays a function of the ticks.

`render_frame`'s `camera` draws from another camera without changing the simulation:
`{"entity": "Boss Cam"}` (a camera entity of the open scene, by id or name), `{"index": n}` (an
entity's index in the file, for a scene that is not open), `{"position": [x, y, z], "target": [x,
y, z], "fov": 50}` (a free 3D camera) or `{"center": [x, y], "zoom": 0.5}` (a free 2D camera).
`safe_area` (`{"top": 48, "bottom": 34}`, pixels) makes the player lay its UI out as on a phone with
a notch and outlines the area on the returned image. The capture frame shows the held tick itself,
so the same arguments give the same pixels. Without a display (a cloud box, SSH) the player runs
on SDL's offscreen driver with Mesa's software rasterizer; `software: true` forces that path. The
server draws with `mce_player` beside it (with its `--module`), or with the project's host for a
`<game>_mcp` build; `--player <exe>` picks another.

`run_headless` with `inspect`:

```json
{ "ticks": 600,
  "inspect": { "entities": [3, 7], "component": "GameState", "show": ["GameState", "PlayerController"] } }
```

reports, after the last tick, the live values (`Transient` fields included) of the listed scene
entities and of every entity carrying `GameState`, spawned ones included, up to 64. This is how an
agent checks "did the player reach the flag" without a window.

## 14.3 A workflow that works

1. **Build first.** `cmake --build --preset <preset> --target my_game my_game_mcp` (`mce build
   MyGame` with the SDK); the server needs the module's components, compiled in or loaded.
2. **Discover.** `refl_list_components`, then `refl_describe_component` for the ones you will
   author (your own and `Sprite`, `RigidBody2D`, `Collider2D`, `TextLabel`, `Camera2DComponent`).
3. **Content.** Copy art and sounds under `assets/`, `cook_run {"adopt": true}` to give them
   sidecars, `asset_list` to get their GUIDs, `asset_set_import_settings` for normal maps and LUTs.
   Particle presets, sequences, tilemaps, materials, input maps and the store, services and config
   catalogs are made with `asset_create` (`asset_schema` shows their fields), never written by hand;
   lay a level out with `asset_create` on a `.mctilemap` and `tilemap_paint`.
4. **Author.** `scene_open`, then `entity_create` with a components object per entity;
   `entity_duplicate` plus `component_set LocalTransform position` to lay out repeated tiles;
   `component_set` to tune. Name entities.
5. **Save and check.** `scene_save`, `cook_run`, `run_headless` with `inspect`. Compare the state
   hash between runs to confirm a change changed something (or nothing).
6. **Iterate on code.** Edit the module, rebuild, restart the server (its component table is
   compiled in), repeat.
7. **Look.** `render_frame` after every visible change (`tick` and `input` reach a given moment;
   `camera` frames a spot the game camera does not show), `render_sheet` to see a run unfold, and
   `image_diff` with `update: true` once a screen looks right, so a later change that breaks it
   shows up as a failing score. Outside MCP the same frames come from `my_game -capture_ticks
   60,300 -capture shots/a_{tick}.ppm` (`-view.free`, `-view.entity`, `-safe_area` as above).

To verify gameplay headless, pass `input` to `run_headless`. An **autoplay** switch (a flag on a
component that drives the character from code), as the earlier MCP-authored games shipped, is
still handy for long runs but no longer required.

## 14.4 Pitfalls

| Pitfall | What happens | Do this |
|---|---|---|
| Entity ids are per scene | ids are saved in the file and survive `scene_open`, but a batch written against another scene's ids edits the wrong entities | re-list with `scene_list_entities` after opening a different scene; address entities by unique name; `scene_undo` repairs a bad batch |
| `run_headless` plays the saved file | unsaved edits are refused | `scene_save` first |
| The MCP cook writes `cooked/`, not the pack | the game executable still plays the old pack | `cook_run {"pack": true}`, or rebuild `my_game` (the build cooks the pack) |
| Stock server without the module | "unknown component type" for your components | run `<game>_mcp` or `mce_mcp --module <lib<game>.so>`, and restart it after adding components |
| `scene_new` makes a new GUID | a `.mceproj` that names the old scene points at nothing if you save over it | to re-author the start scene, `scene_open` it and `scene_clear` it |
| `scene_save` with `force` over another file | the file keeps its own GUID but its content is replaced by the open scene | save as a new path (a fresh GUID), or `scene_open` and `scene_clear` the existing scene |
| Adopted textures default to sRGB | normal maps and LUTs come out wrong | `asset_set_import_settings {"settings": {"srgb": false}}` |
| `-frames` is ignored headless | a headless run uses `-ticks` | `-headless 1 -ticks N` |
| One open scene per server | editing variants means save-as and reopen | keep variants as separate files; `run_headless {"scene": ...}` plays a saved one without opening it |

## 14.5 What an agent cannot do yet

Attach to a running editor, or cancel a long tool call. The friction logs in `docs/research/` list these with
their cost in practice.
