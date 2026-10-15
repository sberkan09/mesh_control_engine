# SUMMARY — code (MarbleGarden)

## What I built
Game module `games/marble_garden/module/` (components.h, marble_garden.h, marble_garden.cpp), plus
`assets/input/controls.mcinput`.

Components
- `MarbleController` (on 'Marble', with RigidBody3D Dynamic Sphere): Move action pushes the body with
  impulses on `kit::Physics3D::world`, relative to the primary 3D camera's horizontal facing; max speed,
  braking/steering, `kill_y` fall respawn, `respawn` entity ('Start').
- `Gem` (with Trigger3D Sphere): spins about +Y, bobs, collected and destroyed when the marble enters.
- `Hazard` (with Trigger3D Box, the 'Pond'): marble entering it is teleported to 'Start' with zero velocity.
- `GardenGame` (on 'Game'): 90 s countdown, gem count, HUD TextLabels (time "M:SS", "Gems N / 5"),
  chime / splash SfxTrigger entities fired from code, win/lose scene change after a 1 s delay.
- `MenuScreen` (title/win/lose): Confirm / Space / Enter / click / South / tap / any typed key -> next_scene.
- `ResultLabel` (win/lose TextLabel): writes the last round's result and best time (from the
  `GardenResult` resource, which survives scene changes).
- Events: `marble_garden::GemCollected`, `marble_garden::MarbleRespawned`, `marble_garden::RoundOver`.

Input map `assets/input/controls.mcinput`: Move (WASD, arrows, stick, d-pad, touch stick), Confirm
(Enter, Space, mouse, South, Start, tap), Jump, Fire, Pause, Look, Cancel, and a UI map.

## Verified
- module_build clean, module_test (marble_garden_headless_300) passes.
- Headless runs on a scratch scene (deleted afterwards): marble rolls along -Z with Move=(0,1), camera-relative
  with camera yaw 90 deg; gem pickup counts, round won/lost, pond respawn, fall respawn, menu scene change on
  Confirm/Jump/Fire, respects min_time.
- On level's garden: walk_to bots collect gems 1,2,3,5 directly (gem 4 is behind the pond: needs a leg).
- Lose path end to end: garden idle 90 s -> lose screen with ResultLabel "0 of 5 gems before time ran out", Confirm retries.
- Fixed task 15 (log line was printf-style; MCE_LOG is fmt-style). Final: tests/ 10/10 pass, level's
  project_check pass and ship_build ok (dist/MarbleGarden-1.0.0-linux-x64.zip).

## Tasks done
3 (marble controller), 4 (gems, timer, HUD, win/lose), 5 (pond hazard), 6 (menus), 15 (log fix).

## Friction filed (4)
- Namespaces/headers of WorldTransform (mce::, ecs/hierarchy.h) and kit types undocumented.
- MCE_LOG_* is fmt-style; printf formats compile and log raw.
- .mctest event checks need the C++-qualified event name; unqualified silently counts 0.
- project_check lists the auto-loaded .mcinput as unused.

## Notes
- Residual spin after a respawn can roll the marble ~0.3 m (no API to zero angular velocity); the brake stops it.
- A garden -> lose/win change logs one audio warning ("streamed clip is already playing") from the music.
