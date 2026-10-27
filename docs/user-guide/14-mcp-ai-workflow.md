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
After a rebuild that adds components, `module_build` maps them into a server started with `--module`; a server with the game compiled in (`<game>_mcp`) needs a restart. A scene that names a component or field the loaded module does not know fails `cook_run`, `run_headless`, `render_frame`, `ui_preview` and `timeline_preview` for every agent until then; the error says so.

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
| | `asset_set_import_settings` | `path` or `guid`, `settings` | rewrites a sidecar's settings (chapter 5.2 lists each importer's: a texture's `srgb` and `mips`, a video's `audio` and `normalize`, ...) |
| Assets | `asset_schema` | `type?` | without `type`: the JSON source types below; with one (`ParticlePreset` or `.mcpart`, ...): its fields with type, default and help, and the template `asset_create` starts from |
| | `asset_create` | `path`, `content?` | a new `.mcpart`, `.mcseq`, `.mctilemap`, `.mcsheet`, `.mcanim`, `.mcmat`, `.mcinput`, `.mcstore`, `.mcservices`, `.mcui`, `.mcconfig` or `.mcdefaults` (the extension picks the type) with a fresh GUID: the template with `content`'s fields over it. Returns `guid:<uuid>` |
| | `asset_write` | `path` or `guid`, `content`, `mode?` | `merge` (default) replaces the named fields and keeps the rest, `replace` writes exactly `content`; the GUID stays |
| | `asset_read` | `path` or `guid` | the parsed document of a JSON source asset (scenes and prefabs too) |
| | `asset_import` | `tool` (`aseprite`, `tiled`, `ldtk`, `gltf`, `fbx`), `file` (a path inside the project), `out_dir?` (under `assets/`), `name?`, `level?` (ldtk), `tile_size?`, `werror?` | converts an Aseprite, Tiled or LDtk export into `.mcsheet` / `.mcscene` + `.mctilemap` assets with the `mce import` converters (user guide 7.3), keeps GUIDs on a re-import, registers the result at once. Unsupported features are listed as warnings in `output`; `werror` makes them a failure that writes nothing. `gltf` / `fbx` take a model under `assets/` and write a `.mcprefab` beside it: a root `MeshRef` plus a child per light or camera at its world transform (8.4 has the unit formulas; a directional light is a warning naming `Environment3D.sun_direction`); FBX is read through `mce_cook`. Needs Python 3 |
| | `tilemap_paint` | `path` or `guid`, `clear?`, `cells?`, `rects?`, `fills?`, `preview?` | `[x, y, tile]` cells, `[x0, y0, x1, y1, tile]` rectangles and `[x, y, tile]` flood fills, applied in that order; (0, 0) is bottom-left, tile 0 erases, n draws tileset cell n-1. `preview` returns the map as text rows |
| Terrain | `terrain_sculpt` | `path` or `guid`, `op`, `points`, `radius?`, `strength?`, `hardness?`, `target?`, `seed?`, `stroke?`, `repeat?` | the Terrain panel's brushes on a `.mcterrain`: `raise`, `lower`, `smooth`, `flatten` (to `target` metres) or `noise`, a dab at each `[x, z]` point (metres from the terrain's centre) or along them with `stroke`. Returns the samples changed, the height range and the height under each point after the change (`strength` 0 only measures) |
| | `terrain_paint` | `path` or `guid`, `points`, `mode?` (`layer`, `hole`, `fill`), `layer?`, `radius?`, `strength?`, `hardness?`, `stroke?`, `repeat?` | paints a splat layer (its `TerrainFoliage3D` grows there), cuts holes or closes them. Returns each layer's share of the paint in percent and the hole samples |
| | `terrain_import` | `path` or `guid`, `file`, `range?`, `raw_bits?`, `raw_width?` | a heightmap under `assets/` (`.png`, `.r16`/`.raw`, `.r8`) into the heights, resampled to the resolution (chapter 5.7) |
| | `terrain_generate` | `path` or `guid`, `seed?`, `feature?`, `octaves?`, `lacunarity?`, `gain?`, `ridged?`, `low?`, `high?`, `island?`, `thermal?`, `talus_deg?`, `droplets?` | fBm hills, an island falloff and erosion-lite into the heights; deterministic in `seed` (chapter 5.7) |
| Roads | `roads_info` | `path` or `guid` | cooks a `.mcroads` and reports nodes, roads, lanes, connectors, conflicts, signals and lane kilometres, each junction's control and signal cycle, dead ends, and what traffic would trip on: `no_way_on` (one-way lanes that end) and `cut_off` (lanes not connected both ways to the rest); `ok` when both are empty (chapter 29) |
| | `roads_generate` | `path`, `columns?`, `rows?`, `block?`, `lanes?`, `speed?`, `sidewalk?`, `control?` (`signal`, `stop`, `priority`, `none`), `seconds?`, `drive?`, `name?`, `replace?` | writes a street grid to start a city from: junctions `n<i>_<j>`, roads `x<i>_<j>` and `z<i>_<j>`, signals with the x roads green first (chapter 29) |
| UI | `ui_list` | `document?` | without `document`: every `.mcui` with its widget count and the input actions it raises; with one (path or `guid:<uuid>`): its widgets as paths (`menu/play`) with kind, text, action, bind, convar |
| | `ui_create` | `path`, `modal?` (true), `font?`, `widgets?` | a new `.mcui` with a fresh GUID; `widgets` in the `.mcui` widget format (`asset_schema` type `UiDocument`) |
| | `ui_widget_add` | `document`, `widget`, `parent?` (root), `index?` (last) | adds a `.mcui` widget object (with its children) |
| | `ui_widget_set` | `document`, `widget` (path), `fields` | `.mcui` keys replace the widget's (`null` removes one); `style` changes the listed fields in every state, `styles` in one, `nav` the listed edges |
| | `ui_widget_remove` | `document`, `widget` | the widget and its children; refused while a nav edge names it |
| | `ui_theme_set` | `document`, `theme` (a `.mcuitheme` path or `guid:`; `null`: the default look) | sets the theme the document is drawn with (a `UiScreen`'s theme still wins in the player); `ui_list` with `document` names it |
| | `ui_preview` | `document`, `width?`, `height?` (960 × 540), `safe_area?`, `ui_scale?`, `save?`, `document_only?`, `software?` | the real player draws the document alone and returns the frame as an MCP **image**; `save` writes the PNG for `image_diff`. A project that does not cook (a broken scene) or has no saved scene is previewed from the document and what it references alone (bound fields show nothing), and the result says so in `document_only`; `document_only: true` does that on purpose, skipping the project cook |
| Timeline | `timeline_create` | `path`, `tracks` (one or more), `name?`, `duration?` | a new `.mctimeline` with a fresh GUID; `tracks` in the `.mctimeline` track format (`asset_schema` type `Timeline`, user guide 13.5). Play it with a `TimelinePlayer` (`component_add` with `{"timeline": "guid:<uuid>"}`), and add a `Cutscene` for the letterbox, hold and skip |
| | `timeline_add_track` | `timeline`, `track`, `index?` (last) | adds a `.mctimeline` track object (`{"type": "property", "target": 0, "path": "CameraRail3D.t", "keys": [...]}`) |
| | `timeline_add_key` | `timeline`, `track` (index), `key` or `keys` | adds keys (or ranges, cuts, lines, in the track's own form) merged in by time; returns the play length |
| AI | `bt_create` | `path`, `name?`, `blackboard?`, `root?` (a selector over a wait) | a new `.mcbt` with a fresh GUID; `blackboard` maps a key to its type or `{"type", "default"}`, `root` is a node in the `.mcbt` form (user guide 13.8). Returns the tree as numbered outline lines |
| | `bt_add_node` | `tree`, `node`, `parent?` (0, the root), `index?` (last) | adds a node (with its children) under a composite, or under a decorator with no child |
| | `bt_set` | `tree`, `node?` (outline number), `value?`, `fields?` (`null` removes one), `replace?`, `remove?`, `name?`, `blackboard?` (`null` removes a key) | edits the tree; each edit is cooked first, and one that does not cook fails with the cook's message and writes nothing |
| | `bt_trace` | `entity`, `ticks?` (300), `every?` (30) | plays the saved scene headless and reports the entity's brain: rows of tick, result, runs, running path and blackboard, and the tick each node `started` |
| | `perception_debug_lines` | `ticks?` (1) | plays the saved scene headless and reports every `Perceiver`: world `eye`, `forward`, `yaw_deg`, range, angle, `sees`, `target`, `last_seen`, and the debug `lines` (cone edges, range arc, a line to the target); the way to see where a guard looks |
| | `debug_lines` | `ticks?` (1), `categories?`, `limit?` (2000) | plays the saved scene headless and returns the gameplay debugger's world-space lines as data: box colliders, rigid bodies, triggers (green while occupied), joints, character capsules, vehicle wheels (red while skidding), waypoint chains and driver targets, with `counts` per category |
| | `debug_view` | `view?` (`top`/`front`/`side`), `ticks?`, `categories?`, `width?`, `height?` (512) | the same lines drawn by a CPU rasteriser and returned as a PNG, no GPU needed |
| | `asset_thumbnail` | `path` or `guid` (one asset) or `folder` (a contact sheet; `""` for all), `type?`, `size?` (16–512; 128 single, 64 sheet), `limit?` (64) | a CPU-drawn picture of the asset as PNG: a texture, a sound's waveform, a mesh (cook first), a material as a lit sphere, a particle preset as its emitter run for 1.5 s, else a plain tile by type; the JSON says per tile whether it is `real` |
| 2D skeleton | `skel2d_create` | `path`, `name?`, `root?` ("root") | a new `.mcskel2d` with a fresh GUID and one root bone (user guide 7.13). Every `skel2d_*` edit is cooked by the real importer before it is written, so an error names `file:line` and nothing changes |
| | `skel2d_add_bone` | `skeleton`, `name`, `parent?`, `x?`, `y?`, `rotation?` (degrees), `scale?` (number or `[sx, sy]`), `length?` | adds a bone, last; the parent must already exist |
| | `skel2d_add_slot` | `skeleton`, `name`, `bone`, `texture?` (`guid:`), `rect?`, `size?`, `pivot?`, `offset?`, `rotation?`, `tint?`, `attachments?`, `attachment?` | adds a slot (a textured quad on a bone, drawn after the earlier slots); `attachments` makes it a list of quads and meshes in the `.mcskel2d` form |
| | `skel2d_add_ik` | `skeleton`, `name`, `bones` (parent first), `target?`, `mix?`, `type?`, `bend?`, `iterations?`, `order?` | adds a two-bone or CCD IK constraint |
| | `skel2d_key` | `skeleton`, `animation`, `channel` (`rotate`, `translate`, `scale`, `color`, `attachment`, `ik_mix`), `target`, `t` and the channel's value, or `keys`; `curve?`, `duration?`, `loop?` | keys a channel at a time, creating the animation when missing; a key at the same time is replaced |
| | `skel2d_preview` | `skeleton`, `animation?`, `time?`, `ik_targets?`, `render?`, `width?`, `height?` | the bone world transforms of the rest pose or an animation at a time (the runtime's pose functions), and an image: a line drawing of the bones and slot quads |
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
| Project | `project_get`, `project_set` | `values` (set) | the `.mceproj` members (title, start scene, games, performance `budgets` per target), checked before the file is written, and each target's budget in force (chapter 2) |
| | `scene_settings_get`, `scene_settings_set` | `values` (set) | the open scene's `WorldSettings` (2D and 3D gravity); set creates the "World Settings" entity when the scene has none |
| Undo | `scene_undo`, `scene_redo` | | one `tools/call` is one step; shared with the editor's history |
| Persist | `scene_new` | `name`, `force?` | fresh GUID, unsaved |
| | `scene_open` | `path`, `force?` | path under `assets/`; a `.mcprefab` opens the prefab itself (entity 0 is its root) |
| | `scene_save` | `path?`, `name?`, `force?`, `resolve?` | atomic; a `.mcprefab` path saves a prefab; `name` renames the scene; a file another agent saved meanwhile is merged by entity (conflicts refuse unless `resolve` is `ours` or `theirs`); refuses to overwrite another existing file unless `force` |
| | `navmesh_bake` | none | bakes the scene's first `NavMeshSurface3D` into `assets/navmesh/<scene>/nav_<id>.mcnav` and points its `baked` at it (save the scene to keep that); returns the asset, its `content_hash` (the same scene bakes byte-identical), tiles, layers, triangles and bytes |
| | `hlod_bake` | `entity?`, `min_size?`, `impostor?`, `texels_per_metre?` | bakes an HLOD proxy for every `SceneChunk` (or only `entity`): the area scene's visible rigid meshes merged per material into `assets/hlod/<scene>/chunk_<id>.mchlod`, positions relative to the chunk, and points its `ChunkLod3D.proxy` at it (adds the component; save the scene to keep that); meshes under `min_size` metres (default 1) are left out; returns per chunk the asset, its `content_hash` (byte-identical re-bakes), merged/small/skipped counts, submeshes, triangles and LOD levels; a new sidecar asks for two simplified far levels (`lod_levels`); `impostor: true` also bakes the far stand-in (a box per root entity with CPU-rendered albedo pictures on its sides and top, cross cards for trees and poles) into `chunk_<id>_far.png` + `chunk_<id>_far.mchlod`, points `ChunkLod3D.impostor` at it and returns its groups, boxes, cards, quads, untextured count and atlas size; `texels_per_metre` (default 4) sets its sharpness |
| Build | `cook_run` | `pack?`, `adopt?`, `paths?`, `skip_errors?` | incremental; `adopt` writes sidecars for new source files (the error for one without says so); `paths` (files or folders under `assets/`) cooks only those and what they depend on, into `cooked/` only, never the pack; `skip_errors` also returns `failed_files`, every file that could not be cooked; the pack is written only by a run with no errors (section 5.4); the result also carries `sizes` and `meshes`, one entry per 3D model: `path`, `triangles`, `submeshes`, `materials`, `joints`, `clips`, `lod_levels`, `warnings` (read from the cooked file, so a cached model reports the same numbers; `warnings` counts this run's only; section 8.4) |
| | `cook_errors` | | last diagnostics with file, line, col |
| | `script_check` | `path` or `source`, `definitions?` | the cook's checks on one Luau script (syntax, hidden state, names with "did you mean", types against `mce.d.luau`) without cooking; returns every problem with line and col (user guide 27.5) |
| | `asset_delete` | `path` | deletes a source file under `assets/`, its `.mceasset` sidecar and its `CREDITS.md` row (a font's license text too, once no row names it; not the open scene) |
| Verify | `run_headless` | `ticks` (default 300), `log_lines`, `scene?`, `input?`, `inspect?`, `trace?`, `convars?` | cooks, plays the **saved** scene (or another saved scene named by `scene`) with the engine systems and the game's `install()`, returns `state_hash`, metrics and the log tail (and `script_errors` when a scene script failed). A pack with a `.mcstore` gets the fake store and ad network, one with a `.mcconfig` its cooked defaults, one with a `.mcservices` game services on this device; nothing is saved or sent |
| See | `render_frame` | `tick?` (default 1), `width?`, `height?` (default 960 × 540), `scene?`, `input?`, `camera?`, `view?`, `safe_area?`, `software?`, `convars?`, `save?` | cooks, draws the **saved** scene with the real player at the first frame after sim tick N and returns the frame as an MCP **image**; `save` also writes the PNG under the project folder. `convars` (`{"lc.quality": 0}`) sets console variables for that run only, so a setting can be previewed without changing the project; `run_headless` takes them too and puts the old values back after |
| | `timeline_preview` | `time`, and `render_frame`'s other arguments | the frame with every timeline in the scene held at `time` seconds: state tracks applied, cinematic cameras placed, a cutscene's letterbox and subtitle showing. Look at a cutscene at its beats without playing it through |
| | `render_sheet` | `ticks` (1-16), `cameras?` (1-4), `cell_width?`, `cell_height?`, and `render_frame`'s `scene`, `input`, `safe_area`, `software` | one image: ticks in columns, cameras in rows, each cell labelled |
| Views | `view_list` | none | the open scene's camera views (window or texture, `viewport`, `order`, split-screen cell, `PlayerCamera`), the `SplitScreen` layout and every `RenderTexture` with its cameras and the surfaces that show it, plus `problems`: a texture nothing draws into, a view into an entity that is no texture, a split camera nothing enables |
| | `view_add` | `name`, `camera?` (`2d`, `3d`), `width?`, `height?`, `update_hz?`, `position?`, `fit?` (2D), `look_at?` (3D), `show_on?` | a minimap, mirror, monitor or picture-in-picture in one call: the `RenderTexture` entity, the camera that fills it and, with `show_on` (an entity with a `Sprite` or `MeshRef`), the `ShowRenderTexture` that displays it |
| Players | `player_list` | none | the co-op setup: the `CoopGame` and, per player 1-4, their `PlayerSpawn`s, `PlayerCamera`s and the project's `.mcinput` maps with `"player": n`, plus `problems` (no `CoopGame`, a player without a spawn, camera or input map, a camera whose `split` does not match its player) |
| | `player_setup` | `players?` (1-4), `prefab?`, `layout?` (`auto`, `columns`, `rows`, `grid`), `mode?` (`2d`, `3d`), `spacing?` | a whole local co-op rig: a `CoopGame`, a `SplitScreen`, and per player a `PlayerSpawn` and a `PlayerCamera` with its split-screen `CameraView`. Add the input maps with `asset_create` (`.mcinput`, `"player"` key), then `player_list` |
| | `image_diff` | `golden`, `image` or `render`, `threshold?` (8), `max_score?` (0.01), `update?` | compares with a golden PNG in the project: `score` (mean channel difference, 0..1), `changed_fraction`, `pass`, and a heat map image; a missing golden is written |
| Measure | `perf_run` | `scene?`, `frames?` (300, 1 to 3600; under 30 the report notes noisy percentiles), `warmup?` (30), `target?` (desktop), `width?`, `height?`, `headless?`, `software?`, `input?`, `convars?`, `save?`, `top?` (8) | runs the real player for N frames at one sim tick per frame, so counters repeat run to run. Returns the verdict against the target's budget, with advice for each limit it is over; frame, sim, render and GPU ms (p50/p95/p99/max); draws, triangles, sprites and other counters; the slowest systems; the GPU stages; and spike frames with their cause. Keeps the `.mcperf` (`cooked/.perf/last.mcperf`, or `save`). With a `progressToken` it sends `notifications/progress` (launching, warmup N/M, capturing N/M, reading the capture) |
| | `perf_compare` | `before?`, `after?` (default: `perf_run`'s last two), `target?`, `top?` | two captures side by side: p50/p95 times, the worst frame's counters, the systems that moved most, `better` and `worse`, and the after capture's verdict |
| Code | `module_build` | `reload?` (true), `warnings?`, `target?` | rebuilds the game module with the project's CMake tree (the one the loaded module came from) and maps the new build into the server: new components and systems are live for the next call, no restart; compiler errors come back as `{severity, file, line, col, message}` |
| | `module_test` | `filter?` (default `^<game>_`) | `ctest` in the module's build tree: each test's result, the output's end when one failed |
| Playtest | `test_run` | `path?` (a `.mctest` file or folder, default `tests/`), `test?` (one inline) | plays each scenario headless with scripted input or a bot and evaluates its checks (section 14.3.1) |
| | `playtest` | `bot`, `scene?`, `ticks?` (1800), `until?`, `death?`, `assert?`, `trace?` | a built-in bot plays the scene; returns the **feel report**: time to finish, deaths, stuck spots, idle time, distance, cells visited |
| Know | `docs_search` | `query`, `group?` (guide, engine, architecture, samples, examples), `limit?` (8) | ranked sections of the shipped docs, samples and example games, each with its `mce://docs/...` URI, heading, line and snippet |
| | `engine_capabilities` | `area?` | each feature and how it is reached today (`data`, `asset`, `mcp`, `console`, `code`), every component, importable asset type and tool |
| Content | `gen_palette` | `seed?`, `count?` (6), `scheme?` (analogous, complementary, triad, pastel, earth, neon, mono), `save?` | hex colors dark to light and a swatch image |
| | `gen_sprite` | `seed?`, `kind?` (creature, character, ship, item, blob), `width?`, `height?` (16), `variants?`, `colors?` or `scheme?`, `save?` | mirrored, outlined, shaded pixel sprites in one row, their rects and an enlarged preview |
| | `gen_pixels` | `rows` (equal-length strings, one character per pixel), `palette` (character to `#rrggbb[aa]`; `.` and space are clear), `save?`, `overwrite?` | exactly the sprite you drew, as a PNG, with an enlarged preview: a moth, a lantern, two animation frames side by side |
| | `gen_mesh` | `shape?` (box, plane, sphere, cylinder, cone, disc, ramp, humanoid), `size?` ([x, y, z] m), `segments?`, `color?`, `roughness?`, `save?` (.glb) | a centred, Y-up mesh with its material inline, its vertex and triangle counts and a shaded preview, for blocking out a 3D level. `humanoid` is a skinned figure (size [shoulders, height, depth], default [0.5, 1.8, 0.28], facing −Z) with 13 bones and the clips `Idle` and `Walk`: an `Animator` plays them, `Ragdoll3D` makes it go limp, `AttachToBone` pins props to its bones |
| | `gen_tileset` | `seed?`, `style?` (grass, stone, brick, dirt, sand, snow, water, wood, metal), `tile?` (16), `colors?`, `save?` | eight tiles in one row (fill, fill_2, fill_3, top, top_left, top_right, platform, decoration) with their rects |
| | `gen_texture` | `seed?`, `kind?` (value, perlin, fbm, ridged, worley, sky), `width?`, `height?`, `scale?`, `octaves?`, `tileable?`, `colors?`, `clouds?`, `stars?`, `save?` | a tileable noise texture through gradient stops, or an equirect sky (width twice the height) |
| | `gen_sfx` | `seed?`, `preset?` (pickup, laser, explosion, powerup, hit, jump, blip, random), `params?`, `save?` (.wav) | an sfxr sound: the parameters used (change any and call again), length, peak and a waveform image |
| | `gen_music` | `seed?`, `bpm?` (120), `bars?` (8), `root?` (0 = C), `scale?` (major, minor, dorian, pentatonic, blues), `drums?`, `save?` (.wav) | a seamless loop of exactly `bars` bars (bass, chords, melody, drums), at most 60 s |
| | `library_search` | `query?`, `kind?` (model, sound, music, font, texture, sprite, recipe), `license?`, `limit?` (20) | entries of the bundled content library with id, license, author and tags |
| | `library_add` | `id`, `path?`, `overwrite?` | copies the entry into `assets/` (with the files it needs and a font's license text), or runs a recipe's generator (see "Content: generators and the library" below) |
| Ship | `project_check` | `checks?`, `ticks?` (300), `target?` (desktop), `budget_ms?` (the target's `sim_ms`), `max_findings?` (200) | findings with severity, file, line, message and a fix: cook errors, broken and unused references, missing credits, sRGB on data textures, each scene's ms per tick, images over the target's `texture_size` and scenes over its `sprite_textures`, 2D text size and contrast; `pass` when there is no error |
| | `ship_build` | `target?` (`desktop`, `web`), `build?` (true), `ticks?` (300), `version?`, `out?`, `steam_app_id?`, `steam_library?` | full build, a headless smoke run of the host, then `mce ship`: each stage's result and the zip's path (needs the SDK's server, `mce mcp`); with `steam_app_id` also the Steam depot and `steam_build_script` (15.7). `web`: the itch.io zip from `mce ship --target web` (a project under the engine checkout's `projects/`, chapter 15, section 15.6), `ticks` played in headless Chromium: `state_hash`, `matches_headless` (the same ticks headless in the browser) and `screenshot` of the last frame |
| Team | `project_watch` | `since?`, `limit?` (50), `others?` | the other agents' writes from `.mce/events.jsonl`: seq, time, agent, tool, path; `next` for the next call |
| | `asset_lock` / `asset_unlock` / `asset_locks` | `path`, `note?`, `ttl?` (1800 s) / `path`, `force?` / — | while you hold a lock, other agents' writes to that file are refused |
| | `task_list` / `task_add` / `task_claim` / `task_update` | `status?` / `title`, `notes?`, `depends?`, `files?` / `id?` / `id`, `status?`, `notes?` | the shared `tasks.mctasks`; a claim is never granted twice, and waits for its dependencies |
| | `scene_switch` / `scene_documents` | `path` / — | several scenes open at once, each keeping its unsaved edits |
| | `scene_diff` | `a`, `b?` (default: the open scene) | what changed between two scenes: entities added, removed or changed by id, renames, components and fields by name; `identical`, counts and a text `report` (the same as `mce scene diff`) |
| | `scene_lint` | `path?` (default: the open scene) | mistakes that load fine: unnamed or same-named siblings, zero scale, a transform that is not a number, a missing parent, a reference to an entity the scene lacks (`dangling_entity_ref`); `clean`, `findings` (entity, name, kind) and a text `report` (the same as `mce scene lint`) |
| | `scene_lint_fix` | `path` (a scene under assets/) | repairs what `scene_lint` finds and writes the file in the canonical text: empty names become `Entity <id>`, repeated sibling names get ` (2)`, ` (3)`, a missing parent or reference is dropped, a transform that is not a number returns to its default, a zero scale axis becomes 1. `fixed`, `written` (false: nothing to fix, the file is untouched) and the text `report` of what was found (the same as `mce_scene_lint --fix`). The open scene is not changed: `scene_open` reloads it |
| | `scene_lint_all` | none | `scene_lint` over every scene and prefab under assets/: `clean`, `scenes_checked`, `findings` (total), `unreadable` (files that do not parse) and `files`, one row per file with findings or unreadable (path, number of findings, text `report`). Read-only; `scene_lint_fix` repairs a file |
| | `scene_stats` | `path?` (default: the open scene) | what the scene holds: `entities`, `roots`, `max_depth`, `prefab_instances`, `min` / `max` of the local positions, `components` (type, entities) and a text `report` (the same as `mce scene stats`) |
| | `scene_find` | `path?` (default: the open scene), `name?`, `component?`, `prefab?` | the entities that match: `name` is a part of the entity name (ignoring case), `component` a component type it carries, `prefab` keeps only prefab instances; every condition given must hold. `count`, `hits` (entity, name, hierarchy path) and a text `report` (the same lines as `mce scene find`) |
| | `asset_used_by` | `path` (under assets/) or `guid` (`guid:<uuid>`) | the assets whose files name this one, so you see what a delete or rewrite would break: `count` and `used_by` (paths). The Assets panel's **Used by...**. Only asset files are read: a script that loads by path, the start scene and files outside `assets/` are not seen |
| | `report_friction` | `summary`, `area`, `severity?` (major), `tool?`, `expected?`, `actual?`, `workaround?`, `details?` | appends a note on what the engine made hard to the project's `friction.jsonl`; `mce friction` collects them |
| Console | `console_list` | `prefix?` | convars and functions |
| | `console_exec` | `line` | cheat and restart-only convars refused without `--allow-cheats` |


For a local-multiplayer game (chapter 11.3), name the second player's actions `P2/<Action>` in `input` (`P3/`, `P4/` likewise); `P1/<Action>` is the plain `<Action>`.

The `ui_*` tools edit a document through the cook's own reader and writer: each edit is read back
as a whole document (so a duplicate name or a nav path to a removed widget fails with the cook's
`file:line` message and nothing is written), then saved in the canonical `.mcui` form (every widget
explicit, a Tabs widget's bar and pages spelled out). Reading a saved document gives the same data,
so documents round-trip through the tools unchanged. A scene shows a document with a `UiScreen`
component (chapter 12.2).

`asset_create`, `asset_write` and `tilemap_paint` cook the document in memory before they write it, with
the same checks `cook_run` makes (field values, unknown keys, references to missing or wrongly typed
assets): a refused call leaves the file as it was and the error names the field. The
`mce://assets/schema` resource holds every type's description at once.

Values use the `.mcscene` spelling (chapter 4): vectors as arrays, assets as `"guid:<uuid>"` (from
`asset_list`), enums by name, nested structs as objects, entity references as `"$<id>"` (a value you write
may also give the entity's unique name). Every `entity` argument (and `inspect.entities`) takes an id or,
when it is unique, the entity's name.

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
entities (of the played scene: with `scene`, ids and names name that file's entities) and of every
entity carrying `GameState`, spawned ones included, up to 64. This is how an
agent checks "did the player reach the flag" without a window. Engine-written fields come back
too, so the 3D gameplay components read like a debugger: a `NavAgent3D`'s `state`, `target`,
`distance` and `next_corner`; a `Joint3D`'s `position` (hinge degrees, slider metres), `force`,
`torque` and `broken`; a `Ragdoll3D`'s `parts`, `root_position` and `settled`; a `Vehicle3D`'s
`speed`, `rpm`, `gear` and `wheels_on_ground`; a sensor's `hit`, `count` and `enters`.
`tests/mcp/test_mcp_gameplay3d.cpp` builds `samples/07_gameplay3d` this way, from an empty
project, and checks it with `inspect`. `tests/mcp/test_mcp_movement.cpp` does the same for `samples/08_movement`:
a lift (a `Joint3D` slider with a velocity motor), a course read back through `CharacterController3D.crouched`
and `CameraFollow3D.arm`, and a race of `VehicleDriver3D` karts read through `laps_done` and the `fired` count of a
`ShakeTrigger` with `"on": "Skid"`. A waypoint chain is easiest created last to first, so each `Waypoint3D.next`
names an entity that exists; close the loop with `component_set`.

`trace` samples fields while the run goes, as series: `{"trace": {"every": 10, "fields":
[{"entity": "Player", "field": "LocalTransform.position"}, {"entity": "Player", "field":
"Health.hp"}]}}` returns `trace.ticks` and one `values` array per field (at most 16 fields and
1000 rows; past that `trace.truncated` is true, so raise `every` for a long run). A field is `Component.path`, a property path (as `component_set` takes it) ending at a number, bool,
string, enum or vector (or one lane of it, `position.x`).

### Docs, samples and prompts

The server carries the engine's knowledge, read from the files the SDK installs (`share/doc/mce`,
`share/mce/samples`, `share/mce/examples`; in a build tree, the source tree; `--docs <dir>` picks
another folder):

| Resource | What |
|---|---|
| `mce://docs/index` | every doc, sample and example served, by group: start here |
| `mce://docs/guide/<file>` | this user guide (`mce://docs/guide/14-mcp-ai-workflow.md` is this chapter), recipes under `recipes/` |
| `mce://docs/engine/<file>`, `mce://docs/architecture/<file>` | the engine reference and architecture notes |
| `mce://docs/samples/<sample>/<path>` | the worked samples' code, scenes and data (a folder URI lists its files) |
| `mce://docs/examples/<Name>/<path>` | the example projects (Kindle, Undelivered, Room3D) |
| `mce://docs/components/<Type>` | one component's schema, as `refl_describe_component` returns it |

Three **prompts** (`prompts/list`, `prompts/get`) start whole jobs: `start_game` (argument `brief`:
the game from a one-paragraph brief, with no human in the loop), `add_level` (`name`, `focus?`) and
`polish_pass` (`focus?`).

### Content: generators and the library

Every `gen_*` tool returns a preview image (a sound's waveform) and is a pure function of its seed and
arguments, so an agent can try seeds until one looks right and then call again with `save`. `save` is a path
under `assets/` (`.png` or `.wav`); the tool writes the file, its `.mceasset` sidecar (pixel art: nearest
filtering, no mips; textures and skies: linear with mips) and a `CREDITS.md` row naming the generator and
seed, so the next `cook_run` takes it without `adopt`. Saving over an existing file needs `overwrite: true`
and keeps its GUID, so scenes that use it still find it.

The **content library** ships with the SDK (`share/mce/library/library.json`): Kenney's CC0 low-poly models
(city kit and platformer: buildings, roads, grass, trees, a character, coin, flag, cloud) and sound effects,
the OFL fonts Crimson Pro and Nothing You Could Do and the MIT ProggyClean, plus recipes (day, dusk and night
skies; rock, grass, sand, water and lava textures; tile sets; sprite sets; retro sounds; music loops) that run
a generator. `library_add` copies an entry into `assets/` (models into `models/` with their shared
`Textures/colormap.png`, fonts into `fonts/` with their license file), writes fresh sidecars and adds a
`CREDITS.md` row with the author, the source and the license, so `mce ship` carries the credits.

### Views and local players

A minimap, a mirror or a split screen is components (chapter 8.4 and 11.3); the view and player tools build and
check them in one call:

1. `player_setup {"players": 2, "prefab": "guid:..."}` adds the co-op rig; `asset_create` a `.mcinput` whose
   maps carry `"player": 1` and `"player": 2` (Move, Join, Leave; `samples/09_coop/assets/input/coop.mcinput`).
2. `view_add {"name": "Minimap", "fit": [26, 18], "show_on": "Monitor"}` makes a texture, its camera and the
   surface that shows it.
3. `view_list` and `player_list` name what is wrong before anything is drawn.
4. `render_frame` looks at it. `input` joins the players (`"5 P2/Join\n20-60 P2/Move=-1,0"`); `view: {"player": 2}`
   draws that player's camera alone and full size, `view: {"texture": "Minimap"}` the camera that fills a
   render texture. Without `view` the frame is the window as the players see it, split cells included.

### A look for each game

Every game made from the same template should not look the same. `mce new` projects carry three starter
themes in `assets/ui/themes/` (user guide 12, "Themes"), each with its own OFL fonts that cover Turkish and the
other Latin-1 and Latin Extended-A languages:

| Theme | Fonts (body, title) | For |
|---|---|---|
| `pixel.mcuitheme` | Pixelify Sans, Press Start 2P | retro and arcade games: dark panels, warm bevelled buttons, a hard text shadow |
| `clean.mcuitheme` | Atkinson Hyperlegible Next, Karla | puzzle, casual and tool-like games: slate panels, rounded blue buttons |
| `story.mcuitheme` | Crimson Pro, Crimson Pro Bold | novels and quiet games: paper panels with an ink rule, brown buttons, dark ink |

Pick the one that fits the brief and name it as each `.mcui` document's `"theme"` (`ui_theme_set`), or a
`UiScreen`'s `theme` for one scene; give titles `"font_role": "Title"`. When none fits, copy one to a new
`.mcuitheme` with a fresh `guid` and change its palette, fonts and skin (`<theme>_skin.png`, `gen_pixels` can
redraw it; `library_add` brings more fonts). Look at the
result with `ui_preview` or `render_frame`, and keep text readable: `project_check` warns about text under 16 px
or under 4.5:1 contrast. Delete the themes and fonts the game does not use before shipping (`project_check`
lists them as assets nothing references).

### Is it fast enough?

`perf_run` answers it for a target: `{"target": "mobile"}` judges the scene against the project's mobile budget
(chapter 2.2) and lists, for each limit it is over, what to try. Change one thing, run again, and
`perf_compare` with no arguments compares the last two runs. Each frame runs exactly one sim tick, so the
counters are the same on every machine and every run, and a change in draws or triangles is the scene's.
Times are the machine's own: on a box without a GPU, `gpu_ms` is the software rasterizer's, so read the
counters first. `headless: true` measures the simulation alone (no window), which is the quickest way to
find a slow system.

A person reads the same captures in the editor: the Profiler panel's **Capture** tab opens a `.mcperf`
(`cooked/.perf/last.mcperf` by default, or `mce_editor -editor.perf_capture <file>`). It shows the verdict
for the chosen target, every frame as a bar (spikes red, frames over the frame budget amber), the frame you
click taken apart into sim and render phases, GPU stages and counters, the spikes with their cause, and the
slowest systems. **Compare with** a second capture (`previous.mcperf` by default) adds the `perf_compare`
tables, with what got better in green and what got worse in red. Chapter 25 covers captures, budgets and
the counter gate in full.

### Before shipping

`project_check` is the review a person would do before a release, as data. Errors (an asset or scene that
does not cook, a GUID no asset has, a scene that does not run) make `pass` false; warnings (a media file
with no `CREDITS.md` row, a `CREDITS.md` row for a file the project no longer has, a normal map or mask
imported as sRGB, a scene over the per-tick budget, an image larger than the target's `texture_size` or a
scene whose sprites use more textures than its `sprite_textures`, text under 16 px on a 1280x720 screen or under 4.5:1
contrast against what is behind it: the backing `Sprite` panel under the label, else the camera's clear
colour) and infos (an asset nothing references; scenes and input maps are entry points) do not. Each finding names the file and, where it can, the call that fixes it. Timing
comes from the machine the server runs on, so treat the budget as a trend, not a gate. `ship_build` then
makes the zip a player gets; `mce ship` puts `CREDITS.md` in it.

### Several agents on one project

Several agents can build one game at once, each with its own server on the same project folder (`mce mcp
<project> --agent level-designer`; without `--agent` the name is the client's plus a short id). They meet in
the folder: `project_watch` reads the event feed every write appends to, `task_add` and `task_claim` share
the work through `tasks.mctasks`, and `asset_lock` keeps the others off a file while you rework it. No edit
is silently lost: when two agents save the same scene, the second save merges the first one's changes in,
entity by entity and component by component, gives a new entity whose id the other side also used a free
id, and refuses with the list of conflicts when both changed the same component, until `resolve` picks a
side. A prefab saved with `scene_save` merges the same way; a material or other file two agents both write is
last-write-wins, so lock it first.

#### Several agents on one scene

Each server holds its own copy of the scene, so another agent's save does not appear until you look. The
rules, in the order you will meet them:

- `scene_open` on the scene you already have open re-reads the file from disk, so it shows what the others
  saved. It refuses to drop your unsaved edits unless you pass `force: true`, which discards them and reloads.
  Opening a scene also replaces the copy `scene_switch` parked for it.
- `scene_save` merges a teammate's save in (their new entities stay). If it cannot (no common version, a
  file it cannot parse), it refuses and lists the entities on disk your copy lacks and the ones only yours
  has. Then either `scene_open {"path": ..., "force": true}` to reload, or `overwrite: true` to save your copy over
  theirs and lose their entities.
- `entity_delete` also deletes the entity's children, and every entity reference to them from entities that
  stay becomes null. The result lists each as `nulled_links` (the entity that holds it, the component and
  the field); set them again with `component_set`, or `scene_undo` the delete.

When the engine makes something hard (a tool that is missing, an error that does not say what to do, a doc
that was wrong), the agent calls `report_friction` before it works around it. The notes collect in the
project's `friction.jsonl`; `mce friction <project>...` turns the notes of one or many projects into a
Markdown backlog, blocking notes first and each area by how often it came up, for the engine's developers.

## 14.3 A workflow that works

1. **Build first.** `cmake --build --preset <preset> --target my_game my_game_mcp` (`mce build
   MyGame` with the SDK); the server needs the module's components, compiled in or loaded.
2. **Discover.** `engine_capabilities` for what is data and what needs code, `docs_search` for the
   closest sample, `refl_list_components`, then `refl_describe_component` for the ones you will
   author (your own and `Sprite`, `RigidBody2D`, `Collider2D`, `TextLabel`, `Camera2DComponent`).
3. **Content.** Copy art and sounds under `assets/`, `cook_run {"adopt": true}` to give them
   sidecars, `asset_list` to get their GUIDs, `asset_set_import_settings` for normal maps and LUTs.
   Particle presets, sequences, tilemaps, materials, input maps and the store, services and config
   catalogs are made with `asset_create` (`asset_schema` shows their fields), never written by hand;
   lay a level out with `asset_create` on a `.mctilemap` and `tilemap_paint`, or take it from Tiled or
   LDtk with `asset_import` (art from Aseprite the same way).
4. **Author.** `scene_open`, then `entity_create` with a components object per entity;
   `entity_duplicate` plus `component_set LocalTransform position` to lay out repeated tiles;
   `component_set` to tune. Name entities.
5. **Save and check.** `scene_save`, `cook_run`, `run_headless` with `inspect`. Compare the state
   hash between runs to confirm a change changed something (or nothing).
6. **Iterate on code.** Edit the module, then `module_build`: the server rebuilds it with the
   project's CMake tree and maps the new build in place, keeping the open scene (re-read with the
   new component layouts; the undo history restarts). Errors come back as file, line and column.
   `module_test` runs the project's ctest tests (the `<game>_headless_300` smoke test and your own).
   A server with the game compiled in (`<game>_mcp`) still needs a rebuild and a restart.
7. **Look.** `render_frame` after every visible change (`tick` and `input` reach a given moment;
   `camera` frames a spot the game camera does not show), `render_sheet` to see a run unfold, and
   `image_diff` with `update: true` once a screen looks right, so a later change that breaks it
   shows up as a failing score. Outside MCP the same frames come from `my_game -capture_ticks
   60,300 -capture shots/a_{tick}.ppm` (`-view.free`, `-view.entity`, `-safe_area` as above).

8. **Playtest.** `playtest` with a bot to see whether a level can be finished and how it feels;
   keep what must stay true as `.mctest` files under `tests/` and run them with `test_run` after
   every change.

No game needs its own **autoplay** flag any more: scripted `input` reaches a given moment, and the
bots play from the actor's position through the game's input actions.

### 14.3.1 Scenarios (.mctest) and bots

`mce test <project> [path]` plays the same scenarios from a terminal or CI with no agent (it starts the project's MCP server, calls
`test_run`, prints each scenario as pass or FAIL with the failed checks and any script errors, and exits 1 on a failure or when
there are none).

`mce inspect <project> --component Type` is the `run_headless` tool's `inspect` for a terminal: it plays the scene (`--ticks`, default 300)
and prints the live component values of every entity with that component and/or of `--entity` ids or names as JSON (Transient runtime fields
such as `ScriptVars.v` included; `--show A,B` picks the components). The state hash goes to stderr; it exits 3 when a script failed, as
`mce_player -headless` does. `--trace Hero:LocalTransform.position.x` (repeatable, at most 16; `--every N`, default 10) samples a field while the
run plays and prints one row per sampled tick (at most 1000), so you see how a value moved, not only where it ended; with only `--trace` no entity dump is printed.

A scenario is a JSON file under the project's `tests/` folder (`tests/reach_flag.mctest`):

```json
{
  "name": "reach the flag",
  "scene": "scenes/level1.mcscene",
  "ticks": 1800,
  "bot": { "kind": "walk_to", "actor": "Player", "target": "Flag", "plane": "x" },
  "until": { "alive": "Flag", "value": false },
  "death": { "entity": "Player", "field": "Health.hp", "<=": 0 },
  "assert": [
    { "feel": "finished_tick", ">": 0 },
    { "feel": "deaths", "<=": 1 },
    { "entity": "Player", "field": "LocalTransform.position.x", ">": 10, "tick": 600 },
    { "entity": "Score", "field": "ScoreBoard.coins", "value": 3 },
    { "event": "CoinCollected", ">=": 3 },
    { "count": "Enemy", "<=": 3 },
    { "script": "enemy.luau", "metric": "peak_steps", "<=": 400 },
    { "log_errors": 0 },
    { "log_contains": "level complete" }
  ],
  "trace": { "every": 30, "fields": [{ "entity": "Player", "field": "LocalTransform.position" }] }
}
```

| Key | Meaning |
|---|---|
| `scene` | a saved scene under `assets/` (default: the open scene, which must be saved) |
| `ticks` | the most the run may take (1-36000, default 1800) |
| `input` or `bot` | scripted input (as `run_headless` takes it) or a built-in bot, not both |
| `actor` | the entity the feel report follows (default: the bot's actor) |
| `until` | ends the run as soon as it holds (a field, `alive`, `event` or `scene` check) |
| `death` | counts a death each time it becomes true; deaths also count when the actor's entity goes away or it jumps more than `teleport` metres (default 3) in one tick (a respawn). Leaving the starting scene by a scene change is not a death: it ends the level (`finished`) |
| `assert` | the checks: at the end of the run, or after tick `tick` |

A check names one thing and compares it: `{"entity", "field"}` (a component field), `alive`
(an entity, default `true`), `event` (events of that type pushed so far, default `>= 1`; the C++
name `my_game::CoinCollected`, or `CoinCollected` when one event type has that name; an unknown
name fails the check with a note listing the game's events),
`scene` (the scene the run is in, by source path, after the game's `scene::request_change`),
`count` (how many live entities have that component, ones the run spawned included: `{"count": "Enemy", "<=": 3}`; it needs a comparison and a known component type),
`script` (a scene script's step profile, `mce profile`'s numbers: `{"script": "enemy.luau", "metric": "peak_steps", "<=": 400}`; the script by its path or the end of it, `metric` is
`peak_steps`, `avg_steps`, `total_steps` or `updates`; an unknown script name fails the check with the scripts that ran listed; steps are deterministic, so a budget regression fails a test),
`log_contains`, `log_errors` / `log_warnings` (the most allowed) or `feel` (`deaths`,
`idle_ticks`, `stuck_ticks`, `stuck_spots`, `distance`, `cells`, `finished_tick`). The comparison
is `"value": x` (equal), an operator key (`"<=": 3`, `">": 0`, `"!=": "Idle"`) or `"op"` with
`"value"`; numbers and vectors compare within `tolerance` (default 0.0001). Enums compare by name.
Entity checks name entities of the scene the test starts in, so after a scene change they read
nothing until the run is back in that scene; check the new scene with `scene`.

The bots drive the game's input actions (by default `Move` and `Jump`; `move` and `jump` rename
them) from the actor's position, so they need no game code:

| Bot | Plays | Arguments |
|---|---|---|
| `walk_to` | heads for `target` (a point `[x, y(, z)]` or an entity; done within `reach`, default 0.5 m, or when the target entity is gone), through the `through` waypoints first, in order (points or entities: around a pond, past each pickup; `{"at": ..., "wait": ticks}` stands still there first, on a pressure plate or a lift); when it stops getting closer it jumps (side view) or sidesteps (top-down); `air_jumps` (0-4) presses Jump again in the air for a double jump or a flap. The run goes on half a second after the arrival, so the game can react (a door, a pickup, a scene change) | `actor`, `target`, `through`, `plane`, `reach`, `air_jumps` |
| `explore` | wanders in seeded random directions, turning when blocked (and jumping, side view); coverage shows in `cells_visited` | `actor`, `plane`, `seed` |
| `masher` | holds a seeded random chord of `actions` (default Move, Jump, Fire, Confirm, Cancel), a new one every `period` ticks (8): crash and soft-lock hunting | `actions`, `period`, `seed`, `actor?` |

Every bot also takes three optional keys that make it act, not only move (a stealth test needs no
custom C++ system):

- `hold`: action names held down the whole run, e.g. `["Crouch"]` for a crouch walk.
- `hold_when`: `[{"action": "Crouch", "entity": "Sentry", "field": "Perceiver.sees", "value": 1}]` holds the
  action on the ticks the field check is true (the same `entity` / `field` / `value` or `{">": 5}` wording as a
  test's checks). Several entries are independent, so a bot can crouch while a sentry sees it.
- `stop_when`: one field check, `{"entity": ..., "field": ..., "value": ...}`. While it is true the bot stands
  still (no steering, no jump, a `wait` is paused), e.g. freeze while `Perceiver.sees` is 1.

A `walk_to` waypoint (in `through`, or the `target`) can also be `{"at": ..., "wait": ticks, "press": ["Interact"]}`:
the `press` actions go out for one tick when the bot arrives, once per waypoint.

`plane` is `xy` (top-down 2D or flying: `Move` is the direction), `x` (side view: `Move.x` and
`Jump`) or `xz` (3D on the ground: `Move` is `(x, -z)`, forward is -Z). Bots are deterministic:
the same scenario gives the same `state_hash`.

The **feel report** (`feel` in each result, when there is an actor): `finished` and
`time_to_finish_s` (a `walk_to` bot arrived, or `until` held), `deaths`, `idle_s` and
`idle_fraction` (the actor did not move), `stuck_s` and `stuck_spots` (asked to move but within
0.25 m of where it was a second earlier, with where and when), `distance_m`, `cells_visited` (1 m
cells), `alive_at_end` and the `bounds` it reached. A level that a `walk_to` bot cannot finish, or
that leaves it stuck at the same spot, is the first thing to look at with `render_frame`.

### When a cook fails

`cook_run` returns `ok: false` with diagnostics (file, line, column); `cook_errors` repeats them.
`run_headless`, `render_frame`, `ui_preview` and `timeline_preview` cook the whole project first, so
one bad file stops them all. Read the first error:

- *no .mceasset sidecar* (a new `.luau`, texture or model): `cook_run {"adopt": true}`.
- *unknown component type* or *unknown field*: the loaded module is older than your code. `module_build`
  (or `mce build`), and restart the server if it was not started with `--module`.
- A file you cannot fix yet: `cook_run {"paths": ["scenes/level_02.mcscene"]}` cooks only that and its
  dependencies, or `{"skip_errors": true}` cooks what it can and lists the failing files in
  `failed_files`. Neither writes a pack; fix the files, then `cook_run {"pack": true}`.

Section 5.4 has the full table.

## 14.4 Pitfalls

| Pitfall | What happens | Do this |
|---|---|---|
| Entity ids are per scene | ids are saved in the file and survive `scene_open`, but a batch written against another scene's ids edits the wrong entities | re-list with `scene_list_entities` after opening a different scene; address entities by unique name; `scene_undo` repairs a bad batch |
| `run_headless` plays the saved file | unsaved edits are refused | `scene_save` first |
| The MCP cook writes `cooked/`, not the pack | the game executable still plays the old pack (`mce play` and `mce headless` run `build/project/assets.mce`, which only `mce build` refreshes; they warn when it is older than `assets/`) | `cook_run {"pack": true}`, or `mce build` (the build cooks the pack) |
| Stock server without the module | "unknown component type" for your components | run `<game>_mcp` or `mce_mcp --module <lib<game>.so>`; `module_build` reloads new and changed components without a restart |
| `scene_new` makes a new GUID | a `.mceproj` that names the old scene points at nothing if you save over it | to re-author the start scene, `scene_open` it and `scene_clear` it |
| `scene_save` with `force` over another file | the file keeps its own GUID but its content is replaced by the open scene | save as a new path (a fresh GUID), or `scene_open` and `scene_clear` the existing scene |
| Adopted textures default to sRGB | normal maps and LUTs come out wrong | `asset_set_import_settings {"settings": {"srgb": false}}` |
| `-frames` is ignored headless | a headless run uses `-ticks` | `-headless 1 -ticks N` |
| `.mctest` entity by name after a prefab instance | refused: the cook expands a prefab into several entities, so later file positions shift | put prefab instances after the entities tests name, or give the scenario's checks entities that come first |
| One current scene per server | the edit tools work on the current scene only | `scene_switch` to the other one (its unsaved edits wait); `run_headless {"scene": ...}` plays a saved one without opening it |

## 14.5 What an agent cannot do yet

Attach to a running editor, or cancel a long tool call. The friction logs in `docs/research/` list these with
their cost in practice.
