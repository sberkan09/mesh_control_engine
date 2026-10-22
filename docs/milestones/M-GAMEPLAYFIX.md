# M-GAMEPLAYFIX — Hardening the item and status kit (milestone record)

**Status:** step 1 built (added 2026-10-22; owner: the M-INVENTORY thread; device-free; no Linear issues: the free limit).
Source: the open items the M-STATUS, M-ITEMCD and M-WEAPONSTAT records left.

| # | Item | State |
|---|---|---|
| 1a | A poison pulse under way when the effect runs out is dealt in proportion (`status.cpp`); test in `test_status.cpp` | built |
| 1b | Two `UseItem` events for one cooldown item in the same tick pass only once (`inventory.cpp`); test in `test_inventory.cpp` | built |
| 2 | A `Weapon3D` on a separate weapon entity (owner = the wielder) gets the wielder's `damage` bonus | open: needs a per-weapon remembered change (`Weapon3D` is size-pinned) |
| 3 | A cooldown item used by an entity without `StatusEffects` | open: the kit does not add components in a system; the game adds `StatusEffects` |
| 4 | A `Health3D` replaced at run time loses the Armor/MaxHealth bonuses | open: needs a respawn hook |
