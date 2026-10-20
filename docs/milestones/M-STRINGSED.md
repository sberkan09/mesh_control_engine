# M-STRINGSED — Editing string tables in the editor (milestone record)

**Status:** done 2026-10-20. **Owner: the "Geliştirme rutini 3" thread** (a gap taken from the assessments at the coordinator's request: no unowned milestone was left). No Linear issues (Linear is unreachable and the workspace is at its free issue limit); this record is the tracker.
Source: `assessments/motor-erisim-bosluklari.md`, section 11 (Editor): "Editörü olmayanlar: ... yerelleştirme ...". String tables are `assets/loc/<locale>.csv`; translating means editing several CSV files side by side in a text editor and finding the missing keys by running the cook.

**Goal.** A `Strings` panel shows every locale's table as one grid: a row per key, a column per locale, the base locale first. A cell is edited in place, keys are added and removed across the locales, the cells a locale lacks are marked and counted, and a save writes each changed CSV through the cook-first write (`validate_and_save_source`): a table the cook refuses (a bad key, a plural form without `#other`, invalid UTF-8) is not written and the panel shows why.

**Not in scope:** machine translation, importing from spreadsheets or `.po` files, plural-form helpers beyond editing the `#one` / `#other` rows as plain keys, previewing text in the UI.

## Steps
| Step | What | Modules | Model |
|---|---|---|---|
| 1. Strings grid | `StringsGrid` in editor core (`mce/editor/strings_grid.h`): loads every `StringTable` asset (RFC 4180 reader that keeps comment rows and extra columns), the union of keys with the base locale first, `set_text`, `add_key`, `remove_key`, `missing`, and a cook-first `save` of the changed locales; headless tests | editor core | Sonnet |
| 2. Strings panel, docs, close | the `Strings` panel (a table with a filter and a "missing only" switch, in-place cells, add key, Save / Revert), guides 03 and 21; the milestone closed | editor layer, docs | Sonnet |

## Gates
- Every step: `local_ci.sh --task` green. Tests pin: a load / edit / save round trip keeps comment rows and extra columns, a cell edit changes only its row, a missing key shows as missing and `set_text` adds the row, a table the cook refuses is not written.
- No change to a wide header.
- The panel is UI like its peers: it compiles and the grid model is tested; its look needs a display check later.

## Step status
| Step | Status | PR | Notes |
|---|---|---|---|
| 1 | done | `StringsGrid` in editor core (all locales in one grid); `validate_and_save_source` now also cooks interchange sources (a `.csv` with its sidecar settings) | |
| 2 | done | the Strings panel (grid, filter, missing only, add and remove key, Save cooks first, Revert), Assets double click, guides 03 and 21 | |
