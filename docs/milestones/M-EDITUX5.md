# M-EDITUX5 — reloading the scene file over the undo history (milestone record)

**Status:** done 2026-11-17 (the fuzz found no defect). **Owner: the "Geliştirme rutini 3" thread.** Follow-up of
[M-EDITUX4](M-EDITUX4.md), defined on the coordinator's instruction. No Linear issues. Editor tests only (no source change).

## Why

`EditorScene::reload_text` replaces the entities when the scene file changes on disk (another tool, an MCP client) as one
undoable entry. It was tested alone, never between other edits, where a stale selection, a reused id or a half-recorded edit
group would show.

## Steps

| # | Step | State |
|---|---|---|
| 1 | A fourth mode of the multi-entity fuzz reloads the file in a third of the commands: the text of this scene after edits made in a second editor (create, rename, delete), the text unchanged, or text that is not a scene; same invariants (one entry or none, undo all byte-identical, redo all exact, `verify_world`) | done 2026-11-17: `tests/editor/test_multi_commands.cpp`, 30 seeds x 120 commands, 12,010 assertions; about 790 reloads made an entry, about 240 unchanged reloads made none, about 160 bad texts were refused with nothing changed |
| 2 | Fix what step 1 finds | not needed |
