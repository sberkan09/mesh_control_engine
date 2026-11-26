# M-MEASURE — read how a group sits in the world (milestone record)

**Status:** steps 1 and 2 done 2026-11-26. **Owner: the "Geliştirme rutini 3" thread.** The layout tools change a selection; none of them tells an author or an agent how wide a fence is or where the middle of a row of lamps lies. This adds the read-only counterpart.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `group_bounds` in editor core: the world box, middle, size and diagonal of a group; read-only | done 2026-11-26: `group_bounds.h/.cpp`, `test_group_bounds.cpp` |
| 2 | MCP tool `entity_bounds` (`entities`) | done 2026-11-26: `tools_layout.cpp`, `test_mcp_layout.cpp` |
| 3 | The selection's size and middle in the viewport's status line | open |
