# M-SCATTER — random variation for placed entities (milestone record)

**Status:** done 2026-11-25 (steps 1 to 3). **Owner: the "Geliştirme rutini 3" thread.** Follows M-LAYOUTTOOLS: arrays, snapping and alignment put things exactly where the author wants them, but a row of trees or crates that all share one angle and one size looks laid out by a ruler. This adds the opposite tool: a bounded, repeatable random variation for the selection.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `jitter_entities` in editor core: moves (world X and Y), turns (about +Z) and scales each entity by its own random amount within given ranges; the same seed gives the same result (own splitmix generator, not `std::` distributions); parents respected; one undo entry | done 2026-11-25: `jitter_entities.h/.cpp`, `test_jitter_entities.cpp` (ranges, seed repeat, undo, refusals, parent/child, 200-seed fuzz) |
| 2 | MCP tool `entity_jitter` (`entities`, `move_x?`, `move_y?`, `rotate_degrees?`, `scale_min?`, `scale_max?`, `seed?`) | done 2026-11-25: `tools_layout.cpp`, `test_mcp_layout.cpp` |
| 3 | `Edit > Randomize...` popup in the editor shell (ranges, seed, new-seed button) | done 2026-11-25: `editor_shell.cpp` |
