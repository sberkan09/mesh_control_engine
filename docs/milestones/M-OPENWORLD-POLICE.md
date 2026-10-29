# M-OPENWORLD-POLICE — respawn, police cars and roadblocks in the city sample

Status: steps 1 to 3 done (2026-10-29); step 2 = pursuit cars on the roads, step 3 = roadblocks that come with the wanted level. Gap record: [M-OPENWORLD-GAPS](M-OPENWORLD-GAPS.md) E4 (A7, A8, A9). Data only: it wires kit components that
already exist ([M-WASTED](M-WASTED.md), [M-ARRESTLOSS](M-ARRESTLOSS.md), [M-HEALSHOP](M-HEALSHOP.md), [M-ARREST](M-ARREST.md) 1-3) into
`samples/09_openworld` (`gen_assets.py` and the regenerated `city.mcscene`). Steps 1-2 are data only; step 3 adds `Roadblock3D` to the kit.

## What the sample now has

- **Respawn.** The player carries `Respawn3D` (3 s, fine 100 busted / 200 wasted) and `Wallet3D` (500 cash). Dying wakes the player at
  the **Hospital** (`RespawnPoint3D`, west avenue at x = -150), an arrest at the **Police station** (plaza, x = 40). The wanted level is
  cleared, health restored and the fine taken from the wallet (never below zero). The stock caption shows "WASTED" / "BUSTED" during the
  wait. `Confiscate3D` is not used: the gun is on the camera, and that component only keeps back the ammunition of the Weapon3D on the
  respawning entity itself. The medic of M-HEALSHOP is not placed in the city yet.
- **Police cars.** Four `Police3D` cars (`drive_car` 1) wait at the plaza: two answer from wanted level 2, two from level 3. They
  drive at the player or at the last known place (`dispatch_range` 600 m) and brake close to it. They neither shoot nor arrest; the
  foot units do.
- **Roadblocks.** Two `Roadblock3D` markers at both ends of the middle avenue (x = -210 and x = 210). When the player reaches wanted level 3
  from at least 45 m away (at most 300 m), each marker spawns two static police cars side by side across the road and two officers
  in front of them (prefabs `roadblock_car` and `roadblock_officer`, `officer_back` -2.5); the officers hold their ground and fire
  when the player is within sight. 10 s after the level drops below 3 (and the player is 45 m or more away) they are destroyed.
  Kit: `kit/roadblock.h`, system `kit3d_roadblocks` after `kit3d_wanted`. Review fixes (#871): the pieces are made after the walk over the markers; a spawn that made nothing waits 5 s; a piece's place must be clear of anything with a `Health3D` or `Vehicle3D` within 3 m (it waits, so a parked car on the spot defers the block for good: move the marker); `up` is read from the pieces, which are run-time entities and not saved, so a load re-forms the block (test "a loaded game puts the block up again").

## Limits

- A roadblock stands in the road once spawned and traffic cars can queue against it; the block is in front of an arriving player only when the player comes from the plaza side (the layout is symmetric about the marker's line, but the officers are on one side). Pieces are created at the marker's height, so a marker on a slope needs a matching prefab.
- The pursuit cars follow the road network (`drive_car` 2, M-ARREST 4) and ignore traffic lights and other cars; a target away from any road (more than 80 m) is approached in a straight line.
- Cars neither ram nor shoot; no helicopters, no spike strips, no siren lights or sound (other threads: audio, HUD).
- Respawn points are flat pads on the avenue sidewalk; no hospital or station building.

## Test

`test_mcp_openworld` counts 4 pursuit cars and 4 roadblock officers, the two respawn points, and checks that the markers stand empty, that the east block (and only it) stands for a level-4 player 110 m away, arrests the player beside a plaza officer (wakes up at the station, 400 cash) and then has the west roadblock's officers shoot it (wakes up at the
hospital west of x = -100, wanted level 0, 200 cash less than the 500 it starts with).
