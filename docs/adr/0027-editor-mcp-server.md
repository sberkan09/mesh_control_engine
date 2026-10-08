# ADR-0027: Editor MCP server (JSON codec, transport, tool semantics)

**Status:** Accepted (2026-10-08). **Milestone:** M4.5. **Requirement:** FR-EDT-160. **Design:** `docs/architecture/editor-mcp.md`.

**Context.** M4.5 adds `mce_mcp`, a Model Context Protocol server that lets an MCP client (Claude Code, Claude
Desktop, any MCP host) read and edit scenes, cook, and run them headless. The design left four things to decide at
milestone start: which JSON codec the server uses (the design's default was "a small hand-written codec", with yyjson
as the fallback past ~600 lines), how the stdio transport keeps the protocol stream clean, how tool values are
spelled, and how `run_headless` reaches "the existing determinism path".

**Decisions.**

1. **No new JSON codec: the server reuses the cook's.** `mce/cook/json.h` is already a hand-written, exception-free
   reader/writer with a 128-level depth limit and line/column positions, and it already lives only on the editor and
   tool side (the cook wall in `check_walls.py`). `mce_mcp_core` adds a ~150-line streaming `JsonWriter` on top of
   it for structured output. No dependency is added and yyjson stays unneeded. The runtime stays JSON-free
   (FR-AST-010): a new link wall forbids `mce_asset_cook` (which holds the codec) and `mce_mcp_core` outside the
   editor, tools and tests.
2. **Transport: stdio only, newline-delimited JSON-RPC 2.0, protocol version `2025-06-18`** (also accepts
   `2025-03-26` and `2024-11-05` when a client asks for them). No socket, listener or port. At startup `mce_mcp`
   moves the real stdout to a private descriptor and points fd 1 at stderr, so the engine log (spdlog's stdout
   sink) and any stray print go to stderr and stdout carries protocol messages only. Batches are refused (removed
   from MCP in 2025-06-18). A tool call runs to completion before the next line is read, so
   `notifications/cancelled` is accepted and ignored. Long calls (`cook_run`, `run_headless`) send
   `notifications/progress` when the client passes a progress token.
3. **Values use the `.mcscene` spelling, with entity references as `"$<EntityId>"`.** The scene reader and writer
   (`mce/cook/scene_text.h`) do the type checking, so a value the tools accept is exactly a value the cook accepts.
   The one difference from the file format: a reference names the document's persistent entity id (what every tool
   returns), not the file index. `component_set` takes either an object of fields (a patch over the current value) or
   a refl property path plus that property's value; the path case runs the same reader over a one-field wrapper type
   at the property's offset. Values are applied to a copy and committed through `EditorScene::set_component` (the
   inspector's funnel). ReadOnly and Transient fields are refused, and a failed call changes nothing.
4. **`run_headless` is the player's headless path, in process.** It cooks (incrementally), mounts a fresh
   `AssetSystem` on `<project>/cooked`, installs physics2d and the audio bridge on a Silent device, `scene::load`s
   the saved scene and ticks it through `app::run`'s headless stepping clock, exactly like `mce_player -headless 1`.
   Running it in process gives back `World::metrics` and the log tail; a test pins its hash to `mce_player`'s. It
   plays the saved file, so a scene with unsaved changes is refused (call `scene_save` first).
5. **Safety rails.** Every path is relative to `assets/` (forward slashes, no `.`/`..` segments, `.mcscene`).
   `scene_save` refuses to overwrite a file whose bytes changed since it was opened or last saved (an xxh64 of the
   content), or another existing file on Save As, unless `force` is set. `scene_open` and `scene_new` refuse to drop
   unsaved changes unless forced. `console_exec` refuses to set `Cheat` or `RequiresRestart` convars unless the
   server runs with `--allow-cheats`.
6. **Tracy is the one socket user in the linked executable.** Every development binary carries the Tracy client
   (`MCE_ENABLE_TRACY`, on-demand), which has its own listener. The symbol wall therefore checks `mce_mcp_core` (the
   server's code) for socket symbols, and the source wall covers `main.cpp`. A strict no-network build of `mce_mcp`
   is a `-DMCE_ENABLE_TRACY=OFF` configure. The executable is still checked for ImGui symbols.

**Consequences.** The whole server is about 1,800 lines with no new dependency. Agents edit through the same funnel
and file format as humans, so the round-trip gate (`tests/mcp`) and the editor's drift checks cover them. A cook
schema change shows up in both the editor and the server at once. Cancellation of a running tool, recorded input for
`run_headless`, attach-to-running-editor and `render_frame` are not part of this ADR (see `docs/milestones/M4.5.md`).
