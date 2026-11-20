# M-STACKLINT — finding copies stacked on one spot (milestone record)

**Status:** done 2026-11-20 (steps 1 and 2). **Owner: the "Geliştirme rutini 3" thread.** Follows M-LAYOUTTOOLS: an array or a duplicate with a step of zero leaves several identical entities on one spot, which look like one and double the cost of drawing and simulating it. Nothing in the editor flagged it.

It is not a `scene_lint` rule: lint's contract is that `fix_scene_lint` leaves a scene clean, and deleting entities is not a repair it can make on its own, and a scene may mean its copies (two particle emitters). So the finding is read-only and the caller decides.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `find_stacked` / `format_stacked` in editor core: groups of entities with the same prefab and the same components and values (names do not count), under the same parent; entities without a LocalTransform are skipped | done 2026-11-20: `stacked_entities.h/.cpp`, `test_stacked_entities.cpp` (named cases, array with no step, 200-seed fuzz against a plain model over the document's values) |
| 2 | MCP tool `scene_find_stacked` (a saved scene or the open one): `groups` with `keep` and `copies`, and a text report | done 2026-11-20: `tools_scene_stacked.cpp`, `test_mcp_layout.cpp` |
