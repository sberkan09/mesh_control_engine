# M-MOVE3D — 3D movement that feels shipped (milestone record)

**Status:** active (added 2026-10-16; step 1 in progress). Plan: ROADMAP §M-MOVE3D; Linear SBA-134 … SBA-139.

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
| 1 | Character feel: acceleration, deceleration and air control; coyote time, jump buffer, jump cut and air jumps; sprint and crouch; `Landed3D` | SBA-134 | in progress |
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
