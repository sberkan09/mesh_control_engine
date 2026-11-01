# M-LOOTVIS — loot in the open-world sample

Status: step 1 done (2026-10-30). Closes the loot/inventory half of A11 in [M-OPENWORLD-GAPS](M-OPENWORLD-GAPS.md).

## Design

`LootDrop` grows `mesh`, `tint`, `mesh_scale` and `lift` so a drop can be seen (before, a pickup was an invisible trigger). New component
`ItemHotkey3D` (on an entity with an `Inventory`): on the `UseItem` action `kit_item_hotkey` pushes a `UseItem` for the first consumable slot.
`samples/09_openworld`: `items/loot.mcitems` (bandage +20, medkit +60, table `pedestrian` with 0 to 1 rolls), a `LootDrop` on the pedestrian
prefab (a small green box), an `Inventory` and `ItemHotkey3D` on the player, a `UseItem` action (H, right shoulder), and `slot_1` to `slot_3`
Text widgets in `hud.mcui` that the player's item grid feed fills.

## Limits

One hotkey for the first consumable (no slot choice, no drop or equip keys); the HUD lists three slots; killed police and drivers drop nothing;
no sell value (the scrapyard takes cars only); items are not shown on the minimap.
