# M-SCENEFIND — Find entities in a scene (milestone record)

**Status:** step 1 in progress. **Owner: the "Geliştirme rutini 3" thread** (editor and asset tools). Defined 2026-10-25 on the coordinator's standing request to keep going when the Backlog is empty, in an area that needs no real account or device (editor, scenes and serialization, tools; outside gameplay and the traffic work). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Goal.** A reviewer or an agent holding a scene wants to ask it a question without opening it: which entities are called "torch", which carry a `Sprite`, which are prefab instances. `find_entities` answers from a scene document without an ECS, so the editor, the command line and agents get the same hits, each with its hierarchy path (`Room/Wall/Torch`).

**Not in scope:** matching on field values (`scene_get_entity` shows them), regular expressions, editing the hits.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `find_entities` and `format_hits` in editor core (`mce/editor/scene_find.h`) with a test (`test_scene_find.cpp`) | open (PR) |
| 2 | `mce scene find <scene> [--name <part>] [--component <Type>] [--prefab]` (the `mce_scene_find` tool under `mce_cli`), user guide row, ctest cases over the fixture scenes | open |
| 3 | MCP `scene_find` tool (`path?`, `name?`, `component?`, `prefab?`) with a `test_mcp_team` case | open |

**Gates:** step 1 `test_editor`.
