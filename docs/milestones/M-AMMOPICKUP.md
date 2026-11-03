# M-AMMOPICKUP: ammo crates in 09_openworld

**Status:** plan (2026-11-03). Closes "no ammo pickups" in [M-OPENWORLD-GAPS](M-OPENWORLD-GAPS.md) section C. Follows
[M-GRENADE](M-GRENADE.md) and [M-AMMOSHOP](M-AMMOSHOP.md) (the shop sells rounds for cash; a crate gives them away, once in a
while, where you find it). Cover (2d) stays with the police thread.

**Scope.** The game layer only: `samples/09_openworld` plus a small addition to the kit's pickup module in `wanted3d` (next to
`CashPickup3D`). No ECS, render or physics core, no HLOD or floating origin.

## Design

`AmmoPickup3D` on a placed entity (a crate), modelled on `CashPickup3D`: `rounds` (30), `grenades` (0), `max_reserve` (150, the
reserve it never fills past), `radius` (1.5 m), `respawn` (s until it can be taken again; 0: destroyed when taken) and
engine-written `left`.

System `kit3d_ammopickup` (after `kit3d_cashpickup`): while `left` is above 0 it counts down. Otherwise the first entity with a
`Weapon3D` (a `magazine` above 0, not `infinite_ammo`) within `radius` is topped up: the wielded weapon and, with a `Loadout3D`,
every stored preset get `rounds` more spare rounds, each at most to `max_reserve`; `grenades` are added to a `Grenade3D` up to its
`max_count`. If nothing could be added (all full) the crate stays. Otherwise `AmmoPickedUp3D { entity, pickup, rounds, grenades }`
is pushed and the crate is destroyed or put on its `respawn` timer.

Size: padding-free struct with a `static_assert(sizeof)` (state_hash hashes raw bytes). One new component and one event, so the
transcript golden count and `capabilities.md` move with step 1.

## Steps (seam, feature, polish; each its own PR)

| Step | What | Status |
|---|---|---|
| 1 seam | `AmmoPickup3D`, system `kit3d_ammopickup`, event, unit tests (top-up of the wielded weapon and of stored presets, the cap, a full buyer leaves the crate, respawn timer, grenades, infinite-ammo and unarmed holders ignored). No sample change. | open |
| 2 feature | Sample: a few crates on the avenue and by the park (60 s respawn, 30 rounds, one with 1 grenade), a map marker, `test_mcp_openworld` inspect check. | open |
| 3 polish | User-guide section, closing doc, `M-OPENWORLD-GAPS` strike-through; a pickup sound and a visible crate mesh if a model exists. | open |

## Decisions taken (change them in the plan PR if wrong)

- A crate fills every weapon in the backpack, not only the wielded one: the player never has to guess which gun to hold.
- Walkers do not drop crates; `LootDrop` stays for bandages and medkits.
- No magnet pull; walking over the crate is enough.
- Linear: the workspace is at its free-issue limit, so there is no card; this file is the record.
