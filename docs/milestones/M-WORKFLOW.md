# M-WORKFLOW — What blind test 3 found in agent authoring (milestone record)

**Status:** active (added 2026-10-16; steps 2, 3 and 4 done). Plan: ROADMAP §M-WORKFLOW; Linear SBA-180 … SBA-184.

**Why:** blind test 3 had three agents build a game (Lantern Keep) over MCP from the SDK and filed GitHub #377 to
#383. #377 to #381 are bugs and workflow holes in code and documents we own; #382 (Luau, UiFlow) belongs to
M-SCRIPT and M-UI-DATA2, #383 (content, texture compression, draw calls) to M-VISUAL2, M-PERF and M-AUDIO2.

**Not in scope:** new content libraries, texture compression, new engine features.

## Steps

| # | Step | Linear | Model | State |
|---|---|---|---|---|
| 1 | `NavAgent3D` reaches its speed on a `CharacterController3D` with the default acceleration (#377) | SBA-180 | Opus | not started |
| 2 | `scene_open` reloads, `scene_save` refuses a changed file, broken links reported (#378) | SBA-181 | Sonnet | done |
| 3 | Cook errors name the fix; `cook_run` `paths` and `skip_errors`; stale pack warning (#379) | SBA-182 | Sonnet | done |
| 4 | Headless `-capture` falls back to offscreen; `-capture_tick`; save folder documented (#380) | SBA-183 | Sonnet | done |
| 5 | Stale documents fixed (#381), the two-server test, the close | SBA-184 | Sonnet | not started |

**Gates:** existing replay hashes unchanged; each fix has a regression test; `local_ci.sh` green on every step.

## Step 4 — #380, SBA-183 (done)

- **What was already there** (the issue was filed against SDK build `ffa87c7c`). `-capture_tick N` and
  `-capture_ticks a,b,c` exist: the sim holds on the tick and the capture is the first frame after it, with
  `-input` played by sim tick; `-platform.video_driver offscreen` was in the command-line table (16.1) and a
  one-line hint in the troubleshooting table; the save folder and `-save_dir` were in 2.5 (`$XDG_DATA_HOME` or
  `~/.local/share`, `%LOCALAPPDATA%`). `save::root()` is `crash::user_data_root()/MCE/MCE/saves`: the engine reads
  `XDG_DATA_HOME` itself and does not use `SDL_GetPrefPath`, so SDL's variables do not matter.
- **What was missing and is now done.** (1) `-capture` / `-capture_tick(s)` with no display still ended in "No
  available video device". Now `player.cpp` (`init_platform_or_offscreen`) retries `platform::init` on the offscreen driver with
  `LIBGL_ALWAYS_SOFTWARE=1` (a value already set wins) when a capture was asked for and SDL could not open a video
  device, and prints `no display for the capture: drawing offscreen with software GL`. An explicit
  `-platform.video_driver` is never replaced. The layer is the player (SDL init), so a project's own host and
  `mce play` get it; the Python CLI sets nothing. (2) The player logs `save folder <path>` (info) once at start.
  (3) Docs: user guide 16.8 (the recipe, which frame each flag gives, the title-menu case), 2.5 (the `<data>` table per
  OS with the override), the 16.1 rows and the troubleshooting row.
- **`-frames` with `-capture`** is the Nth rendered frame, whatever tick the sim is on: `-frames 30` was tick 2 offscreen
  here. That is the "does not land on a fixed tick" of the issue; 16.8 says so and points to `-capture_tick`. The
  behaviour is unchanged (it is the CI smoke the GPU tests use).
- **`timeline_preview` over a title menu: not changed, documented.** The tool starts the real player on the scene
  with `timeline.scrub`; a title menu is drawn by the game (its code or UI document) as a normal part of the scene's
  run, and the engine has nothing that tells it from a HUD, so there is no safe way to leave it out. The tool already
  takes `input` and `convars` to press it away or switch it off; 16.8 and the guide say so.
- **Not done:** a convar that hides all game UI for captures; the capture falls back only when init fails, so a
  display that exists but is dead (`DISPLAY` set to nothing listening) fails as before and needs
  `-platform.video_driver offscreen`.
- **Checked by running it.** In this container (no `DISPLAY`, no GPU) on linux-gcc-debug: `mce_player -pack
  samples/skel2d_demo/project/assets.mce -capture_tick 120 -capture shots/a.ppm` before the change failed with "No
  available video device"; after it prints the fallback line, writes a 1280x720 PPM (2,764,816 bytes) and exits `0
  after 120 ticks`; two runs of `-capture_tick 60` are byte-identical (`cmp`); `-capture_ticks 60,120 -capture
  "t_{tick}.ppm"` writes both; `-frames 30` ends on tick 2; `LIBGL_ALWAYS_SOFTWARE=1 ... -platform.video_driver
  offscreen` and `xvfb-run -a ... env LIBGL_ALWAYS_SOFTWARE=1 ...` work; `-platform.video_driver x11` still fails with
  "x11 not available"; `XDG_DATA_HOME=...` and `-save_dir` move the logged folder. New ctest: `player_save_folder`
  (headless: logged once, XDG, `-save_dir`) and `player_capture_no_display` (GPU label; `DISPLAY`, `WAYLAND_DISPLAY` and
  `SDL_VIDEODRIVER` unset: fallback line, `exit 0 after 30 ticks`, a 320x180 PPM of 172,815 bytes, explicit driver
  respected); both pass.

## Step 3 — #379, SBA-182 (done)

- **What was already there.** The cook already isolates failures: a failed or unregistered file blocks only the
  assets that depend on it, everything else cooks, and a failed asset's last output keeps serving. What an agent
  met was `ok: false`, a pack that was not written, and, for `run_headless`, `render_frame` and the other tools that
  cook the whole project first, a refusal over one unrelated file.
- **The missing-sidecar error names the fix** (`registry.cpp`): `cook_run {"adopt": true}` over MCP,
  `mce cook <project> --adopt` on the command line (the flag exists; `mce build` stops on the same error).
- **`cook_run` `paths`** (`CookOptions::paths`, `mce_cook --only`): files or folders under `assets/`. The scan skips
  the errors of files outside them (and `adopt` does not touch them), the graph keeps the named assets and everything
  they depend on, the rest get `AssetStatus::Skipped`. Skipped assets stay in the manifest with their last output, so
  `cooked/` stays whole. **A subset never writes the pack:** the pack must hold one consistent cook of the whole
  project, so with `paths` it is not rewritten and the report says so (`pack_written: false`, `pack_note`). An entry
  that matches no asset is an error.
- **`skip_errors`** (`CookOptions::skip_errors`, `mce_cook --skip-errors`): the cook always cooked what it could; this
  adds `CookReport::failed_files` (every file with an error and its first message, uncapped, in the tool result as
  `failed_files`). The pack is still written only by a run with no errors: a pack missing a file the game references
  would not load, so `skip_errors` does not relax that.
- **Stale pack warning** (`tools/mce_cli/mce.py`, `stale_pack_warning`): `mce play` and `mce headless` on a game
  project print, on stderr, that `build/project/assets.mce` is older than the newest file under `assets/` and name
  `mce build`. Script projects cook before they run and do not need it.
- **Unknown component or field** (`scene_importer.cpp`, the editor's scene reader): the error adds that the loaded
  module is probably older than the source, to rebuild it (`mce build` or `module_build`) and restart the MCP server
  (`mce mcp <project>`) unless it was started with `--module`; `mce cook` loads the module fresh each time.
- **Docs:** user guide 5.4 "When a cook fails" (the table), 14 "When a cook fails" and the `cook_run` row, 16.4, 17.
- **Not done:** `run_headless`, `render_frame`, `ui_preview` and `timeline_preview` still cook the whole project and
  fail on any error; they do not take `paths`. A `paths` entry that names an unadopted file is its own error (adopt
  first); a file outside the paths that a named asset reaches only by path text is reported as a missing reference.
- **Checked by running it.** `test_cook` has four new cases: the missing-sidecar text names both fixes and
  `failed_files` lists it once; `skip_errors` over an unadopted file and a corrupt PNG cooks the five good assets,
  lists both bad files and writes no pack; `paths: ["materials"]` cooks the material and the texture it needs, skips
  the wav, font and scene, ignores an unadopted file elsewhere, writes no pack and says so, a full cook afterwards
  cooks exactly the rest, a later one-file subset keeps the manifest hash, a path that matches nothing is an error;
  an unknown component and an unknown field both carry the rebuild-and-restart text. `test_mcp` has `cook_run paths
  and skip_errors` over the tool. `cli_stale_pack` (Python) covers the warning function: no warning for a newer
  pack, one naming the newest source and `mce build <project>` for an older one, hidden folders ignored. The MCP
  transcript golden was re-blessed (the `cook_run` description). `ctest -R "mcp|cook|asset|editor|import|cli_stale"`
  passed on linux-gcc-debug except `ai_demo_editor_bt`, which needs a display and fails the same way without this change.

## Step 2 — #378, SBA-181 (done)

- **What was already there.** M-AGENT step 7 already hashed the file at open and save and merged a teammate's
  save by entity, so `scene_save` from a stale copy did not drop their entities. What was still wrong:
  a copy of the scene parked by `scene_switch` came back stale on the next switch, a save that could not be
  merged said nothing about which entities were at stake, and the nulled links were silent.
- **`scene_open`** re-reads the file from disk every time (also when the scene is already open) and drops
  the copy `scene_switch` parked for that file; a parked copy with unsaved edits needs `force`.
- **`scene_save`**: when the file changed on disk it still merges by entity (their new entities are kept,
  unlike a plain refusal). When it cannot merge (no common version, unparsable), it refuses with the entities
  on disk the copy lacks and the ones only the copy has, and names the two ways out: `scene_open {force:true}`
  or `overwrite: true`. `overwrite` is new; it saves over the file like `force` does, without touching
  the save-as rule `force` also has.
- **`entity_delete`** returns `nulled_links` (owning entity id and name, component, field path, the id it
  pointed at) and `deleted_children`, plus a warning, when references from entities that stay became null.
- **Not done: the `project_check` rule.** The reflection data has no required flag for entity-reference
  fields and a null link in a saved file is indistinguishable from an unset one, so there is nothing to check;
  the delete-time report is the detection.
- **Checked by running it.** `ctest -R mcp`: `test_mcp_team` has four new cases on two servers sharing one
  project: a teammate's saved entity and its child show up after `scene_open` and a stale-then-reloaded save drops
  nothing; `scene_open` replaces a parked copy; an unmergeable save is refused naming the entity, `overwrite`
  saves it, `scene_open {force:true}` recovers; deleting `Ball` reports `Camera.CameraFollow2D.target`, a subtree
  delete reports its child and the link to it. The MCP transcript golden was re-blessed (tool descriptions).
