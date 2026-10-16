# M-WORKFLOW — What blind test 3 found in agent authoring (milestone record)

**Status:** active (added 2026-10-16; steps 1, 2 and 3 done). Plan: ROADMAP §M-WORKFLOW; Linear SBA-180 … SBA-184.

**Why:** blind test 3 had three agents build a game (Lantern Keep) over MCP from the SDK and filed GitHub #377 to
#383. #377 to #381 are bugs and workflow holes in code and documents we own; #382 (Luau, UiFlow) belongs to
M-SCRIPT and M-UI-DATA2, #383 (content, texture compression, draw calls) to M-VISUAL2, M-PERF and M-AUDIO2.

**Not in scope:** new content libraries, texture compression, new engine features.

## Steps

| # | Step | Linear | Model | State |
|---|---|---|---|---|
| 1 | `NavAgent3D` reaches its speed on a `CharacterController3D` with the default acceleration (#377) | SBA-180 | Opus | done |
| 2 | `scene_open` reloads, `scene_save` refuses a changed file, broken links reported (#378) | SBA-181 | Sonnet | done |
| 3 | Cook errors name the fix; `cook_run` `paths` and `skip_errors`; stale pack warning (#379) | SBA-182 | Sonnet | done |
| 4 | Headless `-capture` falls back to offscreen; `-capture_tick`; save folder documented (#380) | SBA-183 | Sonnet | not started |
| 5 | Stale documents fixed (#381), the two-server test, the close | SBA-184 | Sonnet | not started |
| 6 | Perceiver forward documented and visible over MCP (`perception_debug_lines`), stale baked component defaults, test bots that crouch and act (#391) | not filed | Sonnet | not started |
| 7 | Luau entity lookup by name, UiFlow reacting to signals and `GameState`, `pause` keeping audio, cutscenes not holding scripts, UI scale, Retry not reloading the title (#382, added on Sami's word 2026-10-16) | not filed | Opus | not started |

**Gates:** existing replay hashes unchanged; each fix has a regression test; `local_ci.sh` green on every step.

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

## Step 1 — #377 (done)

- **Cause.** Not the nav agent's acceleration: `kit3d_bodies` treated any change of the entity's pose as a
  teleport and zeroed the controller's velocity. `NavAgent3D` with `face_motion` (the default) turns the entity
  every tick, so the controller restarted its acceleration ramp every tick and walked at one tick's worth of
  `acceleration` (about 0.13 m/s of 3.5). With `acceleration` 0 the ramp does not exist, which is why that hid it.
- **Fix.** A rotation-only change still teleports the capsule but keeps `velocity`; only a moved position resets it.
- **Tests.** Two cases in `test_nav3d.cpp`: a steady walk reaches the agent's speed at accelerations 8, 20 and 60,
  and a patrol whose post keeps moving (the agent turns every tick) covers 3 s at nearly full speed. User guide 9.
- **The 20 m test (SBA-180 follow-up).** A third case walks the agent the issue's 20 m to arrival in two legs (10 m
  along +X, then a retarget onto a 10 m diagonal) at `acceleration` 14 and 0: the eased walk keeps over 90% of the
  agent's speed mid-leg and arrives within 1.5 s of the instant one. Without the fix it takes 11.25 s against 7.57 s.
