# M-RADIAL — copy a group around a centre point (milestone record)

**Status:** steps 1 and 2 done 2026-11-25. **Owner: the "Geliştirme rutini 3" thread.** Follows M-LAYOUTTOOLS and M-SCATTER: `array_duplicate` lays copies on a grid, but a ring of pillars, the spokes of a wheel or a fan of torches is the same group turned about a pivot.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `radial_duplicate` in editor core: `count` groups in all (originals included) about a centre, a closed ring (360 degrees) or an arc with both ends filled; copies optionally turn with their position; parents keep their copied children; at most 4096 copies; one undo entry | done 2026-11-25: `radial_duplicate.h/.cpp`, `test_radial_duplicate.cpp` |
| 2 | MCP tool `entity_radial_duplicate` (`entities`, `count`, `center_x?`, `center_y?`, `total_degrees?`, `turn?`) | done 2026-11-25: `tools_layout.cpp`, `test_mcp_layout.cpp` |
| 3 | `Edit > Radial Duplicate...` popup in the editor shell | open |
