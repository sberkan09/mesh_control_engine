# M-WEAPONFEEL: reload, dry-fire and weapon-switch feedback

**Status:** done (2026-11-10). Picks up the open "reload" item of [M-OPENWORLD-GAPS](M-OPENWORLD-GAPS.md) at the feedback level:
the gun already reloads (`Weapon3D::reload_time`, the Reload action, automatic reload on an empty magazine) and
[M-WEAPONSWITCH](M-WEAPONSWITCH.md) already switches, but neither is audible or visible, and pulling the trigger on an empty
gun with no spare rounds does nothing at all. Game layer only: `samples/09_openworld` plus kit changes that stay inside
`combat3d` and `loadout3d`. No ECS, render or audio core. Needs no device or account.

## Why

Reload starts and ends in silence, a dry trigger gives no click, and a weapon switch (keys 1 to 3, X / Z, D-pad) only changes the HUD.
`WeaponFired3D` and `WeaponSwitched3D` are the only events, so a sound or a caption cannot tell a reload from nothing.

## Design

- Two new events beside `WeaponFired3D`: `WeaponReloadStarted3D {weapon, owner}` and `WeaponReloaded3D {weapon, owner, rounds}`,
  pushed by the existing weapon system on the tick `reloading` rises and falls. A third, `WeaponDry3D {weapon, owner}`, once per
  trigger pull on an empty gun that cannot reload (no reserve), rate-limited by the fire rate.
- `EventSfx3D` (from [M-GAMEFEEL-SFX](M-GAMEFEEL-SFX.md)) gains the clips and gains for these three and for `WeaponSwitched3D`, so the
  next event still costs a field, not a module. Changing it means one component-size bump, a golden count that does not move.
- Sample: the player's `EventSfx3D` takes clips from the CC0 set already in the sample; the reload caption "Reloading" uses `Caption`
  only if the HUD has no room for a reload bar (decided in step 2).
- Padding-free structs with `static_assert(sizeof)`; events are reflected with `MCE_EVENT`.

## Steps (seam, feature, polish; each its own PR)

| Step | What | Status |
|---|---|---|
| 1 seam | The three weapon events in `combat3d` (pushed by the existing system), `EventSfx3D` fields for them and for `WeaponSwitched3D`, unit tests (reload start and end each push one event with the right owner, an automatic reload too, a dry pull pushes one event and not a second inside the fire interval, an infinite-ammo gun never reports dry, a dead owner is silent). No sample change. | done (2026-11-10, #1218) |
| 2 feature | Sample: clips for reload, dry click and switch, `test_mcp_openworld` inspect check, regenerated assets, any temporary `sample_coverage` line removed. | done (2026-11-10, #1220) |
| 3 polish | Closing doc, the "reload" strike-through in M-OPENWORLD-GAPS (the user-guide section landed with step 1). The HUD reload bar stays open: the sample's HUD has no spare slot and the ammo readout already shows the loaded count. | done (2026-11-10) |

## Decisions taken (change them in the plan PR if wrong)

- Events, not polling: the sound system should not diff `Weapon3D::reloading` itself.
- Reuse the clips already in the sample; a dedicated mechanical reload sample needs a new CC0 fetch rule and a listen on real
  speakers, so it is a later swap of the clip fields (no code change).
- Cover, aim-down-sights and a tracer or bullet-hole decal are not here (cover stays with the police thread).
- Not heard here: the cloud has no output device; the unit tests check the events and `PlaySfx`, loudness is a first guess.

## Notes

- Step 1 touches a component (`EventSfx3D`) and adds events: `capabilities.md` and the three `engine_components.cpp` files move with it.
- Linear: the Backlog holds only work that needs real devices or accounts, so this file is the record.
