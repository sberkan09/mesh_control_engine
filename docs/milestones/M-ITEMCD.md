# M-ITEMCD — Consumable cooldowns (milestone record)

**Status:** done in one step (added 2026-10-21; owner: the M-INVENTORY thread; device-free; no Linear issues: the free limit).

**Why.** M-ITEMFX left "cooldowns on consumables" open: potion spam is limited only by the stack size. M-STATUS made timed state a saved,
deterministic component, so a cooldown is one more kind of status instead of a new component.

**What.** `StatusKind::Cooldown` (no effect of its own). A consumable's `cooldown` stat starts `cd:<item>` on use; a use while it runs is refused and
not consumed. Luau reads it with `status.has`. Test: `tests/kit/test_inventory.cpp` (cooldown case).

## Open items

- Two uses of one item in the same tick both pass (the status starts a tick later).
- A shared cooldown group across items (all potions) and a HUD readout of the time left.
