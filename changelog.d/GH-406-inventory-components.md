- Inventory components (M-INVENTORY step 2, GH-406): `Inventory` (24 slots, stack sizes, optional weight limit), `Equipment`
  (named body slots, fitted by `equip.<slot>` item tags) and `ItemPickup3D`, with add / remove / move / merge / swap / split / equip
  rules in whole numbers and events `GiveItem`, `TakeItem`, `MoveItem`, `SplitItem`, `EquipItem`, `UnequipItem`, `UseItem` in and
  `ItemAdded`, `ItemRemoved`, `ItemEquipped`, `ItemUnequipped`, `ItemUsed` out. `docs/engine/kit.md`. Loot tables and the Luau surface follow.
