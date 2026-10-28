# M-LINTLOOP — Scene lint finds parents that loop (milestone record)

**Status:** open (one step). **Owner: the "Geliştirme rutini 3" thread.** Defined 2026-10-28 on the coordinator's suggestion (more agent tooling; device-free, editor core and MCP only, outside M-WASTED and M-VISUAL). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** A scene where A is under B and B is under A loads fine, but every walk up the hierarchy needs its own guard against the loop (several editor functions already carry one). `lint_scene` does not report it, so neither `scene_lint` nor the repair tools see it. This milestone adds the finding and its repair.

**Design.**
- `LintKind::ParentCycle` in `lint_scene`: reported once per loop, on the entity with the lowest id in it. An entity below a loop is not in it and is not reported. A parent that is the entity itself stays `MissingParent`.
- `fix_scene_lint` removes the `ChildOf` of the reported entity, which breaks the loop with one change (the finding count equals the number of repairs, so the fuzz check `fix == before` keeps holding).
- The MCP kind name is `parent_cycle`; tool descriptions, the user guide and the transcript golden say so.
- No change to the file format, the cook or the runtime.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `LintKind::ParentCycle` with its repair, MCP kind name, two `test_scene_lint` cases, user-guide rows, the transcript golden | open (PR) |

**Gates:** `test_editor` (lint cases), `test_mcp_team`, `mcp_transcript`, `mcp_symbol_wall`, `test_fuzz_misc` (the scene-repair loop).
