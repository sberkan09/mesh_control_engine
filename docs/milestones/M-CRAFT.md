# M-CRAFT — Crafting recipes as data (milestone record)

**Status:** done (steps 1 to 3 built) (added 2026-10-20; **owner: the M-INVENTORY thread (routine 1), claimed 2026-10-20**; device-free; no Linear issues: the workspace is at its free limit, this record is the tracker).

**Why.** M-INVENTORY gave the kit items, inventories, equipment, pickups and loot, and listed crafting as its first follow-up
("crafting and recipes ... are follow-ups once step 5 has shown the shape"; `samples/12_loot` has now shown it). Survival,
RPG and roguelike games all turn some items into others, and today that is a hand-written Luau loop over
`inventory.count/remove/add` that the editor cannot show, the cook cannot validate and no test pins.

**Boundaries.** Data and kit only, in the same `.mcitems` catalog (one source file, one GUID, as M-INVENTORY decided): recipes
beside items and loot tables, a craft rule, a station component, a Luau call, docs and a sample extension. No crafting UI
look (the `slot_N` Text convention and `UiFlow` already exist), no timed multi-stage queues (a `seconds` field is data a
station honours, nothing more), no shops (in-game currency trading is a separate follow-up), no replication. Deterministic:
integer rules only, no RNG unless a recipe has a chance field (then it draws from the seeded stream like loot).

| # | Step | Modules | Model | State |
|---|---|---|---|---|
| 1 | `recipes` in `.mcitems`: id, name, inputs (item, qty), outputs (item, qty), required station tag, craft time; cook validation (unknown items, empty sides, quantities, duplicate ids, limits), a chunk minor bump (the recipes section is appended only when a catalog has recipes, so a catalog without them stays byte-identical to v1.0), `item_catalog_problem` checks, fuzz seed, `asset_schema` text | asset, asset/cook | Opus (untrusted-input reader, format bump) | built (GH-406), Opus-reviewed (no blocker, see the notes below): `items_format.h/.cpp` (`Recipe`, `RecipeIngredient`, `add_recipe`, codec), `items_importer.cpp` (importer version 2), tests in `test_cook_items.cpp` (3 cases) and the fuzz corpus |
| 2 | `craft_check` / `craft_apply` rules (pure functions of an Inventory: all inputs present, room for the outputs, nothing changes on failure), events `CraftItem` in and `ItemCrafted` / `CraftFailed` (reason) out, `CraftingStation` component (station tag, optional queue of one with `seconds`) | kit | Opus for the rules (atomicity, overflow, weight cap), Sonnet for the component | built (GH-406, #647), review below: `kit/craft.h`, `src/craft.cpp` (`craft_check`, `craft_apply`, `CraftingStation`, `CraftItem` / `ItemCrafted` / `CraftFailed`, system `kit_craft`), `tests/kit/test_craft.cpp` (9 cases) |
| 3 | Luau `inventory.craft(e, recipe, station?)` and `inventory.can_craft`, typed definitions with a check test, `samples/12_loot` gets a recipe and a gate line, user-guide text, close | script, samples, docs | Sonnet | built (GH-406, this PR): `api_inventory.cpp` (`can_craft`, `craft`), typed in `definitions.cpp` with a check test, 2 cases in `test_script_inventory.cpp`, `12_loot` (a tonic by hand, a pack at a workbench, pinned in `loot_test.cmake`) |

## Decisions

- Recipes live in `.mcitems` next to items and tables, so one GUID names a game's whole item world and `item_catalog_problem`
  checks every reference in one place.
- A craft is atomic: it either removes every input and adds every output or changes nothing. Room for the outputs is
  checked against stacks, slots and the weight cap *after* the inputs are removed (an input slot frees space).
- A station is a tag, not an entity reference: a recipe may require `station.forge`; a `CraftingStation` entity with that tag
  (or no requirement) allows it. An inventory crafts at hand when the recipe has no station.

## Open items

- Chance outputs and recipe unlocks (a recipe known/unknown per player) are not in scope; add them with the first game that needs one.
- Disassembly is a recipe whose input is the crafted item; no special support.

## Review notes (step 1)

- Opus review: no blocker. Fixed: empty recipe names are refused by `item_catalog_problem`. Left for later: the decoder ignores the
  chunk minor (it reads a trailing section as recipes; the container's size and CRC catch truncation, so only an unverified dev
  mount could load a catalog cut after its tables as recipe-less); recipe errors found by `item_catalog_problem` point at the
  document root, not the recipe's JSON node; a cook warning when an output quantity is at least the input's (item duplication
  loops) is a step 2 candidate. Recipes whose output is also an input are allowed on purpose (a tool used and returned).

## Review notes (step 2)

- Opus review: no blocker (atomic on a trial copy, overflow-safe, queue arithmetic bounded). Fixed: the crafter is saved with the station
  (a craft in progress survives a save), the station tag is checked again at completion, a recipe without a station is always by hand.
  Left: destroying a station drops its queued craft without an event; the step is rounded to whole microseconds so a one-hour craft
  ends a few ticks early.

