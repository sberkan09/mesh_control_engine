# M-WEAPONSWITCH: weapon switching in 09_openworld

**Status:** done (2026-11-02), except per-weapon shot sounds and the mouse wheel. Closes "no weapon switching" in [M-OPENWORLD-GAPS](M-OPENWORLD-GAPS.md) step 2b.
Next in the same series, one milestone each: melee (2c), then progression and goals. Cover (2d) stays with the police thread.

**Scope.** The game layer only: `samples/09_openworld` plus one new kit module (`loadout3d`). No ECS, render or physics core, no
HLOD or floating origin.

## Design

The player keeps its single `Weapon3D`. Everything that reads it today keeps working unchanged: the ammo readout in `hud.mcui`,
`WeaponAudio`, `Confiscate3D` (an arrest keeps half the spare rounds), `AmmoShop3D`, `HitMarker3D` and `ImpactFx3D`.

`Loadout3D` on the same entity holds up to three *presets* (damage, range, rate, spread, pellets, magazine, reload time,
automatic, impulse, and the shot clip) plus each slot's saved `ammo` and `reserve`. Switching:

1. saves the leaving slot's `ammo` / `reserve`;
2. cancels a reload in progress;
3. copies the entering preset onto the `Weapon3D` and restores its `ammo` / `reserve`;
4. sets a short draw time (`Weapon3D.cooldown`) so a switch cannot be used to skip the fire rate;
5. pushes `WeaponSwitched3D { owner, from, to }` (the HUD name and sounds read it).

Inputs: `WeaponNext` / `WeaponPrev` (mouse wheel, gamepad shoulder buttons) and `Slot1..3` (keys 1 to 3). Nothing changes while
the player sits in a car (`Rider3D`).

## Steps (seam, feature, polish; each its own PR)

| Step | What | Status |
|---|---|---|
| 1 seam | `Loadout3D`, system `kit_loadout`, `WeaponSwitched3D`, unit tests (save and restore of ammo, reload cancelled, draw time, car seat ignored). No sample change. | done |
| 2 feature | Sample: pistol (today's gun), SMG, shotgun (8 pellets); keys 1 to 3, X / Z and D-pad up / down; the HUD shows the weapon name. (No mouse wheel: the input JSON has no wheel binding yet; no per-weapon shot sound yet, all three use the shots sound set.) | done |
| 3 polish | `AmmoShop3D` refills every slot, `Confiscate3D` halves every slot, save and load keeps the loadout (preset `ammo` / `reserve` and `current` are `Save` fields: no code needed), MCP inspect check (step 2), user-guide section. Per-weapon shot sounds and a mouse wheel stay open (need assets / an input binding). | done |

## Notes

- Each step adds a component or touches `gen_assets.py`, so the transcript golden component count and `capabilities.md` move
  with step 1 only.
- Linear: the workspace is at its free-issue limit, so there is no card; this file is the record.
