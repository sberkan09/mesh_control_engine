# M-PANELSHOT — Every editor panel drawn headless (milestone record)

**Status:** step 1 done, step 2 in progress. **Owner: the "Geliştirme rutini 3" thread** (the routine had no unowned work left; the editor and asset tools are its area). No Linear issues (Linear is unreachable from this thread and the workspace is at its free issue limit); this record is the tracker.

**Goal.** Most editor panels (`add_default_panels`, about twenty) are only ever build-checked: no test opens them, so a panel that asserts or records a GL validation error is found by hand. Bring every default panel to the front on the real GL device (Xvfb + Mesa in CI) for a few frames, fail on a blank shot or a GL validation error, and write one image per panel so a person or an agent can look at it.

**Not in scope:** clicking through panels, pixel-exact golden images, the viewport's scene rendering (covered by `test_editor_gl`).

## Steps

| # | Step | State |
|---|---|---|
| 1 | `mce_editor -editor.panel_shots <dir>`: brings each panel's tab to the front for 6 frames, reads the backbuffer back, writes `<dir>/<panel>.ppm`; exit 3 on a shot with fewer than 8 colours (the shell did not draw) or a GL validation error. `editor_panel_shots` runs it (label `gpu`, GL debug layer on) | done |
| 2 | The integration run's g1 group uploads `build/panel-shots` as the `panel-shots` artifact (14 days), so a person or an agent can look at every panel; `ci.yml` prints how many shots the PR gate wrote | in progress |

**Gates:** `editor_panel_shots` (step 1, with the other `gpu` tests); step 2 is the workflow change. `editor_panel_shots` carries the `gpu` label, so `--task` runs it unless every changed file is GPU-irrelevant (`local_ci.sh` `test_scope`): a docs-only or simulation-only change skips it or runs only the GPU smoke subset.
