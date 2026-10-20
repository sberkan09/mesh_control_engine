# M-SEQEDIT — Editing action sequences in the editor (milestone record)

**Status:** done 2026-10-20 (steps 1–2). **Owner: the "Geliştirme rutini 3" thread** (a gap taken from the assessments at the coordinator's request: no unowned milestone was left). No Linear issues (Linear is unreachable and the workspace is at its free issue limit); this record is the tracker.
Source: `assessments/motor-erisim-bosluklari.md`, section 11 (Editor): "Editörü olmayanlar: ... sekans ...". M-ASSETLOOK gave materials and particle presets a form panel; a `.mcseq` action list (wait, move, play a sound, spawn a prefab, emit a signal, ...) is still hand-written JSON for a human, and a wrong step only shows up at cook.

**Goal.** A `Sequence` panel opened by a double click on a sequence in Assets: the steps as a list that can be added to, removed, reordered and edited field by field, the `loop` flag and the name, saved through the same cook-first write as the other forms (`validate_and_save_source`), so a document the cook refuses (a negative wait, a looping sequence that never waits, a missing sound) is not written and the panel says why.

**Not in scope:** running or previewing the sequence in the viewport (the Play mode already runs `SequencePlayer`), a timeline view of the steps (`.mctimeline` is the timeline editor's), editing the code hooks a `call` step names.

## Steps
| Step | What | Modules | Model |
|---|---|---|---|
| 1. Sequence form | `SequenceForm` / `SequenceStepForm` in `mce/editor/asset_form.h`: text to form and back (the steps array is rewritten from the form, extras only when they differ from the defaults; every other member stays in place), `load/save_sequence_form`; headless tests | editor core | Sonnet |
| 2. Sequence panel, docs, close | the `Sequence` panel (step list, add / remove / move, per-kind fields, sound and prefab pickers), the double click in Assets, guides 03 and 13; the milestone closed | editor layer, docs | Sonnet |

## Gates
- Every step: `local_ci.sh --task` green. Tests pin: a round trip keeps the other members and writes no default extras, a save the cook refuses writes nothing.
- No change to a wide header; `editor_shell.h` (19 TUs) gets one request member in step 2.
- The panel is UI like its peers: it compiles and the form is tested; its look needs a display check later.

## Step status
| Step | Status | PR | Notes |
|---|---|---|---|
| 1 | done (this PR) | | `SequenceForm` / `SequenceStepForm` with `describe`, `sequence_form_from_text/to_text`, `load/save_sequence_form`: the steps array is rewritten from the form (one action key, extras only when not default: `speed` 1, `tolerance` 0.01, `gain_db` 0, `relative` false, Spawn `at`), `name` and `loop` too, every other member stays; 2 tests |
| 2 | done (this PR) | | `Sequence` panel (`sequence_panel.cpp`): step list with Add / Remove / Up / Down, per-kind fields, sound and prefab pickers with drag and drop, name, loop, Save / Revert; `EditorShell::open_sequence` from the Assets double click. Build-checked only here (no display) |
