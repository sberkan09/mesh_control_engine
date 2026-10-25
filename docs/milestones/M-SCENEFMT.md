# M-SCENEFMT — Canonical scene text (milestone record)

**Status:** step 1 in progress. **Owner: the "Geliştirme rutini 3" thread** (editor and asset tools). Defined 2026-10-25 on the coordinator's standing request to keep going when the Backlog is empty, in an area that needs no real account or device (editor, scenes and serialization, tools; outside gameplay and the traffic work). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** Scenes are text that people and agents edit by hand. The editor saves one canonical form (`SceneDocument::serialize`: one component object per line, only the fields that differ from their defaults), so a hand edit that spells out a default or reorders keys shows up as noise in the next diff. `mce scene fmt` says which files are not canonical and can rewrite them, so a commit hook or a review step keeps scene diffs about content.

**Not in scope:** changing the canonical form, prefabs and other file kinds, a merge of two edited scenes.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `mce scene fmt <scene>... [--write]` (the `mce_scene_fmt` tool under `mce_cli`), user guide row, a fixture with a default field spelled out, ctest cases: a script test that reports, rewrites and re-checks a copy, and a parse failure | open (PR) |

**Gates:** the ctest cases `scene_fmt_cli_roundtrip` and `scene_fmt_cli_unreadable`.
