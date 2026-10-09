# MCP dogfood: Prism Break

*2026-10-09.* An AI developer (Claude, in a Linux cloud container, no GPU) made a small new game with
the engine as an outside user would: `scripts/new_project.py`, the project's game module (C++
gameplay code, the part a game developer writes), and the project's editor MCP server
(`prism_break_mcp`) for every scene edit. This page records what the MCP workflow did well and every
point of friction, with what was fixed and what is still open.

The game: `projects/PrismBreak/`, a brick breaker. A paddle, a ball, 55 bricks in five rows. Normal
bricks take 1 to 3 hits (tinted by the hits left), prism bricks split the ball in two, steel bricks
never break. Three lives; Space serves and restarts. `scenes/attract.mcscene` is the same level with
the paddle on autopilot.

## How it was made

| Step | Tool | Hand-written? |
|---|---|---|
| Project skeleton | `python3 scripts/new_project.py PrismBreak --title "Prism Break"` | no |
| Components and systems | `games/prism_break/module/` (≈ 530 lines of C++) | yes, by design: gameplay is code |
| Sprites | `tools/gen_art.py` (PNG writer, standard library only) | yes: no art tool in the loop |
| Sounds and font | copied from `projects/Kindle/assets/` | yes (file copies) |
| Sidecars for the new files | `cook_run` with `adopt` (fix 2 below); before the fix, `prism_break_cook --adopt` | no |
| The level (65 entities) | `tools/author_level_mcp.py`: 194 MCP tool calls (`entity_create`, `entity_duplicate`, `component_set`, `scene_save`) | no `.mcscene` was hand-edited |
| Start scene | `PrismBreak.mceproj` `start_scene` edited by hand | yes (open item 2) |
| Verification | `run_headless` with `inspect` (fix 3), `prism_break -headless`, Xvfb captures | no |
| Ship | `python3 scripts/ship.py --project projects/PrismBreak --smoke --smoke-headless` | no |

The attract scene clears the level headless in 11 101 ticks (≈ 3 min of game time) with three
prism splits; `run_headless` through `prism_break_mcp` and `prism_break -headless 1` give the same
`state_hash` for the same scene and ticks.

## What worked

- `new_project.py` is one command and the result builds: host, module, cook, editor and MCP server
  targets appear after a reconfigure.
- The tool table is enough to author a whole level. `refl_describe_component` documents the game's
  own components (ranges, help text), `entity_duplicate` + `component_set` with a property path
  (`"position"`) is a comfortable Ctrl+D workflow, and errors are precise (`the entity does not have
  that component: use component_add`, `inspect.entities: no entity with that id`).
- `scene_save` with a new path gives the copy a fresh GUID, so "save as" makes a second scene
  safely; `cook_run` diagnostics carry file, line and column.
- Kindle's `author_level_mcp.py` was a good template for a client; copying it took minutes.

## Friction, fixed in this round

Fixes 1 to 3 are PR #71; 4 and 5 came with the game's PR.

1. **`run_headless` never ran the game.** `<game>_mcp` linked only the game's types, and
   `headless_run.cpp` installed physics2d and audio only, so a project server's run had no game
   systems (and no input, particles or sequences, which the player installs). Gameplay could not be
   verified through MCP at all, and the hash differed from `<game> -headless`. Now `<game>_mcp`
   links `GAME_SOURCES` and hands `install()` to `run_headless` through the play-in-editor hook,
   and `run_headless` installs what `mce_player` installs. Kindle: `88b66bdddc9fbd2c` both ways.
2. **New assets could not be registered through MCP.** A new PNG/WAV/TTF is a cook error until it
   has a `.mceasset` sidecar; the error said "run the editor or mce_cook --adopt", but the editor
   never adopts. `cook_run` now takes `"adopt": true`, and the message names it.
3. **No way to see what a run did.** `run_headless` returned a hash, metrics and the log tail only.
   It now takes `inspect` (`{"entities": [ids], "component": "Type", "show": [...]}`) and returns the
   live component values after the last tick, Transient runtime fields included, also for entities
   the run spawned (the split balls). That is how the brick and ball state were checked.
4. **A relative `--project` path saved scenes to the wrong place.** With `--project
   projects/PrismBreak`, `scene_save` with a new path wrote `assets/projects/PrismBreak/assets/
   scenes/attract.mcscene`: the session resolved the already-joined relative path against `assets/`
   a second time. `EditorProject::open` now stores an absolute root (`cook::fs::absolute`).
5. **The `.mceproj` `start_scene` was never read.** The host played the first scene by path, so
   adding `scenes/attract.mcscene` silently made the shipped game start in attract mode.
   `mce_add_project` now reads `start_scene` at configure time and the template host passes it as
   `PlayerDesc::start_scene`; `-scene` still overrides it.

## Friction, still open

| # | What | Cost here | Suggested fix |
|---|---|---|---|
| 1 | A scene that names a component the game no longer has cannot be opened (`unknown component type 'Spin'`), and no tool repairs it. The template scene uses the template's `Spin`/`Bob`, so replacing them made the starting scene unopenable. | Started a new scene instead (fresh GUID), see 2. | `scene_open` with `drop_unknown: true`, reporting what it dropped. |
| 2 | Nothing edits the `.mceproj`: after `scene_new` the project's `start_scene` still names the old GUID (now that the host reads it, fix 5, a stale one fails the start). | One hand edit of `PrismBreak.mceproj`. | A `project_info` / `project_set_start_scene` tool, or `scene_save` offering to update it. |
| 3 | No scripted input in `run_headless`: a headless run cannot press Space, so a paddle game cannot be played headless. | The game got an `autopilot` field and an attract scene. | `run_headless` `input`: a per-tick list of action states written into `InputState`. |
| 4 | No `render_frame` (the M4.5 stretch item): the agent cannot see the scene through MCP. | Screenshots came from `prism_break -frames N -capture x.ppm` under Xvfb. | Ship the stretch item: an offscreen frame as MCP image content. |
| 5 | Every project re-implements the stdio client (`Mcp` class in Kindle's and Prism Break's scripts); an agent whose host cannot register a new MCP server mid-session has no other way in. | ≈ 60 lines copied. | `scripts/mcp_client.py`: a reusable client (and a JSONL batch mode). |
| 6 | A project whose content does not cook breaks the whole engine build (`<game>_content` is in `ALL`), so a half-made game blocks every other target and local CI. | The first build failed on the new project's missing sidecars. | Build project content with its host target only, or warn instead of fail outside the project's own targets. |
| 7 | `scene_save` to an existing path with `force` re-generates the GUID on every run of the authoring script, so the attract scene's GUID churns. | Harmless here (nothing references it). | Keep the target file's GUID when overwriting with `force`. |
| 8 | The template ships no font and its two placeholder textures; a HUD needs a font copied in. | Copied ProggyClean from Kindle. | Ship ProggyClean (MIT) in the template. |
| 10 | The module host names its copy `lib<game>_loaded_<N>` with N unique only inside one process: two hosts started at once from one directory (parallel ctest, or a player launching the shipped game twice) overwrite each other's mapped copy and one dies with SIGBUS. `mce_add_project` puts a `RESOURCE_LOCK` on the tests it knows, but a project's own `add_test` after it gets none. | The first CI run of this project: `prism_break_attract_clears` crashed with a bus error. Fixed here with the lock. | Put the process id in the copy name (`_loaded_<pid>_<N>`). |
| 9 | Without `--scene`, the server opens an untitled scene, not the project's start scene. | One extra `scene_open`. | Open `start_scene` from the `.mceproj` by default. |

## Numbers

- MCP tool calls for the level, the attract copy and the check: 195 (one script run, about 2.5 s).
- `run_headless` through `prism_break_mcp` and `prism_break -headless 1` on the attract scene, 12 000
  ticks: both `ea22a703dffb44a7`.
- Game code: `components.h` 93 lines, `prism_break.h` 43, `prism_break.cpp` 391.
- Hand edits outside game code and art: one line in `PrismBreak.mceproj`.
