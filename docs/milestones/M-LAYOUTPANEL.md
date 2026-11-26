# M-LAYOUTPANEL — one panel for the layout commands (milestone record)

**Status:** step 1 done 2026-11-26. **Owner: the "Geliştirme rutini 3" thread.** Follows M-LAYOUTTOOLS, M-SCATTER, M-RADIAL, M-GROUPXFORM and M-MEASURE: the layout commands were spread over six Edit-menu items, two modal popups and a set of MCP tools. A level author laying out a street wants one window that stays open with its numbers kept between clicks.

## Steps

| # | Step | State |
|---|---|---|
| 1 | The **Layout** panel (`layout_panel.cpp`, Window menu): live Middle / Size / Spread of the selection; Line up (align, space, flip, snap); Copies (grid, ring); Turn and resize; Randomize. Each button runs the existing headless command, one undo entry; user guide chapter 3 | done 2026-11-26 |
| 2 | Layout tools as viewport gizmos: a pivot handle and an on-canvas ring | open |
