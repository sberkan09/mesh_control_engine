# M-INVENTORY — Items, inventories and loot as data (milestone record)

**Status:** planned (added 2026-10-18; **owner: the routine 1 thread (M-IMPORTMEDIA's), claimed 2026-10-18**; device-free; no Linear issues: the workspace is at its free limit, this record is the tracker).

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
| 1 | `.mcitems` item definitions (id, name, stack limit, weight, tags, free numeric stats, icon and prefab by GUID): JSON source, cooked to a new `CHNK_ITEMS`, reader in `asset/formats.h`, cook validation (duplicate ids, bad GUIDs, limits), fuzz target, guide chapter stub | asset, asset/cook | Sonnet (Opus review: new chunk, untrusted-input reader) | built, Opus-reviewed (no blocker; fixed: a failed decode now leaves the catalog empty, item names must be non-empty without control bytes), in review (GH-406): `mce/asset/items_format.h` (`ItemCatalog`, codec, `item_catalog_problem`), `items_importer.cpp`, `ChunkType::ItemCatalog`, the `ItemCatalog` MCP asset kind, `test_cook_items.cpp`, `tests/fuzz/test_items.cpp`; loot tables are already in the format (step 3 adds the roller); `container.h` reaches about 400 translation units, but the capabilities wall wants a chunk type to come with its authoring path, so the enum value and the importer ship together |
| 2 | `Inventory` (slots, stacks, weight cap), `Equipment` (named slots, tags that fit), `ItemPickup3D`/`ItemPickup2D`, events `ItemAdded` / `ItemRemoved` / `ItemEquipped` / `ItemUsed`; the add/remove/move/split/merge rules as pure functions with a test table | kit | Opus for the rules (determinism, overflow), Sonnet for the components | built (GH-406), Opus review pending, tests in `tests/kit/test_inventory.cpp` (12 cases); `ItemPickup2D` left out (no 2D trigger yet), see open items |
| 3 | Loot tables in `.mcitems` (weighted entries, nested tables, guaranteed drops, level ranges), a `LootDrop` component and `Container` (chest) that rolls on the world's seeded RNG; a golden-hash test that two runs drop the same items | kit, asset/cook | Opus (determinism) | planned |
| 4 | Luau surface (`inventory.add/remove/count/has`, `equip`, `use`, `drop`), `UiFlow`/widget binding (a `ItemGrid` bound to an entity's inventory), save/load of inventories through the existing save path with a version | kit, script, ui | Sonnet | planned |
| 5 | MCP/guide: `asset_create`/`asset_schema` for `.mcitems`, capability table row, user-guide chapter, `projects` sample (`samples/12_loot`: pick up, equip, drop, a chest) and its gate, the close | editor/mcp, docs, samples | Sonnet | planned |

## Decisions

- One source file holds items and loot tables (`.mcitems`), like `.mcsound` holds a sound set: one GUID per catalog, an
  item is referred to by its string id inside the catalog and by `(catalog GUID, id)` from components.
- Stats are free-form named numbers (`damage`, `heal`, `armor`), not a fixed struct: the engine does not know what a game
  means by them, scripts and the combat kit read them by name.
- Quantities are `u32` with explicit overflow rules (an add that would pass the cap leaves the remainder and reports it),
  never wrapping; weights are fixed-point integers (thousandths) so the sum does not depend on float order.

## Open items

- `ItemPickup2D` is not built: the kit has no 2D trigger event to hang it on; add it with the first 2D game that needs it.
- Crafting and recipes, shops over in-game currency, replication of inventories and drag-and-drop inventory UI chrome are
  follow-ups once step 5 has shown the shape.
- Known cost: duplicate-id checks in the chunk reader are quadratic in the item count (4096 items, about 17M short compares on a hostile pack); fine for a load-time check, a sorted check if it ever shows up in a profile.
