# M-TABSFUZZ — fuzzing scene tabs (milestone record)

**Status:** done 2026-11-20 (one step, no defect found). **Owner: the "Geliştirme rutini 3" thread.** Follow-up of M-SCENETABS, defined on
the coordinator's instruction to keep opening editor and tooling work. No Linear issues. Tests only.

## Steps

| # | Step | State |
|---|---|---|
| 1 | Fuzz 60 random sequences of open, switch, edit, save and close over three scenes: the tab list never repeats a path, the active tab is the session's scene, the world stays valid, and a scene left by a switch comes back with the same entity count while its tab stays | done 2026-11-20: `tests/editor/test_scene_tabs.cpp`; no defect (a dirty untitled scene refusing to open is the documented behaviour and is asserted) |
