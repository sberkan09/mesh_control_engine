# M-GROUPXFORM — turn and resize a selection as one body (milestone record)

**Status:** steps 1 and 2 done 2026-11-25. **Owner: the "Geliştirme rutini 3" thread.** Follows M-RADIAL: the layout tools place, copy and randomize, but turning a placed group (a fence line, a room of props) meant turning every entity in place. This moves them round a common pivot.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `transform_group` in editor core: rotate about +Z and scale about a pivot (the group's middle by default), entities turn and scale too; parents respected; one undo entry | done 2026-11-25: `transform_group.h/.cpp`, `test_transform_group.cpp` |
| 2 | MCP tool `entity_transform_group` (`entities`, `degrees?`, `scale?`, `pivot_x?`, `pivot_y?`) | done 2026-11-25: `tools_layout.cpp`, `test_mcp_layout.cpp` |
| 3 | `Edit > Turn and resize...` popup in the editor shell | open |
