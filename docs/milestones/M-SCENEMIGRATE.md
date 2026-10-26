# M-SCENEMIGRATE — Scenes survive a renamed component or field (milestone record)

**Status:** step 1 open. **Owner: the "Geliştirme rutini 3" thread.** Defined 2026-10-26 on the coordinator's suggestion (serialization versioning and migration; device-free, outside traffic, pedestrians and M-VISUAL). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** `SceneDocument::parse` refuses a scene newer than `kFormatVersion` and has an empty migrator table (`scene_document.cpp`, "Older versions migrate here"). Today renaming a component or a field in C++ makes every saved scene fail with an unknown-component or unknown-field error, and the only cure is editing each file by hand. This milestone gives the engine a place to say "`OldName` is now `NewName`" once, so old scenes and prefabs load, and `mce scene fmt --write` rewrites them in the current spelling.

**Design.**
- A migration table in `mce_editor_core`: rows `{from_version, kind: component | field, old, new}` consulted while a component record is read. A row applies to files with `version <= from_version`.
- The format version stays an integer in the file; a bump adds rows, never code paths per version.
- Reading an old file never fails because of a renamed name; it loads under the new name and a `migrated` note is recorded so tools can say so. Saving writes the current version.
- No change to the cooked formats (the cook reads the same documents) or to the runtime.

## Steps

| # | Step | State |
|---|---|---|
| 1 | The migration table and its use in `SceneDocument::parse` (component and field renames), a `test_editor` case with an old-spelling document | open |
| 2 | `mce_scene_fmt` reports and, with `--write`, rewrites migrated files; `scene_lint` / `scene_stats` MCP results carry the migrated note; user guide rows | open |

**Gates:** `test_editor`, `scene_fmt_cli_*` ctests, `mcp_transcript` if a tool description changes.
