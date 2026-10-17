# M-HARDEN — The MCP server's own lifetime and concurrency bugs (milestone record)

**Status:** done 2026-10-16 (added 2026-10-16). Plan: ROADMAP §M-HARDEN; Linear SBA-195 … SBA-197.

**Why:** M-WORKFLOW step 5 (SBA-184) found that `ui_preview` kept a `ProjectAsset*` across `render_ticks`; the cook inside
rescans the project and frees those records, so the reply read freed memory. The Opus review of that fix confirmed the
cause and asked for the pattern to be removed for good: `ui_document_arg` should hand out a value (path and GUID), so a
`ProjectAsset*` is never carried past a rescan. The same step also left two things open: the two-server test uses two
`Server` objects, not two processes, and nothing runs the MCP tests under a sanitizer, which is how a use-after-free
survived.

| # | Step | Linear | Model | State |
|---|---|---|---|---|
| 1 | Handles by value: `ui_document_arg` and the other `ProjectAsset*` uses audited | SBA-195 | Sonnet | done |
| 2 | `run_transcript.py` drives several `mce_mcp` processes; two-process scene test | SBA-196 | Sonnet | done |
| 3 | MCP and transcript tests in the ASan stage; findings fixed; the close | SBA-197 | Sonnet | done |

**Gates:** existing replay hashes unchanged; each fix has a regression test; `local_ci.sh` green on every step.

## Step 1 — handles by value, SBA-195 (done)

**Rule now:** a `ProjectAsset*` (or reference into `project.assets()`) is never kept across a call that can rescan the
project: `cook_run`, `session().cook(...)` (so `render_ticks`, `run_headless`, `bt_trace`, `perf_run`, `test_run`,
`project_check`, `ui_preview`, `timeline_preview`, `render_frame`), `asset_validate_and_write` (every JSON asset write),
`asset_write`/`asset_create`/import/delete, `project.rescan()`. Path and GUID are copied out first.

**Changed**
- `ui_document_arg` (`tools.h`, `tools_ui.cpp`) returns a `UiDocRef {path, guid}` by value; `ui_preview` holds that and no
  longer needs the manual copy-and-null of PR #418.
- `ui_widget_add/set/remove` (`apply_edit`) and `ui_theme_set`: the reply read `a.path`/`a.guid` (and the theme's `path`)
  after `asset_validate_and_write` had rescanned. Copied before the write. Same defect as #418, not yet hit by a test.
- `timeline_add_track/add_key` (`save_edit`): same, copied before the write.
- Terrain tools (`terrain_sculpt/paint/import/generate`): `write_head` read the record after `save_terrain`'s write.
  `load_terrain` now returns a local `TerrainRef {path, guid}`; no `ProjectAsset*` leaves it.
- `bt_trace`: `a->path` was read after `session().cook(false)`. Path copied before it.

**Checked, already safe (copy before the rescan, or no rescanning call after the lookup)**
`asset_write`, `tilemap_paint`, `asset_set_import_settings` (copies before `rescan()`), `skel2d_*` (`SkelFile` by value),
`bt_add_node/bt_set/save_tree` (`kept` copy; guid by value), `entity_create` (prefab guid copied at once), `navmesh_bake`
reply, `asset_read`, `asset_list`, `ui_list`, `project_get` (start scene path), `render_frame/render_sheet/timeline_preview`
via `parse_common` and `perf_run`, `run_headless` and `test_run` (guid copied out of the lookup loop), `project_check`
(loops re-read `project.assets()` after its own `rescan()`/`cook()`; `run_headless` there does not rescan),
`check_ui_document` theme/texture lookups (read at once).

**Left:** the `arg_asset`/`arg_document`/`arg_tree`/`arg_timeline` lookups still return a `ProjectAsset*` to their own
tool body. They are used before any rescanning call now, and a by-value return for each would be churn with no
remaining hazard; the one that crossed into another file (`ui_document_arg`) is the one that became a value. No
behaviour change, replay hashes untouched.

**Tests:** `ui edits echo their own document and GUID after the write rescans the project` (`test_mcp_ui.cpp`, ui edits and
`ui_theme_set` over two documents); gpu suite: `ui_preview: two documents back to back ...` (path and GUID, a write
between the calls) and `timeline_preview: two scenes back to back ...` (`test_mcp_timeline.cpp`); the terrain test
checks the echoed path and GUID. A rescan forced *between* lookup and use is not reachable from the tool surface
(the lookup and the use are one call), so the ASan stage of step 3 is where that class is caught for good.

## Step 2 — several real servers, SBA-196 (done)

**Changed:** `tests/mcp/run_transcript.py` learned a second transcript kind, `<name>.multi.jsonl`: lines of
`{"server":"a","msg":{...}}`. It copies `samples/scene_demo` into one project, starts one `mce_mcp` per distinct server
name, and sends the lines strictly one at a time (a message with an `id` waits for exactly one reply line from that
server, 60 s timeout; notifications get none), so the cross-process order is fixed. At the end it closes every stdin
and checks every exit code. The golden `<name>.multi.golden.jsonl` has `{"server","response"}` per request, normalized
by the same functions as the single-server goldens. Single-server transcripts and `--update` are unchanged; the one
ctest `mcp_transcript` runs both kinds.

**Transcript `two_servers_scene`:** a and b open the same scene; a adds Lantern and Door, b adds Guard and Key; both
save (the second save merges by entity); `scene_info`/`scene_list_entities` on both show all four (a's open copy is
stale, 8 entities, until it reopens, 10 after); then a and b set `Ball` `Collider2D.restitution` differently, a saves,
b's save is refused naming `entity 5 "Ball" components/Collider2D` ("nothing was written"), and `overwrite:true` saves
b's copy with the entity count still 10.

**Found:** no concurrency defect; the merge and the refusal behave as in the in-process `test_mcp_team` case.

## Step 3 — the MCP tests under ASan, SBA-197 (done)

**What runs under ASan now:** nothing had to be added to the stage. `linux-asan` (g++, Debug, `MCE_ASAN=ON`, editor on,
so `mce_mcp`, `mce_mcp_core` and `mce_player` are built with `-fsanitize=address,undefined`) registers and runs every
non-`slow` test, and no MCP test is labelled `slow`. So `test_mcp` (with the template-probe module and `test_mcp_script`),
`mcp_transcript` (the real `mce_mcp` binary, single-server and the `two_servers_scene` pair), `mcp_symbol_wall`,
`test_mcp_agent` (agent, ui, skeleton2d, import, timeline, bt suites; `-tse=gpu`), `test_mcp_playtest`, `test_mcp_docs`,
`test_mcp_content`, `test_mcp_gameplay3d`, `test_mcp_check` and `test_mcp_team` were already sanitized in the ASan stage;
the finding of step 1 simply predates anyone looking. `ctest --preset linux-asan -R mcp -j2`: 10 of 10 pass, about 25 s.

**Changed (two gaps in what a report could do)**
- UBSan only prints by default, so a report in a `mce_mcp` child of `mcp_transcript` (stderr was `DEVNULL` in the multi-server
  runner) or in any test would not have failed anything. `tests/mcp/CMakeLists.txt` sets
  `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` on every MCP test when `MCE_ASAN` is on (not MSVC), so a report aborts
  the process and the test fails; the runner now lets the servers' stderr through, so the report reaches the ctest log.
  ASan itself already aborted.

**Found:** no ASan, UBSan or LeakSanitizer finding in the editor or MCP code, with the default options and again with
halting UBSan, `detect_stack_use_after_return=1` and leak detection. Nothing to fix, so no new regression test; the
step 1 tests are what exercises the fixed paths, now under the sanitizer.

**GPU suite (not in the stage, `MCE_GPU_TESTS` is OFF there):** run once by hand in the same tree with
`-DMCE_GPU_TESTS=ON` under `xvfb-run` and Mesa llvmpipe: `test_mcp_agent_gpu` (`ui_preview`, `timeline_preview`,
`render_frame`, `render_sheet`, `image_diff`, 9 cases, 732 assertions) passes with no ASan or UBSan error in the test
process. Every `mce_player` child it starts prints a LeakSanitizer report at exit (2,792 bytes in 3 allocations) from
Mesa's EGL initialisation reached through `SDL_EGL_InitializeOffscreen` (`SDL_CreateWindow`, `player.cpp:3003`): third
party, not ours, and not fatal (the test passes), so left alone. This is why the GPU suite is not added to the stage:
it needs Xvfb and takes 35 s for a tree that otherwise runs no GPU test.

**Not covered:** the GPU suite in the regular stage (above); `test_mcp_agent_gpu` is run under ASan only by hand. Two real
processes are covered by the transcript. Thread-level races (TSan) were not looked for.
