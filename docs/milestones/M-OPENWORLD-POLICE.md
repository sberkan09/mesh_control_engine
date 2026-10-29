# M-OPENWORLD-POLICE — respawn, police cars and roadblocks in the city sample

Status: step 1 done (2026-10-29). Gap record: [M-OPENWORLD-GAPS](M-OPENWORLD-GAPS.md) E4 (A7, A8, A9). Data only: it wires kit components that
already exist ([M-WASTED](M-WASTED.md), [M-ARRESTLOSS](M-ARRESTLOSS.md), [M-HEALSHOP](M-HEALSHOP.md), [M-ARREST](M-ARREST.md) 1-3) into
`samples/09_openworld` (`gen_assets.py` and the regenerated `city.mcscene`). No engine code changed.

## What the sample now has

- **Respawn.** The player carries `Respawn3D` (3 s, fine 100 busted / 200 wasted) and `Wallet3D` (500 cash). Dying wakes the player at
  the **Hospital** (`RespawnPoint3D`, west avenue at x = -150), an arrest at the **Police station** (plaza, x = 40). The wanted level is
  cleared, health restored and the fine taken from the wallet (never below zero). The stock caption shows "WASTED" / "BUSTED" during the
  wait. `Confiscate3D` is not used: the gun is on the camera, and that component only keeps back the ammunition of the Weapon3D on the
  respawning entity itself. The medic of M-HEALSHOP is not placed in the city yet.
- **Police cars.** Four `Police3D` cars (`drive_car` 1) wait at the plaza: two answer from wanted level 2, two from level 3. They
  drive at the player or at the last known place (`dispatch_range` 600 m) and brake close to it. They neither shoot nor arrest; the
  foot units do.
- **Roadblocks.** At both ends of the middle avenue (x = -210 and x = 210) two police cars stand side by side with the handbrake on,
  with two officers behind them. The officers stay put (`drive_nav` 0, no dispatch) and open fire from level 3 when the player comes
  within 70 m.

## Limits

- The roadblocks are always there; only their officers wake up with the wanted level. The engine has no component that spawns a
  barricade at a level (M-ARREST open item "spawning fresh units"). Traffic cars can queue against a roadblock.
- A police car has no path following (M-ARREST 3): a wall between it and the target stops it, and the navmesh does not help cars.
- Cars neither ram nor shoot; no helicopters, no spike strips, no siren lights or sound (other threads: audio, HUD).
- Respawn points are flat pads on the avenue sidewalk; no hospital or station building.

## Test

`test_mcp_openworld` counts 4 pursuit cars and 4 roadblock officers, the two respawn points, and arrests the player beside a plaza officer (wakes up at the station, 400 cash) and then has the roadblock officers shoot it (wakes up at the
hospital west of x = -100, wanted level 0, 200 cash less than the 500 it starts with).
