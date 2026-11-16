# M-WEAPONSTAT — Stat-driven weapon damage (milestone record)

**Status:** done in one step (added 2026-10-22; owner: the M-INVENTORY thread; device-free; no Linear issues: the free limit).

**Why.** M-ITEMFX left "stat-driven weapon damage" open: `armor` and `max_health` stats change `Health3D`, but a sword's `damage` stat did nothing.

**What.** `apply_equipment_stats` also sums the worn items' `damage` stats and adds them to a `Weapon3D` on the same entity as the `Equipment`, as a remembered
change (first on `Equipment::damage_applied`; since M-GAMEPLAYFIX 2 on `Weapon3D::bonus_applied`, so a weapon on its own entity works too) like armor: applied once, given back on unequip, clamped at 0, kept while the catalog is not loaded. Test:
`tests/kit/test_inventory.cpp`.

## Open items

- (done in M-GAMEPLAYFIX 2: a `Weapon3D` on its own entity gets the wielder's bonus.)
- Per-slot weapon swaps that replace `Weapon3D` fields (kind, rate) from the item.
