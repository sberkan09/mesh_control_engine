# 9. Physics

2D physics is scene-authored: a body is an entity with `RigidBody2D` and `Collider2D`, the engine
steps the world every fixed tick in `FixedUpdate_Physics` and writes the resulting poses into
`LocalTransform`. Game code reads events and controller state, moves kinematic bodies by velocity,
and uses queries. 3D physics is the kit's `Physics3D` world with static `BoxCollider3D` / `MeshCollider3D` / `TerrainCollider3D` (sections 8.2 and 8.7).

## 9.1 Bodies and colliders

```json
"Ground": {
  "LocalTransform": { "position": [0, -3, 0] },
  "RigidBody2D": {},
  "Collider2D": { "shape": { "half": [10, 0.5] }, "density": 0 }
},
"Crate": {
  "LocalTransform": { "position": [0, 5, 0] },
  "RigidBody2D": { "type": "Dynamic" },
  "Collider2D": {}
}
```

`RigidBody2D`:

| Field | Meaning |
|---|---|
| `type` | `Static` (never moves), `Kinematic` (moved by you, pushes others), `Dynamic` (simulated) |
| `fixed_rotation` | keep the body upright |
| `layer` | this body's collision layer, 0..15 |
| `mask` | bit set of layers it collides with (`0xFFFF` = all; a sensor that should see only layer 1 has `mask: 2`) |
| `gravity_scale` | 0 for characters and floating things |
| `linear_damping` | velocity decay |

`Collider2D`: one shape per body plus `density` (0 = massless, which static and kinematic bodies
should use), `friction`, `restitution`, `is_sensor` (overlap events, no collision response),
`one_way` (blocks the character controller only when it moves downward: platforms you jump through).

Shapes: `{"type": "Box", "half": [w, h], "offset": [x, y], "rotation": r}`,
`{"type": "Circle", "radius": r, "offset": [x, y]}`, `{"type": "Convex", "vertices": [...],
"vertex_count": n}` (up to 16 points; the hull is computed for you). `Box` is the default.

Rules that matter in play:

- **Physics owns the pose of a bodied entity.** Write `LocalTransform` to place it in the scene;
  during play, move it with `World2D::teleport(e, pos, angle)` or by velocity, not by writing the
  transform, which physics overwrites each tick.
- A body exists after the first physics step, so `has_body(e)` is false during the first tick's
  `FixedUpdate_Pre`. Guard first-tick logic on it.
- One collider per body. Compound shapes are child entities with their own bodies.
- A degenerate shape (zero-size box, fewer than three hull points) makes no body; the entity simply
  does not collide until its components change.
- Changing `RigidBody2D`, `Collider2D` or `CharacterController2D` bytes re-imports the body;
  nothing else you write to those components matters mid-play.

## 9.2 The physics world from code

Gameplay reaches the world through the resource:

```cpp
physics::World2D& pw = f.resource<physics::World2D>();
pw.set_linear_velocity(e, v);         // kinematic platforms: velocity that lands on the path
pw.apply_impulse(e, impulse, world_point);   // at a world point (the centre of mass for no spin)
pw.apply_force(e, force);
pw.teleport(e, position, angle);
pw.set_enabled(e, false);             // switch a body off: no collisions, no sensor events
physics::BodyState s; if (pw.body_state(e, s)) { s.position; s.angle; s.linear_velocity; }
```

Mutate bodies in `FixedUpdate_Pre`; read results in `FixedUpdate_Post`. `set_enabled(e, false)` is
how crumbling blocks and Kindle's colour blocks vanish without destroying the entity: the body
keeps its slot and comes back with `set_enabled(e, true)`.

A moving platform that follows a path exactly: compute where it should be at the end of this tick
and set the velocity that gets it there, `(target − current) / dt` (Kindle's `Mover`).

## 9.3 The character controller

A player or enemy that should walk, not tumble, is a kinematic body with a swept controller:

```json
"Player": {
  "RigidBody2D": { "type": "Kinematic", "fixed_rotation": true, "gravity_scale": 0 },
  "Collider2D": { "shape": { "half": [0.4, 0.6] }, "density": 0 },
  "CharacterController2D": { "max_slope_deg": 45, "snap_down_distance": 0.05 },
  "PlayerController": { "run_speed": 7, "jump_speed": 13 }
}
```

Gameplay applies its own gravity to a velocity it keeps on its component, then sweeps:

```cpp
physics::QueryFilter filter;
filter.layer_mask = 1u << kWorldLayer;
const physics::MoveResult2D r = pw.move_and_collide(e, p.vel * f.dt, filter);
grounded = (r.flags & physics::ControllerFlags::Ground) != 0;
if (grounded && p.vel.y < 0) p.vel.y = 0;
if (r.hit_ceiling && p.vel.y > 0) p.vel.y = 0;
wall_left  = (r.flags & physics::ControllerFlags::WallLeft) != 0;
```

The controller handles slopes up to `max_slope_deg`, snaps down on stair descents, respects
`one_way` platforms (and lets you drop through), and carries the character on moving platforms
(`OnMovingPlatform`, `platform_velocity`, `ground_body`). The same results are mirrored into a
read-only `CharacterGround2D` component the engine adds to controller entities.

Kindle's `kindle_player` system is a complete platformer controller: acceleration, variable-height
jumps, coyote time, a jump buffer, wall slide and wall jump, an air dash, respawn below `kill_y`.

## 9.4 Triggers and contacts

A collider with `is_sensor: true` raises events instead of colliding. Read them in
`FixedUpdate_Post`:

```cpp
for (const physics::TriggerEnter& ev : f.events().read<physics::TriggerEnter>()) {
    if (ev.other != player) continue;
    if (const Coin* c = f.world().try_get<Coin>(ev.sensor)) {
        score += c->value;
        f.commands().destroy(ev.sensor);
    }
}
```

`TriggerEnter` and `TriggerExit` fire exactly once per overlap (exit also when either body is
destroyed). `ContactBegin2D` / `ContactEnd2D` report solid contacts with the point, the normal (from
`a` to `b`) and the approach speed, for landing sounds and damage from impacts. The number of
events per tick is capped (`max_contact_events_per_tick`).

Without code, `PhysicsEvents` on an entity records the events that name it: `last_trigger` and
`last_contact` (the other entity), the last contact's `contact_point`, `contact_normal` (from this
entity towards the other) and `contact_speed`, counts of each event, and `overlapping` / `touching`
now. It is engine-written, so data, sequences and `run_headless inspect` read it. The effect
components (`SfxTrigger`, `ParticleBurst`, `ShakeTrigger`, chapters 7 and 10) fire on the same
events with `on: TriggerEnter` or `ContactBegin`.

Sensors see what their `mask` allows: a pickup that should react only to the player on layer 1 has
`"mask": 2`, so enemies walking through it raise nothing.

## 9.5 Queries

Read-only and allowed in any phase:

```cpp
if (auto hit = pw.raycast(origin, dir, max_dist, filter)) { hit->entity; hit->point; hit->normal; hit->t; }
pw.raycast_all(...);  pw.shape_cast(shape, from, to, filter);  pw.overlap_point(p, filter);  pw.overlap_aabb(box, out, filter);
```

`QueryFilter` has a `layer_mask`, an `exclude` list, `include_sensors` (false by default, so
queries skip sensors) and an optional `predicate`.
Results are sorted, so queries are deterministic.

In 3D, `kit::Physics3D::world` answers `raycast`, `shape_cast` (a sphere), `overlap_sphere(center,
radius, out, filter)` and `overlap_box(center, half, rotation, out, filter)`; the overlaps fill `out`
in creation order and return how many touch, even when `out` is shorter. `QueryFilter3D` has
`layer_mask`, `exclude`, `include_dynamic` and `include_characters` (false by default: characters'
capsules are not bodies, so set it to see them in a ray or an overlap). Without code, the
`Raycast3D` and `Overlap3D` components (section 8.7) run these every tick from an entity and raise
trigger events when what they see changes.

## 9.6 Joints

From code: `DistanceJointDesc` (a rod, a rope with `min_length < max_length`, or a spring with
`hertz > 0`) and `RevoluteJointDesc` (a hinge with an optional angle limit and motor), created on
the world with `create_distance_joint(desc)` / `create_revolute_joint(desc)` and addressed by the
returned `JointId` (`destroy_joint`, `set_motor_speed`, `set_joint_limits`). Prismatic, wheel, weld and mouse joints are not built.
2D joints are not scene-authored yet.

**3D joints** are components. Put a `Joint3D` on a body's entity (a `RigidBody3D`, or a static
`BoxCollider3D` / `MeshCollider3D` when the other side moves) and set `connected` to the other body's
entity, or leave it empty to join the body to the world. `kind` picks the joint:

| kind | What it does | Its settings |
|---|---|---|
| `Fixed` | glues the two bodies together | — |
| `Hinge` | turns about `axis` through `anchor` (a door, a wheel, a drawbridge) | `limits`, `limit_min` / `limit_max` in degrees, `friction` (N·m), the motor |
| `Slider` | slides along `axis` without turning (a piston, a lift) | `limits`, `limit_min` / `limit_max` in metres, `friction` (N), the motor |
| `Distance` | keeps `anchor` and `connected_anchor` apart (a rod, a rope, a chain link) | `min_distance` / `max_distance` (< 0: the distance when built); a rope is `min_distance = 0` |
| `Cone` | a ball joint whose `axis` swings at most `cone_angle` degrees | `cone_angle` |
| `SixDOF` | each axis of a frame (X = `axis`) locked, free or limited | `linear_min` / `linear_max` (m), `angular_min` / `angular_max` (degrees): min > max is free, min = max locked, else limited |

`anchor` and `axis` are in the entity's space; `connected_anchor` is in the connected entity's space
(the world's without one). The joint is built the first tick both bodies exist, and their poses
then are its rest pose: a hinge's angle and a slider's position start at 0. Changing a setting
rebuilds it from where the bodies are at that moment, except `enabled` (0 switches the joint off
and keeps it) and the motor.

- **Soft limits.** `spring_frequency > 0` turns hinge and slider limits, and the distance, into
  springs (`spring_damping` 0..1): a bungee rope, a door that bounces off its stop.
- **Motors** (hinges and sliders). `motor = Velocity` turns or slides at `motor_target` deg/s or
  m/s; `motor = Position` drives to `motor_target` degrees or metres and holds it, as stiffly as
  `motor_frequency` (Hz). `motor_max_force` caps the torque or force (0 = unlimited). Changing the
  target or the mode does not rebuild the joint, so a sequence or a game module can open a door or
  raise a drawbridge by writing `motor_target`.
- **Breaking.** Above `break_force` (N) or `break_torque` (N·m) the joint breaks: `broken` turns 1,
  a `JointBreak3D { joint, connected }` event is raised in `FixedUpdate_Physics`, and the joint stops
  acting until one of its settings changes.
- **Collisions.** The two bodies of a joint do not collide with each other, so a hinge pin can sit
  inside both; set `collide_connected = 1` to keep them colliding.
- **What it reports.** `position` (hinge degrees, slider or distance metres), `force` (N) and `torque`
  (N·m) the joint held with in the last step: enough for a creaking-door sound or a "too heavy" check.

Removing the component, or either body, removes the joint. From code, `physics::World3D` has the same
joints (`create_joint(JointDesc3D)`, `set_joint_motor`, `set_joint_enabled`, `joint_state`), in
radians and world space.

## 9.7 3D navigation

A 3D scene gets a navigation mesh (navmesh) and agents that walk it from five components
(`mce/kit/nav3d.h`, on Recast & Detour, ADR-0040). The stock player, play-in-editor and MCP's
`run_headless` run them; nothing needs code.

- `NavMeshSurface3D`: one per scene (the first in ECS order is used). Its settings say what an
  agent can do (`agent_radius`, `agent_height`, `agent_max_climb`, `agent_max_slope_deg`) and how
  finely to voxelize (`cell_size`, `cell_height`, `tile_size`). The mesh is baked from the scene's
  `BoxCollider3D`, `MeshCollider3D` and `TerrainCollider3D` in the layers of `layer_mask` (a
  terrain's holes stay open; plus `MeshRef` meshes of entities without a collider when
  `include_meshes` is set). **File → Bake navmesh** in the editor (or MCP's `navmesh_bake`,
  chapter 14) writes `assets/navmesh/<scene>/nav_<id>.mcnav` (a `NavMesh` asset) and points `baked`
  at it, and the editor's 3D view draws it (green floor, amber costly areas, pink links; `show` turns
  it off). Without a bake the mesh is baked when the scene starts, and again whenever `version`
  changes. The bake is byte-identical run to run.
- `NavArea3D`: a box (axis-aligned) whose floor takes an `area` id with a path `cost` per metre:
  mud at cost 5 is walked around unless the detour is longer. Area 0 cuts a hole; 1 is the ground.
- `NavLink3D`: an off-mesh link from the entity to `end` (a jump across a gap, a ladder): paths
  use it like floor.
- `NavObstacle3D`: a box carved out of the mesh while `enabled`, following its entity (a door, a
  crate, a drawbridge); agents re-plan around it. Areas and links are baked in, obstacles are not.
- `NavAgent3D`: set `target` and `move_to = 1` and the entity walks there at `speed`, steering
  around other agents (`avoid_agents`, `separation`). It writes `state` (Idle, Moving, Arrived,
  NoPath, OffMesh), `velocity`, `distance` and `arrivals`. An agent with a `CharacterController3D`
  (with `use_input` off) drives the controller's `move`, so it walks slopes and steps and pushes
  bodies, easing in and out at the controller's own `acceleration` and `deceleration` when they are
  set (8.7); otherwise its `LocalTransform` is put on the mesh. `face_motion` turns its local −Z to the
  way it walks (a turn alone never resets the controller's speed; moving the entity yourself does).
  The controller's `acceleration` and `deceleration` are the only limits on how fast it gets up to
  speed, so leave them 0 for a sentry that should move at `speed` at once. With `follow` set to an entity (the player, a cart, a leader) the engine writes that
  entity's position into `target` every tick and re-plans once it has moved `follow_repath` metres,
  so an enemy chases without code.

From code, the `kit::Nav3D` resource holds the loaded `physics::NavMesh3D`: `find_path`,
`raycast` (along the floor, stopping at walls), `nearest_point` and `area_at` work any time after
the surface's `ready` is 1. Budget: a 200 × 200 m level at the default 0.2 m cells bakes in well
under a second; a crowd of 128 agents steps in about a millisecond.

## 9.8 Ragdolls

A skinned character goes limp with a `Ragdoll3D` component (`mce/kit/ragdolls3d.h`) next to its
`MeshRef` (and usually its `Animator`). Setting `active = 1` turns the skeleton into bodies, posed
and moving as the animation had them that moment, and from then on the physics poses the mesh:
the renderer, and anything pinned with `AttachToBone`, follow the bodies. Setting `active = 0`
removes the bodies and blends from the last pose back to the animation over `blend_time` seconds.

- **The parts.** Skin 0's skeleton gives one body per bone: a capsule from a bone to its child (a
  thigh, a spine), a box over the vertices of a bone with several children (a pelvis, a chest), a
  capsule or ball for a leaf (a hand, the head), each sized from the vertices the bone moves
  (`radius_scale` thickens or thins them all). Each joins its parent with a joint that swings up to
  `swing` degrees and twists up to `twist` either way. A bone that moves no vertex (a weapon
  socket), or is shorter than `min_bone_length`, rides with its parent. `mass` (kg) is shared by
  size. `parts` reports how many bodies were built.
- **Hits.** `impulse` (N·s) is applied once while active and then cleared: on the whole body, or
  on the bone named in `impulse_bone` (a shot to the head). Setting `active` and `impulse` in the
  same tick knocks a character over. With `on_trigger = 1` the character goes limp by itself when a
  `TriggerEnter3D` names it: a `Trigger3D` it walks into, a `Raycast3D` or `Overlap3D` sensor that
  touches its capsule (a cart's bumper, a trap), or its own sensor touching something; its
  `NavAgent3D` stops (`move_to` cleared). `samples/07_gameplay3d` does this with no code.
- **Getting up.** The entity's own transform does not move while the body falls: `root_position`
  is where the root bone is now and `settled` turns 1 once every part rests. Move the entity there
  (and play a get-up clip) before clearing `active`.
- **Collisions.** Parts collide with layers in `mask` (and with each other, but not with their
  neighbours). A character controller on the same entity would collide with its own ragdoll: put the
  ragdoll on a `layer` the controller ignores, or remove the controller while the body is limp. Ray
  and shape queries that hit a part report the ragdoll's entity.

Per-bone overrides go in the mesh's import sidecar, by bone name:

```json
{ "ragdoll": {
    "head":   { "radius": 0.11, "swing": 30, "twist": 40 },
    "spine":  { "mass": 3 },
    "sword":  { "exclude": true },
    "pinky":  { "include": true } } }
```

`exclude` makes a bone ride its parent; `include` gives one a body even when it moves no vertex
or is short; `radius` is in metres; `swing` and `twist` in degrees replace the component's; `mass`
weighs the bone's share of the body's mass (2 = twice its size's share). A bone the mesh does not
have fails the cook.

From code, `physics::World3D` builds ragdolls from parts described in world space
(`create_ragdoll(RagdollDesc3D, parts)`, `ragdoll_state`, `ragdoll_impulse`, `ragdoll_active`). The
replay hash covers them.

## 9.9 Vehicles

A car is a dynamic `RigidBody3D` (its body) with a `Vehicle3D` (`mce/kit/vehicles3d.h`, Jolt's
wheeled vehicle). The body's local −Z is forward. Four wheels hang from suspension points at
`±track` across, `wheel_y` up and `front_axle` / `rear_axle` along the body, in metres in its own
space; the front pair steers up to `max_steer` degrees, and `drive` picks the driven wheels (Rear,
Front, All).

- **Driving.** Write `throttle` (−1..1; negative reverses), `steer` (−1..1, positive turns right),
  `brake` and `handbrake` (0..1) from a game module, a sequence or MCP; or set `use_input = 1` and
  the player's Move action drives it (y throttle, x steer), Jump pulls the handbrake and TriggerL
  brakes. Throttle against the way the car is rolling brakes it first, then reverses.
- **Feel.** `engine_torque` (N·m) and `max_rpm` set the power; the gearbox is automatic. Suspension:
  `suspension_min` / `suspension_max` (travel), `suspension_frequency` (Hz, stiffer higher) and
  `suspension_damping`. `grip` scales the tyres' friction, which also depends on the ground's own
  `friction` (a `BoxCollider3D` at 1.0 is dry asphalt, 0.2 ice). `brake_torque` and
  `handbrake_torque` set the brakes; `max_tilt` (degrees) keeps an arcade car from rolling over
  (180 lets it).
- **Wheel models.** Put each wheel's model on its own root entity (a `MeshRef` whose axle is local
  X) and name it in `wheel_fl`, `wheel_fr`, `wheel_rl`, `wheel_rr`: it is placed on the wheel every
  tick, steering, spinning and following the suspension.
- **What it reports.** `speed` (m/s forward, negative reversing), `rpm`, `gear` (negative in
  reverse) and `wheels_on_ground`: enough for a speedometer, an engine sound's pitch or "airborne".
- **Handling (M-MOVE3D).** Each is off by default. `anti_roll` (N/m) puts a bar across each axle
  that keeps the body level in corners; keep it under the springs' own rate (about 25,000 N/m for a
  1,500 kg car at 1.5 Hz) or the car gets twitchy. `downforce` presses the car onto its wheels with
  `downforce` × speed² newtons. `gears` sets the number of forward gears, spread evenly from
  `first_gear` to `top_gear` (0 keeps the stock five), and the automatic gearbox shifts up above
  `shift_up_rpm` and down below `shift_down_rpm`.
- **Skids.** With `skid_slip` above 0, a grounded wheel going faster than 3 m/s skids when its
  sideways slip (rad) or its lock under braking passes it, and stops at 70% of it. Each start sends a
  `Skid3D` event (vehicle, wheel, slip, position), and `ParticleBurst`, `PlaySfx` or `ShakeTrigger`
  with `on: Skid` fire on it. `skidding` holds a bit per wheel (fl, fr, rl, rr) and `lateral_slip`
  the largest sideways slip.
- **Self-righting.** `self_right = 1` puts a car that has stayed tilted past about 53° (or upside
  down) and nearly still for `right_after` seconds back on its wheels, a metre up and facing the way
  it did. `rightings` counts the times.
- **AI drivers.** Chain `Waypoint3D` entities by `next` (the last back to the first for a loop) and
  give the car a `VehicleDriver3D` whose `path` is the first: it drives the car itself, writing
  `throttle`, `steer` and `brake`. It steers at a point `look_ahead` metres down the path, and slows
  for each corner so its sideways pull stays near `corner_accel` (m/s²), braking at `brake_decel`.
  It tops out at `max_speed`. A waypoint's `speed` caps the car there, and its `radius` sets how
  close counts as reached. Passing the first waypoint again completes a lap: `laps_done`,
  `last_lap` and `best_lap` count it, a `LapCompleted3D` event goes out, and effects with `on: Lap`
  fire. After `laps` laps (0 runs forever) it brakes to a stop and sets `finished`. `drive = 0`
  hands the car back. A tall car needs `anti_roll` to take the corners flat.

### Getting in and out (M-OPENWORLD C1)

Put a `VehicleSeat3D` (`mce/kit/seats3d.h`) on the car and a `Rider3D` on the character that walks
(a `CharacterController3D`). `seat` and `exit` are body-space points: where the driver sits and where
they step out. With the rider's `use_input = 1` the **Interact** action boards the nearest free seat
within `enter_radius` metres of the seat or the exit point; setting `enter = 1` does the same from a
module, a sequence or MCP (the engine clears it), and while seated it leaves again.

- **Boarding.** The rider's `CharacterController3D` is taken off (its settings are kept and come back on
  exit), so the capsule stops colliding, and the rider rides at `seat` every tick. The car gets
  `use_input = 1`: the player's Move, Jump and TriggerL drive it. A `VehicleDriver3D` on the car is
  switched off while someone sits. `locked = 1` keeps everyone out; a taken seat refuses a second rider.
- **Leaving.** Only slower than `max_exit_speed`. The rider steps out at `exit`, else the mirrored side,
  else above the roof, whichever has room; the car gets its handbrake, its input mode and its AI driver
  back. If the car or the rider is destroyed, the seat is freed.
- **Camera.** Name a `CameraFollow3D` entity in `camera` and it follows the car while seated and the
  rider afterwards.
- **Events.** `VehicleEntered3D` and `VehicleExited3D` (rider, vehicle).

A rider is not saved mid-ride: a reloaded rider that was seated gets a default `CharacterController3D`
when it steps out.

Changing a setting other than the driver's inputs, the wheel entities, `downforce`, `skid_slip` and
the self-righting rebuilds the vehicle. The
wheels find the ground with cylinder casts against whatever the body's layer collides with. From
code, `physics::World3D` has the same vehicles with any number of wheels and axles
(`create_vehicle(VehicleDesc3D)` with `anti_roll_bars` and shift points, `set_vehicle_input`,
`vehicle_state` with each wheel's `longitudinal_slip` and `lateral_slip`); the replay hash covers them.

## 9.10 Configuration

The host installs physics with the default `PhysicsConfig`: gravity −9.81 on Y, 8 velocity and 3
position iterations, warm starting on. A scene changes gravity with a `WorldSettings` component (`gravity_2d`; chapter 2,
Project Settings), applied at the first tick. Two v0.5 switches, `enable_sleep` and `block_solver`, are
off by default to keep older replays bit-identical; a new game that stacks many bodies benefits from
`block_solver = true` with 16 velocity iterations. Changing the config means a custom host today
(the player uses the defaults).

Budget: 2,000 bodies is the tested ceiling; a 20-body stack settles within a second.

## 9.11 Debugging

The editor viewport overlays colliders. In code, `World2D::debug_draw(sink, flags)` fills line and
circle primitives for a renderer you provide; `stats()` reports body and contact counts;
`validate_broadphase()` checks the tree. `hash_state()` is part of the determinism hash, so a
physics change shows up in `-headless` runs immediately.

## 9.12 Weapons, health and damage (M-OPENWORLD C2)

`mce/kit/combat3d.h` has three components. `Health3D` goes on anything that can be hurt: a character, a car,
a crate. `Weapon3D` fires from its entity. `Projectile3D` is a shot in flight.

- **Health.** `current` of `max`; `armor` (0..1) is the fraction of every hit absorbed; `regen` hp/s starts
  `regen_delay` seconds after the last hit. At zero it dies: `dead = 1` and a `Died3D` event (with the
  killer), its `CharacterController3D` stops listening to the input and its `NavAgent3D` stops. With
  `ragdoll_on_death = 1` its `Ragdoll3D` goes limp with the hit's push; with `destroy_on_death = 1` the entity
  is destroyed `linger` seconds later. A hit of at least `stagger_damage` (0 = off) knocks a living entity with
  a `Ragdoll3D` limp for `stagger_time` seconds. `invulnerable = 1` takes hits (events still fire) and loses
  nothing. `team` groups: a weapon whose owner shares the team hurts nothing here unless `friendly_fire`.
- **Weapons.** The shot leaves from the entity's position plus `muzzle` along `direction` (entity space, forward
  -Z): put the weapon on the camera, or on a child of the character at eye height, to shoot where it looks. It
  fires when `fire` is set (the engine clears it), or with `use_input = 1` on the **Fire** action (held with
  `automatic`, else one shot per press), at most `rate` shots a second. `pellets` rays fan out over a cone of
  `spread_deg` from a seed kept in the component, so a replay shoots the same shots. `magazine` > 0 limits
  the rounds; `reserve` refills it after `reload_time` seconds (the **Reload** action, `reload = 1`, or on its
  own when empty; `infinite_ammo` never uses `reserve`). `owner` is credited for the damage and never hit.
- **Hitscan and projectiles.** A hitscan ray reaches `range` metres. A `Projectile` weapon spawns a
  `Projectile3D` with `projectile_mesh` that flies at `projectile_speed`, falls under `projectile_gravity` times
  the world's gravity, and ends on its first hit or after `projectile_life` seconds; `splash_radius` > 0 makes
  it explode where it lands, hurting everything in the radius with damage falling off to the edge. `impulse`
  pushes dynamic bodies the shot hits.
- **Events.** `WeaponFired3D` (every pellet, for tracers and muzzle flashes), `WeaponHit3D` (a shot landed,
  hurt or not: impact marks and sounds), `Damage3D` and `Died3D`.
- **Other damage.** `apply_damage(world, target, DamageInfo{...})` hurts a `Health3D` from any source (a fall,
  a crash, an explosion, a script); call it from a `FixedUpdate_Pre` system or between ticks.

Add `Health3D` to a `Vehicle3D`'s entity and a crash or a rocket can wreck it: `Died3D` tells the game when.

## 9.13 Missions and objectives (M-MISSION)

`mce/kit/missions.h` makes a mission chain out of data. A `Mission` entity holds the state; each goal is an
`Objective` entity, and `Objective.next` points at the goal that follows. Scenes without these components are
unchanged, and everything lives in components, so a save in the middle of a mission resumes there.

- **Mission.** `objective` is the first goal, `actor` is who plays it (the player's character), `state` is
  `Idle`, `Active`, `Complete` or `Failed`. It starts when `start` is set to 1 (the engine clears it) or, with
  `start_on_load`, on its first tick. `time_limit` seconds after it started it fails; `fail_on_death` names an
  entity (the player, a person to protect) whose death fails it. `elapsed` and `ticks` count the time.
- **Objective kinds.** `ReachZone`: the actor is within `radius` metres of the target. `Kill`: `count` `Died3D`
  events of the target (with no target or tag, of anything). `Collect`: `count` `TriggerEnter3D` events of the
  target `Trigger3D`, as pickups. `Interact`: the Interact action is pressed within `radius` of the target.
  `Signal`: `count` `MissionSignal` events carrying the objective's `signal` name; a game system, a script or
  the test pushes them. `Timer`: `duration` seconds pass.
- **Target.** The `target` entity, or, when it is empty, the entity named `tag` in the loaded scene.
- **One at a time.** A mission has one active objective (`Mission.current`). When it is done the next one
  goes active and is looked at from the next tick; the last one completes the mission.
- **Events.** `MissionStarted`, `ObjectiveDone` (with the objective and its index), `MissionCompleted`,
  `MissionFailed` (`reason` 0 time limit, 1 `fail_on_death`, 2 an objective went missing). Use them to show
  text, play a sound or give a reward.

Time is counted in ticks, so the same run gives the same state hash. Entity references in a save are the
handles of the base scene a load rebuilds: give the entities a mission uses a `SaveKey` and spawn them from the
same scene.
