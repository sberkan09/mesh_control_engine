# M-ITEMCD — Consumable cooldowns (milestone record)

**Status:** done in one step (added 2026-10-21; owner: the M-INVENTORY thread; device-free; no Linear issues: the free limit).

**Why.** M-ITEMFX left "cooldowns on consumables" open: potion spam is limited only by the stack size. M-STATUS made timed state a saved,
deterministic component, so a cooldown is one more kind of status instead of a new component.

**What.** `StatusKind::Cooldown` (no effect of its own). A consumable's `cooldown` stat starts `cd:<item>` on use; a use while it runs is refused and
not consumed. Luau reads it with `status.has`. Test: `tests/kit/test_inventory.cpp` (cooldown case).

**Step 2 (cooldown groups).** An item with a `cooldown_group` stat (1 to 999999) uses the effect `cdg:<group>` instead of `cd:<item>`, so every item of the group shares one cooldown
(all potions). Test: `test_inventory.cpp` (a second member is refused and not wasted, an item without a group is unaffected, the group frees up).

## Open items

- Two uses of one item in the same tick: fixed in M-GAMEPLAYFIX 1.
- A HUD readout of the time left: the `status_1..8` feed already prints the `cd:<item>` / `cdg:<group>` effect with its tenths of seconds.
- A cooldown for a user without `StatusEffects` stays the game's (see M-GAMEPLAYFIX).
