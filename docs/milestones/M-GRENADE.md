# M-GRENADE: thrown grenades in 09_openworld

**Status:** done (2026-11-03), except a throw sound, a blast effect and a shove on physics bodies. Closes "grenades" in [M-OPENWORLD-GAPS](M-OPENWORLD-GAPS.md) step 2c. Follows
[M-WEAPONSWITCH](M-WEAPONSWITCH.md), [M-MELEE](M-MELEE.md) and [M-PROGRESS](M-PROGRESS.md). Cover (2d) stays with the police thread.

**Scope.** The game layer only: `samples/09_openworld` plus one new kit module (`grenade3d`). No ECS, render or physics core, no
HLOD or floating origin.

## Why not a `Weapon3D` preset

`Weapon3D` already has a `Projectile` kind with `splash_radius`, but a grenade is a different thing to hold: a stack of
throwables (`count`, not `ammo` and `reserve`), a fuse that runs after the throw, and it must not replace the wielded gun. So
it is its own component on the thrower, like `Melee3D`, and uses the shared `apply_damage` entry for the blast.

## Design

`Grenade3D` on the thrower (the player): `count`, `max_count`, `damage` (at the centre), `radius` (m, damage falls off linearly
to the edge), `fuse` (s), `throw_speed` (m/s), `upward` (extra lift, so a throw arcs), `rate` (throws/s), and engine-written
`cooldown`, `thrown`, with `use_input` and `trigger` as for the other kit weapons.

`GrenadeLive3D` on the thrown body: `owner`, `timer`, plus the blast numbers copied from the thrower at the throw (so a later
change does not alter a grenade in flight). The body is a dynamic sphere from the existing physics kit, so it bounces and rolls
by itself.

System `kit_grenade` (FixedUpdate_Pre, after `kit_progress`):

1. a press of the action `Throw` (or `trigger` set by script) with `count` > 0, `cooldown` 0, an alive thrower who is not a
   seated `Rider3D`: spawn the body at the thrower, velocity along the thrower's forward at `throw_speed` plus `upward`,
   `count` -1, push `GrenadeThrown3D { entity, grenade }`;
2. each live grenade counts its `timer` down; at 0 it explodes: every `Health3D` entity within `radius` takes
   `damage * (1 - d / radius)` through `apply_damage` in entity order (deterministic), crediting the owner (so `Died3D`,
   `Wanted3D` crimes, cash drops and the progress counters work as for a bullet), the thrower included (friendly fire on
   the owner is on, so a grenade at the feet hurts), the grenade entity is destroyed
   and `GrenadeBlast3D { entity, owner, point, hits }` is pushed;
3. the blast reuses the splash effect that `Weapon3D` projectiles already show (listening to the event, not drawing its own).

Sizes: padding-free structs with a `static_assert(sizeof)` (state_hash hashes raw bytes).

Ammo shop and respawn: `kit3d_ammoshop` restocks `count` up to `max_count` from the same wallet, and `Confiscate3D`'s `ammo_kept` applies to the
grenade count too (small additions in `wanted3d.cpp`, as #1024 did for stored presets).

## Steps (seam, feature, polish; each its own PR)

| Step | What | Status |
|---|---|---|
| 1 seam | `Grenade3D`, `GrenadeLive3D`, system `kit_grenade`, events, unit tests (throw uses a grenade and respects cooldown and an empty stack, fuse timing, falloff at centre and edge, order, owner credit, dead or seated thrower ignored). No sample change. | open |
| 2 feature | Sample: the player gets `Grenade3D` (3 grenades); action `Throw` on B / gamepad Back (G is the horn, the shoulders are taken); HUD count; the shop sells them at 10 rounds' price; an arrest keeps the `ammo_kept` share; `test_mcp_openworld` inspect check. | done |
| 3 polish | The user-guide section, the capability row and the `M-OPENWORLD-GAPS` strike-through landed with steps 1 and 2 and this closing doc. A throw sound and a blast effect need a clip and an effect hooked to `GrenadeBlast3D` (the impact FX listen to `WeaponHit3D`), and a shove on dynamic bodies and a progression stat (grenade kills) are left open. | done |

## Decisions taken (change them in the plan PR if wrong)

- The blast hurts the thrower: realism over comfort, and it needs no team rule.
- Walls do not block the blast in step 1 (no line of sight check); add it later if it looks wrong.
- Fuse is time only, no impact trigger.
- A shove on physics bodies in range is left out of step 1 (the blast only calls `apply_damage`); add it in polish if wanted.
- The grenade flies by its own simple integration (gravity from `Physics3D`, a ray per tick, `bounce`), not as a physics body, so it needs no collider and replays stay cheap; without `Physics3D` it just falls.
- One grenade type. Molotov and smoke are later.

## Notes

- Step 1 adds components, so the transcript golden component count and `capabilities.md` move with it (the golden count in
  the PR is bumped by hand as in #1017; the integration runner regenerates the rest).
- Linear: the workspace is at its free-issue limit, so there is no card; this file is the record.
