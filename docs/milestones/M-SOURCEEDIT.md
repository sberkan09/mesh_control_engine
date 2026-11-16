# M-SOURCEEDIT — A source editor for every JSON asset (milestone record)

**Status:** done 2026-10-21. **Owner: the "Geliştirme rutini 3" thread** (a gap taken from the assessments: no unowned milestone was left). No Linear issues (Linear is unreachable and the workspace is at its free issue limit); this record is the tracker.
Source: `assessments/motor-erisim-bosluklari.md`, section 11 (Editor): "these file types are all hand-written JSON for people too".

**Goal.** The editor has panels for materials, particle presets, sequences, timelines, trees, UI documents, terrain, skeletons and string tables. Sound sets, sprite sheets, input maps, store and services catalogs, remote config, convar defaults, item catalogs and road networks have none: double-clicking them in Assets does nothing. A **Source** panel opens any JSON source asset as text, shows what the cook says about it while the user types (line and column included), and saves only what the cook accepts.

**Not in scope:** syntax colors, a JSON tree view, reformatting (a rewrite would drop the file's own layout and key order), editing binary imports (textures, meshes, audio clips).

## Steps

| # | Step | State |
|---|---|---|
| 1 | Seam: `check_source` (the in-memory cook of `validate_and_save_source`, without the write) in `mce/editor/asset_form.h`; `SourceDoc` in editor core (`mce/editor/source_doc.h`: open, edit, check, save, revert, problems with line and column), with tests | done |
| 2 | The **Source** panel (multiline text, problem list, Save / Revert / Check), Assets opens it for a double click on a kind with no panel, an "Edit source..." context entry for every JSON source, guide rows | done |

**Gates:** `test_source_doc.cpp` (step 1); step 2 is build-checked (no display in CI).

## Notes

- A file that names its own GUID (every native kind) is no asset without it, so a text that drops the GUID is refused by `check` and `save` (found by the first test: the asset vanished from the catalog on save).
- `validate_and_save_source` now calls `check_source`; its error text is unchanged.
- The cook runs about 0.6 s after the last keystroke, not on every key: it scans the project's asset registry each time.
