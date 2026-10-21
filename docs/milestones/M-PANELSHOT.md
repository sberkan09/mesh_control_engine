# M-PANELSHOT — Every editor panel drawn headless (milestone record)

**Status:** step 1 open. **Owner: the "Geliştirme rutini 3" thread** (the routine had no unowned work left; the editor and asset tools are its area). No Linear issues (Linear is unreachable from this thread and the workspace is at its free issue limit); this record is the tracker.

**Goal.** Most editor panels (`add_default_panels`, about twenty) are only ever build-checked: no test opens them, so a panel that asserts, leaves an ImGui id stack unbalanced or records a GL validation error is found by hand. Open every default panel on a headless device for a few frames, fail on any of those, and write one PNG per panel so a person or an agent can look at it.

**Not in scope:** clicking through panels, pixel-exact golden images, the viewport's scene rendering (covered by `test_editor_gl`).

## Steps

| # | Step | State |
|---|---|---|
| 1 | `test_panel_smoke` in `tests/editor`: an `EditorShell` on the headless GL device of `test_editor_gl`, `add_default_panels`, each panel opened alone for 3 frames; fails on a GL validation error, an unbalanced ImGui id or style stack, or a panel that draws nothing | open |
| 2 | Same harness writes `build/panel-shots/<panel>.png` (one per panel) and `scripts/ci` uploads the folder as an Actions artifact | open |

**Gates:** `test_panel_smoke` runs in `--task`. If it passes a panel name list it is added to the module-to-subject map once that map exists.
