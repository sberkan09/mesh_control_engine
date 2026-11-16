# M-ITEMFX — Item stats that do something (milestone record)

**Status:** done (steps 1 to 3 built) (added 2026-10-20; **owner: the M-INVENTORY thread (routine 1), claimed 2026-10-20**; device-free; no Linear issues: the workspace is at its free limit, this record is the tracker).

**Why.** M-INVENTORY and M-CRAFT made items, inventories, equipment, loot and recipes real, but an item's `stats` (`"heal": 25`,
`"armor": 0.2`) are only data: `UseItem` on a potion removes the potion and sends `ItemUsed`, and nothing heals anyone;
an equipped helmet changes nothing about the wearer's `Health3D`. Every game on the kit rewrites the same two Luau handlers.
This milestone connects the two ends the kit already has: the item catalog (stats, tags) and combat (`Health3D`).

**Boundaries.** Kit only, on `.mcitems` stats the catalog already carries, no new asset format. Two behaviours: a consumable's
`heal` stat heals the user when used, and an equipped item's `armor` and `max_health` stats add to the wearer's `Health3D`
while it is worn. No timed buffs or poison (a status-effect system is its own milestone, once a game asks), no new damage types,
no stat-driven weapon damage (a `Weapon3D` keeps its authored damage), no UI beyond the existing text feeds.

## Steps

| # | Step | Modules | Model | State |
|---|---|---|---|---|
| 1 | `UseItem` of a consumable with a `heal` stat adds it to the user's `Health3D.current` (clamped to `max`, a dead entity is not healed) and sends `ItemHealed{owner, item, amount}` (amount actually gained); a user who has a `Health3D` that is full or dead refuses the drink (nothing consumed, `ItemUsed.consumed` is 0); a user without a `Health3D` consumes it as before, so existing games keep working | kit | Sonnet; Opus reviews the use-refusal rule together with step 2 | built (this PR): `inventory.cpp` / `inventory.h` (`ItemHealed`), 1 test case in `test_inventory.cpp` |
| 2 | The bonus (`Equipment.armor_applied` / `max_applied`, engine-written, not saved): equipped items' `armor` and `max_health` stats are added to `Health3D.armor` (clamped 0..1) and `.max`; applied as a delta against what was last applied so the authored base is never overwritten and unequipping restores it; re-evaluated in slot order, so the same equipment gives the same numbers | kit | Opus (numeric, determinism, save/load of a delta) | built (this PR): the delta lives on `Equipment` (`armor_applied`, `max_applied`, not saved) and is applied in `kit_inventory` before the use loop and at its end; Opus-reviewed with step 1 (see the notes) |
| 3 | `samples/12_loot` hero gets a wounded `Health3D` (40/100), a helm (`armor` 0.2, `max_health` 20) and the potion heals it; pinned in `loot_test.cmake` (`health 65/120 armor 0.2`); docs, close | samples, docs | Sonnet | built (this PR) |

## Decisions

- The stat names are conventions, not schema: `heal`, `armor` and `max_health` are read from `ItemDef` stats by name, like `equip.*` tags;
  a project may ignore them.
- Refusing a useless heal (only when the user has a `Health3D`) keeps potions from vanishing at full health; the game can still force a consume by `TakeItem`.
- `Equipment` stores the applied delta, not the total, so a designer editing `Health3D.armor` in the scene, or a save made
  before the item stats changed, never ends up with a permanently wrong value.

## Open items

- Status effects (poison, regeneration buffs, durations) and stat-driven weapon damage.
- Cooldowns on consumables.

## Review notes (steps 1 and 2)

- Opus review: one blocker, fixed. The applied amounts were `Save` fields while `Health3D` is not in a save game, so a load lost the bonus and
  unequipping then ate the base; they are no longer saved. Also fixed: `apply_equipment_stats` runs before the `UseItem` loop (a drink
  in the tick of an equip is judged against the new max), non-finite stat sums are ignored, the clamp test was vacuous and now isn't.
  Left (documented in `inventory.h`): a `Health3D` overwritten at run time (respawn) loses the bonus until the equipment changes.
