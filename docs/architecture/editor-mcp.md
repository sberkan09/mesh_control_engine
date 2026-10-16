# Architecture: Editor MCP server (`mce_mcp`) — design document

**Project:** Mesh Control Engine (MCE) · **Subsystem:** `editor-tools` (tooling extension) · **Date:** 2026-10-08
**Status:** Implemented in M4.5 (`src/editor/mcp`, ADR-0027; record: `docs/milestones/M4.5.md`). **Build:** ROADMAP M4.5. **Requirement:** FR-EDT-160 (Should).
**Inputs:** `docs/REQUIREMENTS.md` (FR-EDT, FR-RUN-040, FR-AST-010, §5 N7/N8/N10), `docs/architecture/editor-tools.md`
(§2.1 process model, §2.3 scene document, §3.1–§3.3 mutation funnel), `docs/architecture/core.md` (convars/confuncs),
Model Context Protocol specification (JSON-RPC 2.0 over stdio).

---

## 1. Why, and what this is not

MCE already has the three things an AI collaborator needs, built for other reasons:

- **Reflection** (`mce_refl`, `MCE_COMPONENT`) describes every component and field in machine-readable form. The
  inspector reads it with zero per-type code; a tool server can read it the same way.
- **Headless, deterministic runs** (`World::state_hash`, the determinism ladder, the 30-second headless platformer
  run). An agent can make a change, run the game without a window and get a hash, metrics and a log back — it can
  check its own work instead of asking a human to look.
- **The console / convar + confunc registry** is already a flat, named command surface.

`mce_mcp` exposes those through the Model Context Protocol so an MCP client (Claude Code, Claude Desktop, any other
MCP host) can read a scene, edit it through the same mutation funnel as the editor UI, cook, run headless, and read
the results.

**Not in scope (deferred, see ROADMAP M10+):** AI inside the shipped game (LLM-driven NPCs, dialogue, runtime
generation). That brings latency, per-call cost and non-determinism into the sim, conflicts with FR-RUN-040, and
needs a network stack the runtime does not have (N7). It is a separate, optional module for later — if ever.

## 2. Process model

```
MCP host (Claude Code, …)
    │  stdin/stdout, JSON-RPC 2.0, newline-delimited (MCP stdio transport)
    ▼
mce_mcp(.exe)  ──►  mce_mcp_core (protocol + tool table, headless, ImGui-free)
                       │
                       ├─► mce_editor_core  (SceneDocument + EditorScene funnel, EditorSession/CookService,
                       │                     ConsoleModel, GUID registry scan)
                       └─► mce_engine       (refl registry, ecs::World, game-module loader, headless host)
```

- **A separate executable, not a mode of `mce_editor`.** It links `mce_editor_core` (headless by construction,
  editor-tools §2.1 / ARCHITECTURE F7) and never ImGui, so it builds and runs in `local_ci.sh` without a display.
- **stdio only.** No socket, no listener, no port: the MCP host spawns `mce_mcp` as its child and talks over pipes.
  This keeps N7 (no networking) intact; a CI grep wall forbids socket APIs in `mce_mcp*`.
- **One project per process**, chosen on the command line: `mce_mcp --project <dir> [--module <game module>] [--scene <file>]`. Every path a
  tool accepts is resolved inside that root; anything escaping it is an error.
- **Attaching to a running GUI editor** (same tool table, served by the editor process over a local pipe so the
  human sees edits live) is a stretch goal; it reuses `mce_mcp_core` unchanged and is cut first.

## 3. The JSON boundary

FR-AST-010 says the *shipped runtime* never parses JSON. MCP is JSON-RPC, so:

- The JSON codec lives in `mce_mcp_core` only. `mce_engine`, `game.exe` and every runtime target stay JSON-free; the
  existing symbol scan gains `mce_mcp_core` as a forbidden link of runtime targets.
- **Default: a small hand-written codec** (reader + writer, `Result<T,Error>`, no exceptions, depth/size limits),
  matching the engine's hand-written-core rule and fuzzed like the `.mce` container. Fallback if it grows past
  ~600 lines: an ADR'd FetchContent dependency (yyjson) under ADR-0017's rules. ADR-0027 records the pick at
  milestone start (0024–0026 went to the editor catch-up and M6). **Picked: the cook's existing hand-written codec (`mce/cook/json.h`) plus a small writer; no new codec, no dependency.**

## 4. Tool surface (MVP)

Names follow `area_verb`. Every write goes through the `EditorScene` command funnel (`create_entity`,
`set_component`, `paste_entities`, ... — ADR-0025) — the same funnel as the inspector and gizmos (editor-tools §3.2) — so mirroring, dirty tracking and undo (FR-EDT-040, ADR-0030 §1–§3) apply to
agent edits exactly as to human ones. Nothing touches disk unless a tool says so.

| Group | Tools | Notes |
|---|---|---|
| Discover | `refl_list_components`, `refl_describe_component` | Field names, types, ranges, enum values, read-only flags — straight from `mce_refl`. |
| Read | `scene_info`, `scene_list_entities`, `scene_get_entity`, `asset_list` | Entities by persistent `EntityId` (saved in the file as each entity's `"id"`, so the same id names the same entity after `scene_open`) or by name where it is unique; components as reflected field maps; assets from the GUID registry (type filter). `asset_set_import_settings` rewrites a sidecar's importer settings; `asset_delete` removes a source file with its sidecar (the two writes outside the scene). |
| Assets | `asset_schema`, `asset_create`, `asset_write`, `asset_read`, `tilemap_paint` | The JSON source assets (`.mcpart`, `.mcseq`, `.mctilemap`, `.mcsheet`, `.mcanim`, `.mcmat`, `.mcinput`, `.mcstore`, `.mcservices`, `.mcui`, `.mcconfig`) by path or GUID: a described template per type, a fresh inline GUID on create, merge or replace on write, cells/rects/flood fills on a tilemap. Every write is cooked in memory first (`cook::cook_document`) and refused whole when the cook would refuse it (M-DATA-REACH 1). |
| Terrain | `terrain_sculpt`, `terrain_paint`, `terrain_import`, `terrain_generate` | A `.mcterrain` by path or GUID (M-TERRAIN 6): the editor's brushes (`editor::TerrainDocument`, the Terrain panel's code) dabbed at points or along strokes, a PNG or raw heightmap resampled into the heights, and an fBm generator with an island falloff and erosion-lite. Each reads the source, cooks it in memory for the data the brushes edit, and writes it back through the asset tools' cook-checked write. |
| UI | `ui_list`, `ui_create`, `ui_widget_add`, `ui_widget_set`, `ui_widget_remove`, `ui_theme_set`, `ui_preview` | `.mcui` documents by widget path (M-UI-DATA 5, `tools_ui.cpp`; `ui_theme_set` sets the document's `"theme"`): each edit is made on the canonical form (`cook::write_ui_document`), read back whole by `cook::read_ui_document` (refused with the cook's message, nothing written) and saved canonically, so a document round-trips unchanged. `ui_preview` runs the player with `-ui_preview <guid>` (the document alone, no world) through `render_frame`'s capture path. |
| Timeline | `timeline_create`, `timeline_add_track`, `timeline_add_key`, `timeline_preview` | `.mctimeline` documents (M-CINEMA 6, `tools_timeline.cpp`): edited as JSON, read back whole by `cook::read_timeline_document` (refused with the cook's message, nothing written) and saved canonically by `cook::write_timeline_document`. `timeline_preview` runs `render_frame`'s capture with the player's `timeline.scrub` convar: every timeline is scrubbed to the time and held at speed 0. |
| Edit | `entity_create`, `entity_delete`, `entity_duplicate`, `scene_clear`, `component_add`, `component_remove`, `component_set` | `component_set` takes a refl property path + value; type errors come back as tool errors with the expected type. |
| Undo | `scene_undo`, `scene_redo` | The editor's undo history (FR-EDT-040, ADR-0030 §1–§3): each editing `tools/call` is one step, named after the tool. |
| Persist | `scene_open`, `scene_save` | `.mcscene` text format; save uses the same temp-write-then-rename as autosave. Save As gives a new file a fresh GUID, keeps the GUID of a file overwritten with `force`, and `name` renames the scene. |
| Build | `cook_run`, `cook_errors` | Through `CookService`; errors carry file + line (FR-EDT-070 data). `adopt` writes sidecars for new source files (`mce_cook --adopt`). The result names the cooked tree and, with `pack`, the `assets.mce` it wrote: a project host plays its own build-tree pack unless started with `-pack` pointing at one of these. |
| Verify | `run_headless` | Cooks, then plays the saved scene (or another saved scene of the project, `scene`) headless with what `mce_player` installs plus the game's `install()` (`--module`, or a project's `<game>_mcp`, registers it), runs N fixed ticks, returns `state_hash`, `World::metrics`, last log lines and, with `inspect`, the final component values (Transient fields included) of chosen entities. `input` scripts the actions the game reads from `InputState` per tick (`mce/input/script.h`: a text script `"10-70 Move=1,0 Jump"` or segments `{from, to, actions}`), so a keyboard game can be played to its win and lose states without a window. |
| See | `render_frame` | Renders one frame of the scene offscreen (llvmpipe in CI) and returns a PNG as MCP image content. Stretch; cut second. |
| Console | `console_list`, `console_exec` | Convars/confuncs via `ConsoleModel`; `Cheat`/`RequiresRestart` vars refused unless `--allow-cheats`. |

**Resources:** `mce://scene/current` (the document as `.mcscene` text), `mce://log` (console ring tail),
`mce://refl/schema` (all components). **Prompts:** none at MVP.

## 5. Determinism and safety

- Agent edits are ordinary `SceneCommand`s, so the drift-killer test (editor-tools §3.1) covers them for free.
- `run_headless` is the existing determinism path, not a new one: the same scene + ticks + input give the same hash
  as `mce_player`/the devhost. The MCP layer never feeds wall-clock time or randomness into the sim.
- Long operations (cook, headless run) report MCP progress notifications and honour cancellation.
- No tool deletes files outside the scene document; `scene_save` refuses to overwrite a file changed on disk since
  it was opened (mtime + hash check) unless `force` is passed.

## 6. Tests and gates

1. **Codec:** unit tests + a fuzz target (malformed, deep, huge input never crashes; ASan clean).
2. **Protocol conformance:** a scripted transcript test (`tests/mcp/`) drives `mce_mcp` over pipes:
   `initialize` → `tools/list` → calls → `shutdown`; golden responses checked in.
3. **Round-trip equivalence (the gate):** the same edit done via MCP tools and via `SceneCommand`s in a C++ test
   saves byte-identical `.mcscene` files and produces the same `run_headless` hash.
4. **Walls:** no ImGui and no socket symbols in `mce_mcp*`; no JSON codec in any runtime target.
5. All of the above run in `scripts/ci/local_ci.sh` (new stage or folded into the preset ctest runs).

## 7. Risks

| Risk | Mitigation |
|---|---|
| Scope creep into an "AI feature" project | Tool table above is the whole MVP; anything else is a slip note. Milestone carries no Must FRs and may slip whole past M5b. |
| Depends on editor catch-up landing (SceneDocument, CookService wiring, headless player) | Hard prerequisite; M4.5 does not start until those are merged. |
| MCP spec churn | Target one pinned protocol version; the transport is tiny and isolated in `mce_mcp_core`. |
| Agent produces scenes a human cannot follow | Same funnel, same text format, same diffable `.mcscene`; attach-to-editor stretch makes edits visible live. |
