# M9 — UC-D: the proof game (milestone record)

**Scope:** ROADMAP M9, "UC-D: a vertical slice, not a second full game: one small level reusing Ember Step's kit components plus one new mechanic, authored exclusively through the project template and editor workflows, shipped through the one-command pipeline". The gate precondition is that the M8 project template is the path UC-D uses. Engine-source changes to gameplay systems are capped at 2 (REQUIREMENTS §2 UC-D, §6 v1.0). This record also covers the v1.0 gate items around it, and which of them an agent could not close.

**Built by:** an agent in the Linux CI container: Debug build `linux-gcc-debug`, with GPU tests under Xvfb and Mesa llvmpipe. The ship zip is built from the default release preset (`linux-gcc-release`).

## What UC-D is: Kindle

`projects/Kindle`, game id `kindle`. One 65 × 14 m level: run right, light the flag.

**The new mechanic is the colour switch.** Every `SwitchBlock` is red or blue. Only blocks of the active colour are solid; the others are drawn as ghosts at 22 % alpha, and the player walks through them. Touching a `ColorSwitch` (a sensor) swaps the active colour, and its lamp shows the colour. A block that turns solid while the player stands inside it waits until they step out, as Ember Step's crumbling blocks do when they grow back.

The level cannot be finished without the mechanic. Red starts solid.

1. A 5-tile red wall blocks the way. The switch before it turns blue on, which removes the wall and makes the blue bridge over a 10-tile pit solid. Jump plus dash clears at most about 8 m.
2. A second switch turns red back on, which raises the red stairs. Without them the plateau with the flag is a 4 m wall; a jump clears 2.5 m.
3. On the plateau, a crumbling bridge over spikes leads to the flag.

Ember Step pieces in the level: the platformer controller (coyote time, jump buffer, variable jump height, wall slide and wall jump, dash), coins, spikes, the goal flag, crumbling blocks, a moving lift with two bonus coins, and Ember Step's art and sounds. Enemies are not in the level (see Gaps).

## How it was made

| Step | How | Hand-written? |
|---|---|---|
| Project scaffold | `python3 scripts/new_project.py Kindle` produced `Kindle.mceproj`, `CMakeLists.txt`, `games/kindle/{module,host}`, the README and `.gitignore`. | No. The template's example content (the `Spin` / `Bob` components, `main.mcscene`, two tiles) was deleted. `Kindle.mceproj`'s `start_scene` was edited by hand to the new scene's GUID (one line; nothing reads it yet). |
| Gameplay | `games/kindle/module/components.h` (10 reflected components, 3 enums) and `kindle.cpp` (8 systems, 361 lines). This is the project's game module and nothing else. Ember Step's components are not in the kit and are not reflected, so its controller, pickup, hazard, goal, crumble and mover logic was ported from `samples/03_platformer/game/platformer.cpp` into authored components. | Code, as UC-D intends: gameplay is the one part written in C++. |
| Content | Ember Step's sprites and win jingle were written out from `art.cpp` (`make_atlas` / `make_clips`) as PNG and WAV. The Kenney CC0 SFX, the `extras_2d` backdrops and ProggyClean were copied. The switch and red / blue block tiles come from `tools/gen_switch_art.py`, following `new_project.py`'s 16×16 PNG writer. Sidecars are new. | Generated or copied; credits in `projects/Kindle/CREDITS.md`. |
| The level | Authored through the editor's MCP server. `tools/author_level_mcp.py`, the MCP client the agent ran, drives `kindle_mcp --project projects/Kindle` over stdio. **Every entity and component went through the editor's `EditorScene` funnel.** The final pass made 772 tool calls: `refl_list_components`, `asset_list` (all GUIDs come from the server), `scene_open`, 263 × `entity_delete` (clearing the previous pass so the GUID stays), 24 × `entity_create` with components, then 239 × `entity_duplicate` + 239 × `component_set LocalTransform.position` (Ctrl+D and a move, per tile), `scene_save`, `cook_run` (ok, 0 errors) and `run_headless` (300 ticks, 263 entities, 0 warnings). `main.mcscene` (2,015 lines) was written by the server's `scene_save`. | **No hand-edited scene JSON.** The layout is the designer's ASCII sketch in the script, and the script turns it into editor commands. A person in `kindle_editor` would do the same with clicks. That equivalence is shown by M4.5's FR-EDT-160 gate (MCP edit == `SceneCommand` edit), not measured here with a human. |
| Look in the editor | `kindle_editor -project projects/Kindle` opens the scene, cooks it (0 errors) and plays it with the game's systems (screenshots). | — |
| Ship | `python3 scripts/ship.py --project projects/Kindle --smoke --smoke-headless` (below). | — |

## Engine changes (the ≤ 2 budget)

**Gameplay systems: 0 changed.** Physics2d, the kit, ECS, audio and scene are untouched.

Outside gameplay systems, two engine-tree changes:

| # | Change | Why |
|---|---|---|
| 1 | **Player runtime input** (`src/player/src/player.cpp`, +20 lines; `src/player/CMakeLists.txt` links `mce_input` on the static path). `mce::player::run` now installs the `InputState` resource, samples the starter-kit default map once per tick (`AppDesc::input_source`) and publishes it before `World::tick`. | Every project host is `mce::player::run`, and it had **no input at all**. The M8 template game (spin and bob) needed none, so no project game could be played. This is the host runtime, not a gameplay system, but it is engine source and counts as one change against the budget. Headless hashes are unchanged in kind: with no source, the resource stays zeroed, and `scene_demo_player_600`, `extras_2d_*` and `template_probe_*` all pass. |
| 2 | **`<game>_mcp`** in `cmake/MceProject.cmake` (+13 lines): `mce_mcp`'s main plus the project's `TYPES`, linked against `mce_mcp_core`. | The stock `mce_mcp` rejects a game's components (`unknown component type`), so a project's scenes could not be authored over MCP. It mirrors `<game>_cook` and `<game>_editor`. It is editor tooling: nothing in the runtime links it. |

Also fixed: `templates/project/README.md` told users to author with `mce_editor`, which rejects the game's components, and placed components in `<game>.h`. Both are corrected for every new project. The template probe now builds `template_probe_mcp` too.

## Evidence

| Gate item | Evidence |
|---|---|
| Built on the M8 template, no hand-rolled scaffold | `projects/Kindle` comes from `new_project.py Kindle`; its `CMakeLists.txt` is the generated `mce_add_project(kindle …)` call plus one project test |
| One new mechanic, used by the level | ctest `kindle_level` (`projects/Kindle/tests/test_kindle.cpp`) loads the **cooked pack the build made from `assets/`**, so it tests the scene as saved, and plays it through the real systems with scripted `InputState`s. The 4 cases (1,011 assertions) all pass. "the authored level has a player, a flag, two switches and both block colours" (9 coins, 3 crumbling blocks, 1 lift; red solid and blue ghost at start). "the red wall blocks the way until a switch turns it into a ghost": running and jumping at the wall stops at x < 17.7, walking back through the switch turns blue on, and the player then crosses the wall onto the bridge at y ≈ 2.45 with 0 deaths. "a block that turns solid around the player waits until they step out". |
| The level can be completed | Same test, "a scripted run toggles both switches and reaches the flag": `cleared in 471 ticks; toggles 2, coins 7/9, deaths 0` (7.85 s), and a second run gives the same `World::state_hash` (FR-RUN-040). With real keyboard input, keys were sent to the windowed host through XTest under Xvfb on a fixed timeline. 3 of 6 runs with targeted jump timings lit the flag (`LIT! 9.4 s` / `9.5 s`). The other 3 missed a jump by a few frames of timing jitter and respawned at the spikes; this is a blind key timeline, not a player. |
| Template tests pass for Kindle | `kindle_headless_300`: `kindle: 300 ticks, 263 entities, state_hash 88b66bdddc9fbd2c`. `kindle_gpu`: `kindle: 60 frames, 67 ticks, 0 GL errors`. `kindle_editor_gpu`: the editor cooks the project (`0 cooked, 27 up to date, 0 failed, 0 error(s)`), plays it (`play stopped after 154 ticks`) and reports `60 frames, 154 ticks, 0 GL errors`. |
| No regressions | Full `linux-gcc-debug` ctest with `MCE_GPU_TESTS=ON` under Xvfb: **71/71 passed**, including `scene_demo_player_600/_gpu/_console_gpu`, `extras_2d_*`, `player_crash_report`, `template_probe_*`, `test_mcp`, `mcp_transcript` and `test_editor`. `scripts/ci/check_walls.py`: all walls hold. `clang-tidy-18` with the repo's `.clang-tidy` on `kindle.cpp`, `test_kindle.cpp`, `types.cpp` and `player.cpp`: no findings. |
| Shipped through the one-command pipeline | `python3 scripts/ship.py --project projects/Kindle --smoke --smoke-headless` (the default `linux-gcc-release` preset) → `dist/Kindle-0.0.1+<commit>-linux-x64.zip` (1,995 KiB: `Kindle`, `libkindle.so`, `libmce_engine.so`, `assets.mce`, `CREDITS.md`, `README.txt`, `VERSION.txt`), plus `…-symbols.zip` beside it. The staged copy ran headless: `kindle: 300 ticks, 263 entities, state_hash 88b66bdddc9fbd2c`, the same hash as the Debug build-tree `kindle_headless_300` (release and debug agree), with the module loaded from its own folder and cheats locked. The zip was unzipped elsewhere and run windowed: `kindle: 120 frames, 94 ticks, 0 GL errors`. |

Screenshots (`/mnt/project-files/m9-ucd/`):

- `kindle-start.png`: red solid, blue bridge a ghost.
- `kindle-blue-state.png`: after the first switch; the wall is a ghost and the player is on the blue bridge.
- `kindle-red-state.png`: after the second switch; the red stairs are solid and the bridge is a ghost.
- `kindle-flag-lit.png`: `LIT! 9.4 s COINS 7/9`.
- `kindle-editor.png`: the project editor with the level and the inspector.
- `kindle-editor-inspector.png`: `ColorSwitch` in the inspector.
- `kindle-editor-play.png`: play-in-editor with the game's systems; blue blocks are ghosts.
- `kindle_mcp_transcript.jsonl`: the authoring session, every JSON-RPC line.

## v1.0 gate items that need a human

These are not closed. An agent cannot close them honestly.

- **Non-programmer double-click test.** Not run. Evidence so far: the release zip runs from its own folder (smoke above), and the level was finished with real key events. A named non-programmer still has to unzip it, double-click `Kindle` and finish the level with the README paragraph as the only help.
- **UC-C formal measurement** (a new UC-A level from an empty scene to cooked, launchable content in under 30 minutes of editor-only actions; 0 crashes in a 2-hour session). Not measured: it needs a person at `kindle_editor` with a clock. Partial evidence: the whole level (263 entities) was authored, saved, cooked and run through the editor funnel in one 0.6 s MCP session, and `kindle_editor_gpu` opens, cooks and plays it with 0 GL errors. Neither is a human authoring time or a soak.
- **The seven learning gates** (LRN docs written from memory plus a self-exam, FR-DX-080). These are the author's own learning record. An agent cannot write them on the author's behalf.
- **Memory budgets reconciled within 20 % of the Tracy memory track.** Not done. That needs a Tracy capture of the shipped build on the dev machine. No Tracy viewer runs in this container, and Kindle has no memory budget table yet.
- **60 fps on the dev machine.** Not measured on hardware. Under llvmpipe in the container, the Debug host ran 2,400 frames in 53 s with 3,202 ticks of sim, about 45 fps at 1280×720 in software GL.

## Gaps found in `docs/engine/*.md` while building (fixed in those pages)

| Needed | Was missing | Now in |
|---|---|---|
| How a game module gets input under the player runtime | It got none: the runtime never installed or published `InputState`, and no page said so | `modules.md` (what `run` does, the default map, headless behaviour), `input.md` (the module's view, the missing input-map asset) |
| How to author a game's components over MCP | `mce_mcp` knows only engine components; no page said a project needs its own server, how a client talks to it, or how to re-author without changing the scene GUID | `editor.md` (project MCP server, value spelling, `entity_duplicate` order, `scene_new` GUID) |
| What MCP `run_headless` and play-in-editor do with a project's game | `run_headless` does not install the game, so its hash is not the host's; play-in-editor feeds no input and its view is the panel size | `editor.md` Known gaps |
| A HUD, frame animation, tiles and what gameplay the kit lacks | The kit page listed components only. Parallax {0, 0} as a HUD, texture swapping for frames, sprite stretching and the missing controller / pickups / follow camera were not written down | `kit.md` "Building a 2D game on the kit" |
| Physics authoring conventions | That `layer` is an index and `mask` a bit set, the kinematic-character recipe, bodies only existing after the first step, and that `set_enabled` survives the ECS bridge | `physics2d.md` Rules |

## Gaps

- **Ember Step's components are not reusable as they stand.** They live in the sample, unreflected and spawned from code. Kindle reuses them by porting about 250 lines into its module. A shared, reflected "platformer kit" (in `mce_gameplay_kit` or a shared game library) would turn the next game's reuse into plain authoring. It was not built, because it would have been a third engine change.
- **No enemies in the level.** Ember Step's enemy FSM was not ported. The slice needed one new mechanic, not every old one.
- **No project input map.** Kindle dashes on `Fire` (J / X) because the default map has no `Dash`, and bindings are not persisted in the runtime (`modules.md` Known gaps).
- **Play-in-editor cannot be played**, only watched, because it has no input (`editor.md`).
- **The lift's two bonus coins** are not covered by the scripted run (7/9 coins). Their reachability rests on the lift's 4 → 7 m travel against a 2.5 m jump, not on a test.
- **Ember Step's particles, screen shake and music** are host code in the sample, not components, so Kindle has none of them.
- **The MCP authoring run took 0.6 s.** It says nothing about how long a person would take in the editor (UC-C, above).
