# M-LINTALL — One call lints the whole project (milestone record)

**Status:** in progress (step 1). **Owner: the "Geliştirme rutini 3" thread.** Defined 2026-10-27 on the coordinator's suggestion (more agent tooling; device-free, outside M-WASTED and M-VISUAL). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** `scene_lint` (M-SCENELINT) and `scene_lint_fix` (M-LINTFIX) work on one scene at a time. An agent that wants to know whether the project is tidy has to list the assets and call the tool per file. This milestone adds one read-only call over every scene and prefab under `assets/`.

**Design.**
- `scene_lint_all` in `tools_scene_lint.cpp`: lists the files like `project_check` does, parses each scene or prefab and runs `lint_scene`. It answers `clean`, `scenes_checked`, `findings`, `unreadable` and one row per file that has findings or does not parse.
- Read-only. Repair stays `scene_lint_fix` per file, so the agent decides which files to touch.
- No change to the file format, the cook or the runtime.

## Steps

| # | Step | State |
|---|---|---|
| 1 | MCP `scene_lint_all`, a `test_mcp_team` case, capabilities and user-guide rows, the transcript golden | added 2026-10-27, open (PR) |

**Gates:** `test_mcp_team`, `mcp_transcript`, `mcp_symbol_wall`.
