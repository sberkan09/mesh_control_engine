# M-MOVE3D — 3D movement that feels shipped (milestone record)

**Status:** done (added 2026-10-16; steps 1–6 done). Plan: ROADMAP §M-MOVE3D; Linear SBA-134 … SBA-139.

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
| 2 | Camera: spring arm, look-ahead, first person, lock-on | SBA-135 | done |
| 3 | Vehicle feel: anti-roll, downforce, wheel slip and skid events, self-righting, gears as data | SBA-136 | done |
| 4 | AI drivers: `VehicleDriver3D` on a waypoint path, laps; agents with acceleration | SBA-137 | done |
| 5 | Animation from movement: `Locomotion3D`, foot IK on slopes and stairs | SBA-138 | done |
| 6 | MCP and docs; the demo `samples/08_movement`; the hierarchy order fix (6b) | SBA-139 | done |

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

## Step 2 — the camera

`CameraFollow3D` (`mce/kit/effects.h`) gains these fields. Each is off by default, and with all of
them off the camera's pose is computed exactly as before.

- **Spring arm (`collide`):** each tick, `World3D::camera_sweep` sweeps a sphere of
  `collision_radius` from the pivot along the boom. The arm snaps in to the hit, measured along the
  boom, and grows back at `return_speed` once the way is clear. On the camera's first tick it starts
  at its reach rather than growing out from the target.
- **Look-ahead:** the pivot leads by `look_ahead` seconds of the target's ground velocity. The
  velocity is measured from the unsmoothed pivot's change per tick (`last_goal`), so it works for any
  target: a character, a body or an entity moved by a sequence.
- **First person:** the camera sits on the pivot with the orbit's rotation.
- **Lock-on:** the yaw turns the short way round, at most `lock_on_rate` × dt per tick, towards
  `atan2(target − locked)`, which puts the locked entity straight beyond the target. Look input
  stops turning the yaw while locked.

**Tests:** `tests/kit/test_move3d.cpp` covers:

- a wall face at 3 m puts the camera at 2.8 m (the 0.2 m sphere), while a boom without `collide`
  stays at 6 m;
- with the wall removed, the arm is at 3.8 m after 15 ticks at 4 m/s, then back at 6 m;
- look-ahead of 0.5 s at 3 m/s leads by 1.5 m and ignores vertical motion;
- the first-person pose and look direction;
- lock-on at 3 rad/s turns 0.5 rad in 10 ticks, then lines the camera, the target and the foe up;
- a replay with every feature hashes the same.

## Step 3 — vehicle feel

`Vehicle3D` (`mce/kit/vehicles3d.h`) gains these fields. Each is off by default, and a car without
them drives exactly as before.

- **Anti-roll (`anti_roll`, N/m):** one Jolt `VehicleAntiRollBar` across each axle (wheels 0–1 and
  2–3). `World3D` takes any pairs as `VehicleDesc3D::anti_roll_bars`. A bar stiffer than the
  springs' own rate (about 26,000 N/m for the default 1,500 kg car) makes the car unstable, so the
  guide says to stay under it.
- **Downforce:** an impulse of `downforce` × speed² × dt along the body's down axis each tick. It is
  not structural, so changing it does not rebuild the car.
- **Gears as data:** `gears` forward ratios spaced geometrically from `first_gear` to `top_gear`
  (0 keeps Jolt's stock five), and `shift_up_rpm` / `shift_down_rpm` for the automatic gearbox,
  also on `VehicleDesc3D`.
- **Slip and skids:** `WheelState3D` reports Jolt's `longitudinal_slip` and `lateral_slip` while a
  wheel is grounded. A wheel skids above 3 m/s when its sideways slip or its braking lock (negative
  longitudinal slip) passes `skid_slip`, and stops below 70% of it. Wheelspin and slow-speed noise
  never count. The start edge pushes `Skid3D` (`FixedUpdate_Physics`); `FireOn::Skid` plays effects
  on it. `skidding` is a bit per wheel and `lateral_slip` the largest sideways slip.
- **Self-righting:** with `self_right`, a car whose up axis has dropped under 0.6 (about 53°; the
  default `max_tilt` holds a flipped car at 60° on its side) and that moves under 1 m/s for
  `right_after` seconds is put a metre higher, upright with its old heading, at rest. `rightings`
  counts it.

**Tests:** `tests/kit/test_move3d.cpp`, on a flat road at 60 Hz:

- a full-lock turn at speed rolls the bare car past 0.5 rad onto two wheels; 8,000 N/m bars hold it
  under 0.1 rad on four;
- downforce 12 sits the car more than 2 cm lower at speed and exactly as high at rest;
- `gears = 2` tops out in 2nd while the stock box passes it, and the first shift comes at tick 66,
  122 and 224 for 3,000, 4,000 and 5,000 rpm;
- gentle driving raises no skid, a handbrake turn skids for more than 10 ticks with a `Skid3D` and a
  `ParticleBurst`, and `skid_slip = 0` raises nothing;
- a car dropped on its roof is set upright after 0.5 s facing its old heading, and stays tilted
  without `self_right`;
- a replay with every feature hashes the same.

## Step 4 — AI drivers

- **`Waypoint3D`** (`mce/kit/vehicles3d.h`): a point on a path, chained by `next`. Its `speed` caps
  the car there, and `radius` sets how close counts as reached; passing the point along the leg into
  it counts too.
- **`VehicleDriver3D`**, on the car, writes its inputs inside `kit3d_vehicles` before they are
  applied.
  - **Steering:** pure pursuit at a point `look_ahead` metres (at least 0.6 s) down the path. The
    heading error is measured in yaw alone, so a leaning body cannot bend it. The wheel angle is
    capped so the sideways pull stays under 1.5× `corner_accel` at the current speed. Steering at
    the waypoint itself, or with no cap, ran the test car wide and onto two wheels.
  - **Speed:** the lowest of `max_speed` and three limits:
    - each of the next two corners, braking at `brake_decel` (4 m/s², what the stock brakes deliver
      at low speed) down to sqrt(`corner_accel` × r), with r from the turn angle and the shorter
      half-leg, capped by the waypoint's `speed`;
    - the point where the next waypoint counts as reached;
    - the arc to the aim point.
  - **Laps:** the first time the car reaches `path`, lap timing starts. Each time after, it counts a
    lap and pushes `LapCompleted3D` (`FixedUpdate_Pre`, read by `FireOn::Lap`). After `laps` laps it
    brakes to a stop.
- **Navmesh agents:** an agent steering a `CharacterController3D` already writes its `move`, so step
  1's `acceleration` and `deceleration` apply to it with no new code. A test now pins this.

**Tests:** `tests/kit/test_move3d.cpp` runs a 40 × 120 m rectangle of five waypoints with an
8,000 N/m anti-roll car:

- four laps end in `finished`, with four `LapCompleted3D` events. Laps 2–4 fall within 0.5 s of
  each other (about 24.9 s), and the first lap, from rest, is slower.
- the top speed is 19–20.5 m/s, the roll stays under 0.15 rad, and the car never runs more than 5 m
  outside the rectangle. After the last lap it stops and holds.
- half the `corner_accel` costs more than 3 s a lap, and so does a `max_speed` of 12.
- a 3 m/s cap on the first corner takes the car through it under 3.5 m/s, more than 1 m/s slower
  than with no cap.
- `FireOn::Lap` fires once per lap, and `drive = 0` leaves the inputs alone.
- a driven lap replays to the same hash.

`tests/kit/test_nav3d.cpp`: an agent steering a character with 2 m/s² reaches 31/60 × 2 m/s after
31 ticks, while the instant controller is past 2 m/s.

## Step 5 — animation from movement

- **`Locomotion3D`** (`mce/kit/anim3d.h`), on the mover, runs as `"kit3d_locomotion"` in
  `FixedUpdate_Post` before `"kit3d_animators"`.
  - It reads the motion from a `CharacterController3D` (velocity, `grounded`, `crouched`), else a
    `NavAgent3D`'s velocity, else the change of the world position.
  - It writes the graph parameters it finds by name: `speed`, `move_x`, `move_y`,
    `vertical_speed`, `grounded`, `airborne` and `crouch`.
  - The speeds are eased over `smoothing` seconds.
  - With `stride_speed`, it sets the Animator's play rate to speed ÷ `stride_speed`, clamped, and
    to 1 when still.
  - `animator` points it at a model on another entity.
- **Foot IK on slopes and stairs** is the existing foot chain (M-GAMEPLAY3D 5). Step 5 adds tests
  for a slope and for a walking character.

**Tests:** `tests/kit/test_anim3d.cpp`:
- A character with a graph whose states switch only on those parameters goes from idle to walk at
  3 m/s, with `speed` and `move_y` at 3 and the walk at 1.5× for a 2 m/s stride. Walking sideways
  leaves `move_y` at 0 and `move_x` at 3, and at 6 m/s the rate clamps at 2.
  - Stopping returns to idle at rate 1.
  - A jump goes to the air state, with `grounded` at 0 and a rising `vertical_speed`, and lands
    back in idle.
  - Crouching ducks, and standing returns.
- An entity moved only by its transform drives another entity's Animator at exactly 3 m/s.
- On a 10° slope the feet sit at ±0.2·tan 10° and the pelvis drops by the lower foot's depth.
- A character walking 2 m/s beside a 0.15 m stair keeps one foot on the step and one on the floor
  within 1 cm (measured: 0.1 mm) on every tick of a 90-tick walk.

**Hierarchy order (fixed in step 6b):** `ecs::install_hierarchy` used to register
`"transform_propagate"` only where it was called. A shipped scene with a `ChildOf` gets it at
`scene::load`, after `kit::install_stock`, so in `FixedUpdate_Post` it ran after the kit's animators,
IK and `AttachToBone`, and a model hanging as a child of its mover was posed and foot-cast from last
tick's `WorldTransform`. The hierarchy now also registers `"transform_propagate_early"` at the front
of `FixedUpdate_Post` (`World::add_system_first`), so every Post reader sees this tick's moves; the
late pass stays where install was called and only redoes what Post systems moved after the early
one. The stair test installs the hierarchy after the kit, the shipped order. Replay impact: of the
headless sample hashes only `07_gameplay3d` changes (`ee6bfe319ca8e23b` -> `cc49e872cae6cadd` at 600
ticks). A per-entity dump of both runs differs in one entity only, the camera: `kit_camera_follow`
(`FixedUpdate_Post`) used to read the cart's `WorldTransform` from last tick and now follows this
tick's pose. The cart, grunts, ragdolls, door and bridge are byte-identical at every tick checked.

## Step 6 — the demo and the gates

- **Demo `samples/08_movement`** is an editor project (assets and `scenes/course.mcscene`, no code) built over
  MCP alone by `tests/mcp/test_mcp_move3d.cpp` (`MCE_MOVE3D_EXPORT=samples/08_movement` writes it):
  - A `gen_mesh` humanoid on a `CharacterController3D` (`use_input`, acceleration 40, coyote time, jump
    buffer, crouch) with `Locomotion3D` driving an `.mcanim` graph (Idle and Walk), under a
    `CameraFollow3D` with `collide`.
  - The course: a 0.8 m hurdle (above the 0.35 m step height, so a jump is needed), a lintel with 1.5 m of
    headroom (a crouch is needed), and a kinematic lift a looping `.mctimeline` moves up to a 3.5 m ledge.
  - A race: three karts (`Vehicle3D`, anti-roll 8000) on `VehicleDriver3D` over five `Waypoint3D` points, two
    laps each.
- **Gates:**
  - `test_mcp_move3d` runs the course with a scripted input and checks every obstacle (jump over the
    hurdle, stopped by the lintel when standing, crouched through it, carried up and onto the ledge),
    that the graph is on Walk at 5 m/s and that Locomotion3D reads it, that all three karts finish two
    laps in 15 to 45 s, and that a replay matches.
  - `ctest` `sample_08_movement_player_900` plays it with `drive.input` (900 ticks, 17 entities).
  - `local_ci.sh` determinism runs the same 900 ticks three times on the Debug and Release players and
    compares the hashes.
  - `tests/kit/test_move3d.cpp` "64 characters and 8 driven cars": 300 ticks of the crowd and the race
    together at 6.4 ms a tick in Debug (the budget asserts 16.6 ms in Release builds), and two runs hash
    equal.
- **Coverage:** `Locomotion3D`, `Waypoint3D` and `VehicleDriver3D` leave `sample_coverage.allow`.
