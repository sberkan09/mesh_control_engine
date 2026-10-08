# ADR-0029: Play-in-editor, hot reload and the orbit-camera gate (M5b editor side)

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

## 2. Hot reload

1. **Polling, not OS notifications.** `AssetWatcher` rescans `assets/` (path, size, mtime) every
   `editor.hot_reload_interval` seconds (default 0.5; 0 turns it off), the same polling choice ADR-0015 made for the
   cook watch mode: portable, and cheap at project scale. A change list (relative paths) goes to
   `EditorSession::hot_reload`; the File menu's "Hot reload now" forces one.
2. **Re-cook incrementally, swap in place.** `EditorProject::reload` runs the normal cook (the cook DB re-cooks what
   changed and its dependents) and, when the mounted tree already knows every cooked asset and none went away, feeds
   each re-cooked output to `AssetSystem::commit_reload`. Handles stay valid; resident assets bump their generation,
   so the viewport's sprite/material caches, the play world and the clip bank re-resolve on their next frame. Play
   keeps running across a swap.
3. **New or removed assets remount.** Play stops and the preview detaches first (both hold the mount being
   replaced), then the fresh mount is attached.
4. **A failed re-cook keeps the previous output.** The error goes to the console with `file:line`; the old
   generation keeps serving, so a broken texture never blanks the viewport.
5. **The open scene follows its file.** When the open scene's file changed on disk (an MCP client or a text editor
   saved it) and the document has no unsaved edits, it is re-opened from disk; with unsaved edits the document is
   kept and a warning logged.
6. **Game modules are timed on the module host** (§1.7): `scripts/module_reload_stopwatch.py` touches a module
   source, rebuilds the module target with ccache disabled, and has `mce_devhost` load the fresh image and tick.

**Measured (Linux container, Debug):** texture edit → swapped 2.7 ms, material 6.4 ms, scene 6.3 ms (budget 1 s,
`test_hot_reload`); `samples/03_platformer` module edit → rebuilt, loaded and ticked: median 2.66 s, worst 2.71 s
over 3 rounds (budget 10 s).

## 3. The orbit-camera collision gate

1. **A 3D inspection view, not a 3D editor.** View → "3D orbit view" (O) swaps the 2D viewport for an orbit camera
   around the scene: left drag orbits, right/middle drag pans, the wheel dollies, F frames the colliders. It draws the
   scene's colliders as wireframe boxes. Picking and gizmos stay in the 2D view.
2. **The gate is `World3D::camera_sweep`.** `OrbitCamera::resolve` sweeps a sphere (`probe_radius`, 0.2 m) along the
   boom from the target to the eye against static and kinematic geometry. The camera stops in front of a wall or
   above the floor, and slides back out as the obstacle clears. This is the same query the runtime's third-person
   boom uses (the `04_cubes` walk).
3. **The world comes from the document.** `OrbitColliders` turns every entity with a `LocalTransform` and a
   non-sensor `Collider2D` into a static box. The 2D shape is extruded `depth` metres along Z (View → Collider
   depth, default 1 m). It rebuilds only when an entity it reads changed. Sensors never
   stop the camera. A 3D collider component joins it when the ECS physics3d bridge lands.
4. **Off switch.** View → "Orbit camera collision" turns the gate off to look through geometry on purpose.
