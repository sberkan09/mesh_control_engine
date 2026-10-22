# M-STATUS — Timed status effects (milestone record)

**Status:** done (steps 1 #684, 2 #688, 3 this PR) (added 2026-10-21; **owner: the M-INVENTORY thread (routine 1), claimed 2026-10-21**; device-free; no Linear issues: the workspace is at its free limit, this record is the tracker).

**Why.** M-ITEMFX made item stats heal and armour, but only at the instant of use or while worn. Games on the kit still hand-write
"regenerate 5 hp/s for 10 s", poison, a temporary armor boost or a shield, each as a Luau timer that the editor cannot show, that
saves nothing and that two scripts can double-apply. This is the "status effects" item M-ITEMFX listed as open.

**Boundaries.** Kit and Luau only, on `Health3D`: effects that move health or armor over a duration. No new damage types, no
resistances, no visual effects (a game reacts to the events), no stat system beyond `Health3D.armor` and `.max`, no replication.
Deterministic: effect time is integer microseconds like `CraftingStation`, per-tick amounts are computed from the fixed step, no RNG.

## Steps

| # | Step | Modules | Model | State |
|---|---|---|---|---|
| 1 | `StatusEffects` component (up to 8 slots: id, kind, amount, remaining time, stacks) with kinds `Regen` (hp/s), `Poison` (hp/s, can kill through the normal damage path so `Died3D` fires and `last_attacker` is the source), `Armor` and `MaxHealth` (added while active, as a remembered delta like `Equipment`); events `ApplyStatus` / `ClearStatus` in, `StatusApplied` / `StatusEnded` out; re-applying the same id refreshes the time (and never stacks unless `stackable`); system after `kit_craft` | kit | Opus (numeric, determinism, damage path, delta bookkeeping) | built (this PR): `kit/status.h`, `src/status.cpp`, `tests/kit/test_status.cpp` (8 cases); Opus-reviewed, see the notes |
| 2 | Items apply effects: a consumable's `regen` + `regen_seconds` (and `armor_boost`, `boost_seconds`) stats start the matching effect on use; Luau `status.add(e, id, kind, amount, seconds)`, `status.has(e, id)`, `status.clear(e, id?)`, typed with a check test | kit, script | Sonnet | built (this PR): `inventory.cpp` use handler, `script/src/api_status.cpp`, tests in `test_inventory.cpp`, `test_script_inventory.cpp`, `test_script_check.cpp` |
| 3 | `samples/12_loot`: a regeneration tonic and a poisoned goblin strike, pinned log lines; user-guide text, close | samples, docs | Sonnet | built (this PR): `12_loot` drinks an elixir (regen, armor boost) while a poisoned strike runs; two pinned `status:` lines, run extended to 150 ticks |

## Step 4 — HUD text feed (built 2026-10-22)

| # | Step | Modules | Model | State |
|---|---|---|---|---|
| 4 | `ItemGridFeed` (stock player) fills Text widgets `status_1` to `status_8` from `StatusEffects`: `<id> <seconds>s`, time rounded up to tenths with integer arithmetic; empty slots blank; `tests/player/test_item_grid_feed.cpp` | player, docs | Sonnet | built (this PR) |

## Decisions

- Effect time is `u32` microseconds with the fixed step rounded to whole microseconds (as in `craft.cpp`), so a long effect ends a few
  ticks early at most and the same run always gives the same state hash.
- Poison goes through the same damage path as weapons so armor, `Died3D`, ragdolls and loot drops all work without a second code path.
- Bonuses (`Armor`, `MaxHealth`) are applied as a remembered delta, as `Equipment` does in M-ITEMFX, and are not saved (they
  re-apply on the first tick after a load; `Health3D` is not in a save game either).
- Stacking is off by default: re-applying an id refreshes its time, which is what potion spam should do.

## Open items

- Resistances, immunities and effect types beyond health and armor.
- Effect icons (images) in the HUD; the Text feed `status_1` to `status_8` is built (see Step 4).

## Review notes (step 1)

- Opus review: one blocker, fixed (a refresh that changed an effect's kind kept the old `applied`, leaking the old bonus). Also fixed:
  non-finite and unknown-kind saved slots are dropped, kill credit is per effect, clears run before applies, a hostile `pulse_us` is reset,
  Regen and Poison need positive amounts and amounts are limited to ±1e6. Left (documented): a `Health3D` overwritten at run time loses
  the bonus kinds, and two ids with the same 32-bit hash share a slot.

