# M-LINTFIXALL — One call repairs the whole project (milestone record)

**Status:** done (one step). **Owner: the "Geliştirme rutini 3" thread.** Defined 2026-10-27 on the coordinator's suggestion (more agent tooling; device-free, outside M-WASTED and M-VISUAL). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** `scene_lint_all` (M-LINTALL) tells an agent which files are untidy, and `scene_lint_fix` (M-LINTFIX) repairs one file. Tidying a project still takes one call per file. This milestone lets `scene_lint_all` repair what it finds when asked to.

**Design.**
- `scene_lint_all` gets an optional `fix` boolean (default false, so the call stays read-only unless asked). With `fix: true` each file with findings goes through `fix_scene_lint` and is written back in the canonical text, as `scene_lint_fix` does.
- The answer adds `fixed` (total and per file). `clean` is true when nothing unreadable is left, since the files are tidy afterwards.
- No new tool, no change to the file format, the cook or the runtime.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `fix` on MCP `scene_lint_all`, a `test_mcp_team` case, user-guide row, the transcript golden | done (#823) |

**Gates:** `test_mcp_team`, `mcp_transcript`, `mcp_symbol_wall`.
