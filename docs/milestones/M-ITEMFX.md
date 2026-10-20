# M-ITEMFX — Item stats that do something (milestone record)

**Status:** planned (added 2026-10-20; **owner: the M-INVENTORY thread (routine 1), claimed 2026-10-20**; device-free; no Linear issues: the workspace is at its free limit, this record is the tracker).

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
| 1 | `UseItem` of a consumable with a `heal` stat adds it to the user's `Health3D.current` (clamped to `max`, a dead entity is not healed) and sends `ItemHealed{owner, item, amount}` (amount actually gained); a consumable that cannot heal (full health, or no `Health3D`) is still consumed only when it healed or has no `heal` stat: a full-health drink is refused and `ItemUsed.consumed` is 0 | kit | Sonnet; Opus reviews the use-refusal rule | planned |
| 2 | `EquipmentBonus` (engine-written, saved): equipped items' `armor` and `max_health` stats are added to `Health3D.armor` (clamped 0..1) and `.max`; applied as a delta against what was last applied so the authored base is never overwritten and unequipping restores it; re-evaluated in slot order, so the same equipment gives the same numbers | kit | Opus (numeric, determinism, save/load of a delta) | planned |
| 3 | `samples/12_loot` hero gets a `Health3D`, the potion heals after the goblin's hit (a scripted `Damage3D`-free setup), the sword stays; pinned log lines; docs (`kit.md`, `12-ui-and-console.md` if the HUD gets a health line), close | samples, docs | Sonnet | planned |

## Decisions

- The stat names are conventions, not schema: `heal`, `armor` and `max_health` are read from `ItemDef` stats by name, like `equip.*` tags;
  a project may ignore them.
- Refusing a useless heal keeps potions from vanishing at full health; the game can still force a consume by `TakeItem`.
- `EquipmentBonus` stores the applied delta, not the total, so a designer editing `Health3D.armor` in the scene, or a save made
  before the item stats changed, never ends up with a permanently wrong value.

## Open items

- Status effects (poison, regeneration buffs, durations) and stat-driven weapon damage.
- Cooldowns on consumables.
