# M-INVENTORY — Items, inventories and loot as data (milestone record)

**Status:** done 2026-10-20 (steps 1-5 merged: #591 step 1, #607, #634, #636, #639 and #642) (added 2026-10-18; **owner: the routine 1 thread (M-IMPORTMEDIA's), claimed 2026-10-18**; device-free; no Linear issues: the workspace is at its free limit, this record is the tracker).

**Why.** The kit has combat, wanted levels, missions, vehicles and weather, but nothing that holds things: no item
definitions, no inventory, no equipment, no pickups, no loot. Every genre the engine targets needs them (the open-world
sample's weapons and cash, an RPG's gear, a roguelike's drops, a survival game's crafting), and today a game would hand-roll
integers in `ScriptVars`, which the editor cannot show, the cook cannot validate and a save cannot version. A grep of the
milestones and capabilities for "inventory", "item" and "loot" finds nothing.

**Boundaries.** Data and kit only: a new cooked asset type, kit components and systems, a Luau surface, docs and one
demo. No UI look (the Text/Grid widgets and `UiFlow` already exist; this binds them), no crafting graph or shop (the
`.mcstore` catalog is real-money products and stays separate), no networking of inventories (a replicated component can
follow M-NET3). Stay off the render, script VM internals and perf files. Deterministic: every roll draws from the
world's seeded stream, never wall-clock time.

| # | Step | Modules | Model | State |
|---|---|---|---|---|
| 1 | `.mcitems` item definitions (id, name, stack limit, weight, tags, free numeric stats, icon and prefab by GUID): JSON source, cooked to a new `CHNK_ITEMS`, reader in `asset/formats.h`, cook validation (duplicate ids, bad GUIDs, limits), fuzz target, guide chapter stub | asset, asset/cook | Sonnet (Opus review: new chunk, untrusted-input reader) | merged (#591, GH-406), Opus-reviewed (no blocker; fixed: a failed decode now leaves the catalog empty, item names must be non-empty without control bytes): `mce/asset/items_format.h` (`ItemCatalog`, codec, `item_catalog_problem`), `items_importer.cpp`, `ChunkType::ItemCatalog`, the `ItemCatalog` MCP asset kind, `test_cook_items.cpp`, `tests/fuzz/test_items.cpp`; loot tables are already in the format (step 3 adds the roller); `container.h` reaches about 400 translation units, but the capabilities wall wants a chunk type to come with its authoring path, so the enum value and the importer ship together |
| 2 | `Inventory` (slots, stacks, weight cap), `Equipment` (named slots, tags that fit), `ItemPickup3D`/`ItemPickup2D`, events `ItemAdded` / `ItemRemoved` / `ItemEquipped` / `ItemUsed`; the add/remove/move/split/merge rules as pure functions with a test table | kit | Opus for the rules (determinism, overflow), Sonnet for the components | merged (#607, GH-406), Opus-reviewed, tests in `tests/kit/test_inventory.cpp` (15 cases); `ItemPickup2D` left out (no 2D trigger yet), see open items |
| 3 | Loot tables in `.mcitems` (weighted entries, nested tables, guaranteed drops, level ranges), a `LootDrop` component and `Container` (chest) that rolls on the world's seeded RNG; a golden-hash test that two runs drop the same items | kit, asset/cook | Opus (determinism) | merged (#634), Opus-reviewed (fixed: chest catalog mismatch null deref, work budget against hostile table chains, lost-loot logging, self-open); `mce/kit/loot.h` (`roll_loot`, `LootDrop`, `Container`, `OpenContainer`/`ContainerOpened`), `tests/kit/test_loot.cpp` (10 cases, pinned golden hash); the table format itself shipped in step 1 |
| 4 | Luau surface (`inventory.add/remove/count/has`, `equip`, `use`, `drop`), `UiFlow`/widget binding (a `ItemGrid` bound to an entity's inventory), save/load of inventories through the existing save path with a version | kit, script, ui | Sonnet | 4a merged (#636, GH-406): the Luau `inventory` global (`api_inventory.cpp`, `tests/script/test_script_inventory.cpp`, 3 cases); 4b merged (#639): name-convention Text widgets (`slot_N`, `equip_*`, `inventory_weight`) fed by `player/item_grid_feed.cpp`, `tests/player/test_item_grid_feed.cpp`; instead of a new `ItemGrid` widget kind (a `.mcui` chunk bump for a Text the document can already lay out); the save version is the existing whole-save version, documented in kit.md (no separate per-component version exists) |
| 5 | MCP/guide: `asset_create`/`asset_schema` for `.mcitems`, capability table row, user-guide chapter, `projects` sample (`samples/12_loot`: pick up, equip, drop, a chest) and its gate, the close | editor/mcp, docs, samples | Sonnet | merged (#642): `samples/12_loot` (scene, `.mcitems`, script, `loot_test.cmake` gate: exact `inv:` lines and the same state hash twice), typed `inventory` definitions for the script checker (4a had missed them), `ItemCatalog` references in scenes and the inspector (a gap from step 1: scenes could not name a catalog), user-guide text in 05-assets and 18-samples; `asset_create`/`asset_schema` for `.mcitems` shipped in step 1 |

## Decisions

- One source file holds items and loot tables (`.mcitems`), like `.mcsound` holds a sound set: one GUID per catalog, an
  item is referred to by its string id inside the catalog and by `(catalog GUID, id)` from components.
- Stats are free-form named numbers (`damage`, `heal`, `armor`), not a fixed struct: the engine does not know what a game
  means by them, scripts and the combat kit read them by name.
- Quantities are `u32` with explicit overflow rules (an add that would pass the cap leaves the remainder and reports it),
  never wrapping; weights are fixed-point integers (thousandths) so the sum does not depend on float order.

## Open items

- Crafting followed as [M-CRAFT](M-CRAFT.md).
- `ItemPickup2D` is not built: the kit has no 2D trigger event to hang it on; add it with the first 2D game that needs it.
- Crafting and recipes, shops over in-game currency, replication of inventories and drag-and-drop inventory UI chrome are
  follow-ups once step 5 has shown the shape.
- Known cost: duplicate-id checks in the chunk reader are quadratic in the item count (4096 items, about 17M short compares on a hostile pack); fine for a load-time check, a sorted check if it ever shows up in a profile.
