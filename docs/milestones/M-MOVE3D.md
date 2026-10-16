# M-MOVE3D — 3D movement that feels shipped (milestone record)

**Status:** active (added 2026-10-16; step 1 done). Plan: ROADMAP §M-MOVE3D; Linear SBA-134 … SBA-139.

**Why:** M-GAMEPLAY3D gave 3D games their systems: navmesh agents, joints, ragdolls, vehicles and sensors.
What a player feels is still raw:

- `CharacterController3D` reaches full speed on the first tick. It has no coyote time, jump buffer, variable
  jump height, crouch or landing event.
- `CameraFollow3D` goes through walls and only orbits.
- `Vehicle3D` has no anti-roll, downforce, slip outputs or recovery after a flip. Nothing drives a vehicle
  but the player.
- Animation does not follow movement without game code.

It needs no device. Every step gates on headless runs and replay hashes in `local_ci.sh`.

**Not in scope:** GPU-only polish (motion blur, camera post effects), networked prediction of characters
(M-NET2 covers replication), new physics backends.

## Steps

| # | Step | Linear | State |
|---|---|---|---|
| 1 | Character feel: acceleration, deceleration and air control; coyote time, jump buffer, jump cut and air jumps; sprint and crouch; `Landed3D` | SBA-134 | done |
| 2 | Camera: spring arm, look-ahead, first person, lock-on | SBA-135 | not started |
| 3 | Vehicle feel: anti-roll, downforce, wheel slip and skid events, self-righting, gears as data | SBA-136 | not started |
| 4 | AI drivers: `VehicleDriver3D` on a waypoint path, laps; agents with acceleration | SBA-137 | not started |
| 5 | Animation from movement: `Locomotion3D`, foot IK on slopes and stairs | SBA-138 | not started |
| 6 | MCP and docs; the demo `samples/08_movement` | SBA-139 | not started |

**Demo:** `samples/08_movement`, built over MCP alone. A humanoid runs an obstacle course, and a cart races
two AI karts. A scripted input replays to the same hash in Debug and Release.

**Gates:**
- Each feel parameter has a headless test with exact numbers.
- Replay hashes hold across runs and builds.
- 64 characters and 8 vehicles step within a stated budget.
- Defaults keep existing scenes moving as before.
- `local_ci.sh` green on every step.

## Step 1 — character feel

`CharacterController3D` (`mce/kit/bodies3d.h`) gains these fields. Each is off by default, so a
scene without them moves as before. The existing character, nav and MCP tests are unchanged and
still pass.

- **Acceleration:** `acceleration` and `deceleration` move the ground velocity towards the wanted one
  at a fixed m/s² rate. `air_acceleration` replaces both in the air. 0 keeps the old instant
  response.
- **Jumps:** a jump request starts a buffer of `jump_buffer` seconds plus the current tick. The jump
  fires while grounded, within `coyote_time` seconds of leaving the ground (unless it left by
  jumping), or as one of `air_jumps`. Letting Jump go (`release_jump`, or the Jump action's release)
  while rising scales the rising speed by `jump_cut`.
- **Sprint and crouch:** `sprint` (or the Sprint action) scales the speed by `sprint_multiplier`.
  `crouch` (or the Crouch action) swaps the capsule through the new
  `World3D::set_character_height`. The shorter capsule is a `RotatedTranslatedShape` hung below
  the standing centre, so the entity's position and the feet stay where they were. Standing up asks
  Jolt's `SetShape` with a penetration limit of 1.5× the padding and is refused under a ceiling.
  Sensors see the crouched capsule where it is.
- **Landed3D:** an event (`FixedUpdate_Pre`) with the downward speed at touchdown. `FireOn::Landed`
  plays effects on it.

**Tests:** `tests/kit/test_move3d.cpp` covers the exact windows and rates at 60 Hz:

- acceleration of 0.5 m/s per tick;
- a 6-tick coyote window that rejects tick 7 and never follows a jump;
- a 0.1 s buffer that holds through landing for a request 5 ticks early but not 6;
- the jump cut's peak against v²/2g;
- one air jump per landing;
- sprint ×1.5 and crouch ×0.5, with standing refused under a 1.3 m roof;
- a landing at sqrt(2g·2.1) m/s that fires a `ParticleBurst`;
- a replay of every feature to the same hash.
