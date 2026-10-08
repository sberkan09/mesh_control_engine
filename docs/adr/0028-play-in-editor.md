# ADR-0028: Play-in-editor, hot reload and the orbit-camera gate (M5b editor side)

**Status:** Accepted (2026-10-08). **Milestone:** M5b. **Requirements:** FR-EDT-060, NFR-M-03, FR-AST-090,
FR-DX-040. **Design:** `docs/architecture/editor-tools.md` §2.8, §4.1.

**Context.** M5b's editor half has three items: play-in-editor (embedded host, pause/step/slow-mo, ≤ 1 s enter,
Play→Stop→Play hash), the hot-reload matrix (textures/materials/scenes ≤ 1 s, game module ≤ 10 s) and the camera
collision gate for an editor orbit camera. The design (§2.8) sketched a `PlaySession` that "serializes the document
to a transient scene chunk and constructs the runtime host exactly as game.exe does". It left open how the chunk is
made without writing the user's file, which host the play world copies, and what the editor does with game modules.

## 1. Play-in-editor

1. **The document is cooked in memory, not saved.** `PlaySession::enter` serializes the `SceneDocument` and hands the
   text to the scene importer through a new cook entry point, `cook::cook_document(registry, importer, guid, path,
   text, out, diags)`. It runs the same reference checks as the cook's graph build (missing or mistyped assets are
   `file:line` errors) and the same importer, so prefab instances are flattened exactly as in a cooked pack. Nothing
   on disk changes: the open scene may be unsaved, and Play does not save it.
2. **The play world is `mce_player -headless`'s world.** `install_physics2d`, the audio bridge on a Silent device
   with a `ClipBank` over the project's mounted tree, then `scene::instantiate` of the in-memory chunk. Ticks are the
   app loop's: `dt = 1/60` exactly, an accumulator fed by wall time × time scale, at most 5 ticks per frame. The
   same scene and tick count therefore give the same `World::state_hash` as `mce_player -headless 1 -ticks N` on
   the cooked pack (tested at 600 ticks against the player binary).
3. **Pause, single-step and slow motion ride the fixed step.** Pause stops the accumulator; Step runs exactly one
   tick; the time scale (0.05×–8×) scales the inflow, never `dt`, so determinism (FR-RUN-050) holds at every speed.
4. **Play→Stop→Play is idempotent by construction.** Stop destroys the play world; the document and the preview
   world are never written while playing. Entering twice yields the same post-instantiate hash, the same first-tick
   hash and the same hash after N ticks (tested). The design's "play-mode edits are discarded" holds trivially: the
   Hierarchy and Inspector are read-only while playing, and the Play panel shows the play world's live values.
5. **Editor surface.** F5 plays in the editor and stops; F6 pauses/resumes; F10 steps; the speed slider sets the
   time scale. Run-in-player (the MVP path, a child `mce_player`) stays in the File menu as Ctrl+F5. The viewport
   shows the play world through the scene's primary camera (View → "Game camera while playing" switches to the
   editor camera). The Play panel holds the controls, tick/hash/enter time, the selected entity's live component
   values (the `.mcscene` text of each component, read from the play world) and the event-flow monitor
   (`World::event_registry`: readable and total counts per event type).
6. **Cook, open and new stop play.** The play world reads the project's mounted cooked tree, which a cook replaces.
7. **Game modules stay out of the editor process for now.** A game module must share one engine instance with its
   host, so hosts that load modules link the `mce_engine` umbrella and no static engine library (src/module). The
   editor links the static libraries, so it cannot load a module in-process without relinking the whole editor
   against the umbrella. Play-in-editor therefore runs data-only scenes (as `mce_player` does today); module
   projects keep running through their own host. The game-module half of the hot-reload matrix is measured on the
   module host.

**Measured (Linux container, Debug):** enter 1.1 ms for `scene_demo` (6 entities; budget 1 s).
