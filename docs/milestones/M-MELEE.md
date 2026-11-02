# M-MELEE: melee attack in 09_openworld

**Status:** done (2026-11-02), except a swing sound and hit feedback. Closes "melee" in [M-OPENWORLD-GAPS](M-OPENWORLD-GAPS.md) step 2c. Follows
[M-WEAPONSWITCH](M-WEAPONSWITCH.md); progression and goals come after it. Cover (2d) stays with the police thread.

**Scope.** The game layer only: `samples/09_openworld` plus one new kit module (`melee3d`). No ECS, render or physics core, no
HLOD or floating origin.

## Design

`Melee3D` on the attacker (the player): `damage`, `reach` (m), `arc_deg` (the cone in front of the attacker that a swing hits),
`rate` (swings/s), `windup` (s between the press and the hit), and engine-written `cooldown`,
`pending`, `swings`. It is independent of `Weapon3D`, so it works with any wielded gun and costs no ammo.

System `kit_melee` (FixedUpdate_Pre, after `kit_loadout`):

1. a press of the action `Melee` (or `use_input` off and `trigger` set by script) starts a swing when `cooldown` is 0, the
   attacker is alive and not a seated `Rider3D`;
2. after `windup` the swing hits every other entity with `Health3D` inside `reach` and `arc_deg` of the attacker's forward, in
   entity order (deterministic), through `apply_damage` (so `Damage3D`, `Died3D`, `Wanted3D` crimes, cash drops and hit markers
   work as for a bullet); a shove on physics bodies is left for later;
3. pushes `MeleeSwung3D { entity, hits }`.

Size: a padding-free struct with a `static_assert(sizeof)` (state_hash hashes raw bytes).

## Steps (seam, feature, polish; each its own PR)

| Step | What | Status |
|---|---|---|
| 1 seam | `Melee3D`, system `kit_melee`, swing event, unit tests (cone and reach, windup, rate, dead and seated attacker ignored, deterministic order). No sample change. | done |
| 2 feature | Sample: the player gets `Melee3D`; action `Melee` on V / gamepad right stick click; the pedestrians and police take damage (a HUD hint is left out). | done |
| 3 polish | The user-guide section, the MCP inspect check and the `M-OPENWORLD-GAPS` strike-through landed with steps 1 and 2. Damage, death, crimes and cash drops work as for a bullet; the hit marker and impact effects listen to `WeaponHit3D`, so a swing shows none yet, and a swing sound needs a new clip (both left open). | done |

## Notes

- Step 1 adds a component, so the transcript golden component count and `capabilities.md` move with it (integration runner
  regenerates; the golden count in the PR is bumped by hand as in #1017).
- Linear: the workspace is at its free-issue limit, so there is no card; this file is the record.
