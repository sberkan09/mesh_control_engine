# M-GAMEPLAYFIX — Hardening the item and status kit (milestone record)

**Status:** steps 1 and 2 built (added 2026-10-22; owner: the M-INVENTORY thread; device-free; no Linear issues: the free limit).
Source: the open items the M-STATUS, M-ITEMCD and M-WEAPONSTAT records left.

| # | Item | State |
|---|---|---|
| 1a | A poison pulse under way when the effect runs out is dealt in proportion (`status.cpp`); test in `test_status.cpp` | built |
| 1b | Two `UseItem` events for one cooldown item in the same tick pass only once (`inventory.cpp`); test in `test_inventory.cpp` | built |
| 2 | A `Weapon3D` on a separate weapon entity (owner = the wielder) gets the wielder's `damage` bonus | built (step 2): the remembered change moved to `Weapon3D::bonus_applied` (size 160 → 168); the wielder is the owner, else the weapon's entity; `Equipment::damage_applied` is gone |
| 3 | A cooldown item used by an entity without `StatusEffects` | not built, on purpose: the fix would add a component nobody authored (~456 bytes of save data per entity, a different hash) and a special case for end-of-phase command timing; the workaround is one line, `StatusEffects` in the prefab of anyone who drinks cooldown potions (documented) |
| 4 | A `Health3D` replaced at run time loses the Armor/MaxHealth bonuses | built (step 2): `Health3D::bonus_mark` (bit 0 equipment, bit 1 status, in the old padding, size stays 64); a bonus remembered while the bit is clear means the component was replaced, so it is dropped and applied in full again. Not noticed: a copy changed and written back with the old mark |
