# M-WORKFLOW — What blind test 3 found in agent authoring (milestone record)

**Status:** planned (added 2026-10-16). Plan: ROADMAP §M-WORKFLOW; Linear SBA-180 … SBA-184.

**Why:** blind test 3 had three agents build a game (Lantern Keep) over MCP from the SDK and filed GitHub #377 to
#383. #377 to #381 are bugs and workflow holes in code and documents we own; #382 (Luau, UiFlow) belongs to
M-SCRIPT and M-UI-DATA2, #383 (content, texture compression, draw calls) to M-VISUAL2, M-PERF and M-AUDIO2.

**Not in scope:** new content libraries, texture compression, new engine features.

## Steps

| # | Step | Linear | Model | State |
|---|---|---|---|---|
| 1 | `NavAgent3D` reaches its speed on a `CharacterController3D` with the default acceleration (#377) | SBA-180 | Opus | not started |
| 2 | `scene_open` reloads, `scene_save` refuses a changed file, broken links reported (#378) | SBA-181 | Sonnet | not started |
| 3 | Cook errors name the fix; `cook_run` `paths` and `skip_errors`; stale pack warning (#379) | SBA-182 | Sonnet | not started |
| 4 | Headless `-capture` falls back to offscreen; `-capture_tick`; save folder documented (#380) | SBA-183 | Sonnet | not started |
| 5 | Stale documents fixed (#381), the two-server test, the close | SBA-184 | Sonnet | not started |

**Gates:** existing replay hashes unchanged; each fix has a regression test; `local_ci.sh` green on every step.
