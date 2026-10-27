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

## 9.14 Missions and objectives (M-MISSION)

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
  `MissionFailed` (`reason` 0 time limit, 1 `fail_on_death`, 2 an objective went missing, 3 a script called `mission.fail`). Use them to show
  text, play a sound or give a reward.

Time is counted in ticks, so the same run gives the same state hash. Entity references in a save are the
handles of the base scene a load rebuilds: give the entities a mission uses a `SaveKey` and spawn them from the
same scene.

**Example: `samples/10_missions`.** A flat field, a player (`CharacterController3D` with input, a `Health3D` and a
hitscan `Weapon3D` that the Fire action drives) and three one-objective `Mission` entities, each with a bare
`MapMarker` so the active goal shows on the HUD's `Minimap`: reach `Zone A`, kill the `Target` (a `Health3D`), then
reach `Zone B` within 2.5 s (`time_limit`). `assets/scripts/missions.luau` starts mission 1 after a short briefing and
starts mission 2 and 3 when `mission.status` says the one before is `complete`; a script cannot read the mission
events, so it polls the state (and so sees a completion up to a tick after the kit did). It keeps what it has seen in
`ScriptVars` and logs `mission 2 complete at tick 82`, which `ctest -R sample_missions` pins: the three completions,
a delivery that runs out of time (`MissionFailed`), a save written in the middle of mission 3 that resumes at the
same tick, and the same state hash on a second play. The scene is data and a script: no C++.

## 9.15 Map markers and the minimap (M-MISSION)

`mce/kit/map_markers.h` puts things on a map, and a `Minimap` widget in a `.mcui` document draws it. Scenes
without a `MapMarker` are unchanged: nothing runs unless a host asks for the markers.

- **MapMarker** (on any entity): `icon` (a region of the minimap skin's sprite set; 0 draws a flat square),
  `color`, `layer` (0..31), an optional `label`, `always_visible` and `follow_objective`. With no
  `follow_objective` the marker sits at its entity's position (x and z; the height is dropped).
- **Objective markers.** Add a bare `MapMarker` to an `Objective` entity: its target (the `target` entity, or the
  entity named `tag`) gets a marker while that objective is `Active`, and the marker disappears when the
  objective is done. Add one to the `Mission` entity: it follows the mission's current objective, one after
  the other, and goes away when the mission completes or fails, with nothing added per objective.
  `follow_objective` names an `Objective` or a `Mission` explicitly, so a marker can live anywhere else.
- **Reading them.** `kit::collect_map_markers(world, out)` lists the shown markers in entity order;
  `kit::map_tracked_view(world, "Player", pos, heading)` gives the tracked entity's place. A custom host copies
  them into the widget with `Document::set_minimap_view`.
- **Fed automatically.** The stock player does that for you: every frame, before a visible `UiScreen` document
  updates, each of its `Minimap` widgets gets the shown markers and the view of the entity named by its `track`
  (default `Player`; a name nobody has leaves the arrow at the origin). A scene needs only the `MapMarker`s and a
  `.mcui` with a `Minimap`; a document without one costs nothing. In split-screen every Minimap follows its own
  `track` name, so name the players' entities apart and give each HUD its own `track`.

The widget is `"kind": "Minimap"` with a `"minimap"` object (`UiMinimap`):

```json
{ "name": "mini", "kind": "Minimap", "at": [1, 0, 200, 200],
  "minimap": { "track": "Player", "radius": 60, "zoom": 1, "rotate_with_heading": false,
               "clamp_to_rim": false, "background": [0.05, 0.07, 0.06, 0.75], "layer_mask": 4294967295 } },
{ "name": "map", "kind": "Minimap", "fill": true,
  "minimap": { "full_map": true, "world_min": [-500, -500], "world_max": [500, 500] } }
```

- `radius` metres reach the rim at `zoom` 1; the visible radius is `radius / zoom`. The map is the circle
  inscribed in the widget's rect, so a square widget is the usual choice.
- North is up (`-z`, east right) unless `rotate_with_heading` is on; then the tracked entity's heading is up. The
  heading is the yaw about +Y in radians, 0 facing north.
- A marker beyond the radius is hidden, unless `clamp_to_rim` is on or the marker is `always_visible`: it then
  sticks to the rim along the line from the centre.
- `full_map` shows the rectangle `world_min` .. `world_max` (x, z of the north-west and south-east corners),
  north up, with every marker at its place.
- `layer_mask` picks the marker layers (bit n = layer n). The tracked entity is an arrow in the style's `accent`
  colour; `background` is the rect's colour.
- Drawing uses the ordinary 2D quads, nothing else. `ui::minimap_project` (`mce/ui/minimap.h`) is the pure
  function behind it, from a world position, the tracked position, the heading, the radius and the widget rect to
  a widget-space point with `inside` and `on_rim`; `minimap_project_full` is the full-map one.

## 9.16 World clock (M-DAYNIGHT)

`mce/kit/world_clock.h` is a clock for the time of day. One `WorldClock` entity per scene holds it; a scene
without one is unchanged. The sun follows it with no script: while `drive_environment` is 1 (the default) the
system writes the game's `Environment3D.time_of_day` every tick (the field the 3D extract reads; set the scene's
`Environment3D` as usual, `time_of_day` there is then replaced at run time).

- **Fields.** `hour` (0..24) is the start value and then the current hour; write it to set the time. `day_length_seconds`
  (default 1200) is the real time for 24 hours, `time_scale` multiplies the speed (1 = as set, 0 stops it, a
  negative value counts as 0), `paused` stands still, and `day` counts the midnights passed (read-only).
- **Exact time.** The day is a whole number of ticks and the clock adds an integer number of sub-steps per tick, so
  the hour after N ticks has no float drift: at 60 Hz a 60 s day shows 6.0 after exactly 900 ticks, and a 1200 s day
  moves one hour every 3000 ticks. The state is in the component (`phase`, `phase_den`), so a save mid-day resumes
  on the same tick and the same inputs give the same state hash. Changing `day_length_seconds` keeps the time of day.
- **Events.** `HourPassed {hour}` (the hour that began, 0..23) once per hour boundary and `DayPassed {day}` at
  midnight, in time order, pushed in the fixed update before the 3D extract reads the environment.

## 9.17 Day cycle (M-DAYNIGHT)

`mce/kit/day_cycle.h` colours the day. One `DayCycle` entity per scene (next to the `WorldClock` of 9.16, which
installing it also installs) holds keyed curves over the hour; a scene without one is unchanged. Every tick the
system reads `Environment3D.time_of_day` (the clock writes it) and writes the game's `Environment3D`: `sun_color`,
`sun_intensity`, `ambient_sky`, `ambient_ground`, `fog_color`, `fog_density` and `exposure`. With no clock
(`time_of_day` below 0) or `enabled` 0 it writes nothing.

- **Keys.** Reflected components have no arrays, so a `DayCycle` has 8 key slots as plain fields, `k0_*` to `k7_*`,
  and `key_count` (default 5) says how many are used. A slot holds its hour (`kN_hour`, 0..24) and the value of
  every curve at that hour: `kN_sun`, `kN_sun_i`, `kN_sky`, `kN_ground`, `kN_fog`, `kN_fog_d`, `kN_exp`. Slots
  may be in any order; the system sorts them by hour.
- **Default preset.** An empty `DayCycle` has five keys that already look right: night at 0:00, dawn at 6:00
  (warm sun, intensity 1.0), noon at 12:00 (the stock environment: intensity 2.0, white sun), dusk at 18:00 (orange sun,
  intensity 1.0) and night at 21:00 (blue moonlight, intensity 0.05, exposure 1.2).
- **Interpolation.** Linear between neighbouring keys; the curve wraps at midnight (after the last key it runs to
  the first key a day later). At a key's hour the values are exactly the key's. One key is a constant.
- **Determinism.** The same hour gives the same values, and the system only writes the resource, so replays and the
  state hash are stable. `evaluate_day_cycle(cycle, hour)` is the pure function behind it.

## 9.18 Timed lights (M-DAYNIGHT)

`mce/kit/timed_light.h` switches lamps and lit windows with the clock. Put a `TimedLight3D` on an entity that has a
`PointLight3D`, a `SpotLight3D` and/or a `MeshRef` (installing it also installs the world clock and day cycle); it
follows the hour of the first `WorldClock`. Without a clock, or without the component, nothing is written.

- **Window.** `on_hour` (default 18.5) and `off_hour` (default 6) may wrap midnight. `fade_minutes` (default 20) is
  in game time. The ramp (0..1) rises linearly from `on_hour` to `on_hour + fade`, stays 1 until `off_hour`, then
  falls linearly to 0 at `off_hour + fade`. `on_hour == off_hour` means always on. With the defaults a lamp is off at
  noon, half on at 18:40, fully on at 19:30 and off again at 06:20.
- **What is written.** Light `intensity` is the authored value times the ramp (the authored value is captured at
  the first tick into the read-only `light_scale` / `spot_scale`, saved with the scene, so it never drifts), and
  `enabled` is 0 while the ramp is 0, so an off lamp costs nothing. `MeshRef.emissive_tint` becomes
  `emissive_off + (emissive_on - emissive_off) * ramp` (defaults 0 and 1) on all three channels; use `emissive_on`
  above 1 to feed bloom. Colours and the rest of the `MeshRef` are not touched.
- **Events.** `DuskStarted` (payload `hour`: the `on_hour`) and `DawnStarted` (the `off_hour`) are pushed once when
  the clock passes that hour. The first `TimedLight3D` in ECS order stands for the scene (it remembers the last clock
  hour in `last_hour`). Setting the clock by more than 6 hours is a jump, not a crossing, and fires nothing.

## 9.19 Weather state (M-WEATHER)

`mce/kit/weather.h` keeps the weather as data. One `Weather` entity per scene (the first in ECS order drives; installing
it also installs the timed lights, day cycle and world clock); a scene without one is unchanged. It maintains the
component and an event, and modulates the `Environment3D` resource (see "Environment" below).

- **Setting it.** `target` is `clear`, `overcast`, `rain`, `storm` or `snow`; write it at run time to change the
  weather. `current` is the kind in effect (read-only). `transition_seconds` (default 30) is one fade.
- **Blending.** `intensity` (0..1, read-only) is `fade / fade_den`, an exact ratio of ticks: a 30 s transition at
  60 Hz is 1800 ticks, so intensity is 0.5 after 900. Clear has intensity 0. A change of kind fades the old kind
  out to 0, switches `current` (and pushes the event) and fades the new kind in; a change to clear only fades out.
- **Wind.** `wind` = 0.1 + (kind wind - 0.1) * intensity, with the kind wind of `current`: clear 0.1, overcast 0.3,
  rain 0.5, storm 1.0, snow 0.2.
- **Wetness.** While `current` is rain, storm or snow, `wetness` rises with the intensity (fully wet after 60 s at
  intensity 1); otherwise it falls to 0 over `dry_seconds` (default 120).
- **Random schedule.** `auto_change_seconds` > 0 writes `target` every that many seconds from a deterministic
  integer hash of (`seed`, `counter`) modulo 5 and counts `counter`. No float RNG state, no wall clock; the
  counter and the timer are saved, so a save mid-run resumes on the same pick.
- **Event.** `WeatherChanged {from, to}` (kinds as numbers 0..4) once when `current` changes.
- **Determinism.** All state is in the component in integer ratios, so replays, saves and the state hash are exact.
- **Environment.** After the state advances, `kit_weather` sets `sun_intensity`, `sun_color`, `ambient_sky`,
  `ambient_ground`, `exposure`, `fog_density` and `fog_color` from the base (the day cycle's value, or the authored one)
  to the kind's target by `intensity`: `env = base + (target - base) * intensity`. At intensity 1:

  | kind | sun_intensity | sun_color | ambient (x, to grey, tint) | exposure | fog_density | fog_color (to, mix) |
  |---|---|---|---|---|---|---|
  | clear | x1 | - | x1 | x1 | +0 | - |
  | overcast | x0.35 | 0.4 to grey | x0.85, 0.5 | x1.0 | +0.002 | (0.60, 0.60, 0.62), 0.4 |
  | rain | x0.2 | 0.5 to grey | x0.7, 0.6 | x1.1 | +0.006 | (0.50, 0.52, 0.55), 0.6 |
  | storm | x0.08 | 0.5 to grey | x0.5, 0.5, tint (0.85, 0.9, 1.05) | x1.2 | +0.012 | (0.30, 0.32, 0.36), 0.7 |
  | snow | x0.4 | 0.3 to grey | x1.05, 0.3, tint (0.95, 1.0, 1.08) | x1.0 | +0.008 | (0.85, 0.88, 0.92), 0.7 |

  "To grey" mixes the colour toward its luminance. The component remembers what it wrote (`env_applied`) and the
  base (`env_base`, both read-only): a field that differs from what was written was rewritten by a day cycle or an
  edit, and becomes the new base, so nothing compounds. At intensity 0 the base is written back: clear weather is
  bit-identical to no `Weather`, and clearing the weather restores the authored values. Wind is exposed as
  `Weather.wind` for scripts and the precipitation step; `Foliage3D` wind is not changed by the weather itself (see "Wind sway").
- **Wet surfaces.** `mce/kit/wet_surfaces.h` adds an optional `WetSurfaces` entity (one per scene, registered with
  the weather) that decides how the wet world looks. When the scene has both a `Weather` and a `WetSurfaces`, the
  renderer gets the wetness `Weather.wetness * strength` (default strength 1) with `darken` (0.5, the albedo a
  fully wet dielectric loses), `wet_roughness` (0.15), `puddles` (0.5, the share of up-facing ground that collects
  puddles at full wetness) and `puddle_scale_m` (4 m). A scene without a `WetSurfaces` (or without a `Weather`)
  stays dry and renders exactly as before; `strength` 0 is dry too. The Low quality tier keeps the darkening but
  drops the puddles, and `r.wet_surfaces 0` switches wet surfaces off.
- **Wind sway.** `mce/kit/wind_sway.h` adds an optional `WindSway` entity (one per scene, registered with the weather)
  that makes foliage sway with `Weather.wind`. When the scene has both a `Weather` and a `WindSway`, every `Foliage3D`
  and `TerrainFoliage3D` is drawn with `wind_amplitude` and `wind_flutter` times `1 + (gain - 1) * wind`
  (`amplitude_gain` 4, `flutter_gain` 3), `wind_frequency` times `1 + (frequency_gain - 1) * wind` (1.5), and
  `wind_gust` plus `gust_gain * wind` (0.6, clamped to 0..1). At wind 0 the authored look is unchanged, the foliage
  components are never written, and a scene without a `WindSway` (or without a `Weather`) renders exactly as before.
- **Snow cover.** `mce/kit/snow_cover.h` adds an optional `SnowCover` entity (one per scene, registered with the
  weather) that lets snow settle on up-facing surfaces. While `Weather.current` is snow its saved, read-only `cover`
  (0..1, kept as the integer `snow_q` out of 2^24) rises by the snow intensity, reaching full cover after
  `build_seconds` (90) at full intensity; in any other weather it melts away over `melt_seconds` (120). When the scene
  has both a `Weather` and a `SnowCover`, the renderer gets `cover * strength` (default strength 1) with `coverage`
  (0.8, the share of up-facing ground that whitens at full cover), `noise_scale_m` (3 m) and `wind_bias` (0.5). A scene
  without a `SnowCover` (or without a `Weather`) stays bare and renders exactly as before. The Low quality tier
  keeps the whitening but drops the noise breakup, and `r.snow_cover 0` switches snow off.

## 9.19b Thunder that arrives late (M-RAINFX)

`mce/kit/thunder_delay.h` adds an optional `ThunderDelay` entity (one per scene, registered with the weather) that
turns each lightning strike into a thunderclap that reaches the listener late, the way sound lags light. Installing
precipitation adds its system `kit_thunder_delay`. Set `clip` to a cooked Sfx, `speed_mps` (343) and `volume` (1,
the gain at zero distance). Every `LightningStruck` starts the clip `ceil(distance / speed_mps / dt)` ticks after the
strike, so a faster `speed_mps` brings the thunder sooner. The event carries no position, so the distance is its
`thunder_delay_ticks` at 343 m/s (30..300 ticks, about 170 to 1700 m). The sound is quieter the farther the strike:
linear gain `volume / (1 + distance / 100 m)`, never below -60 dB. A scene needs a 3D camera (the listener), a
`clip` and the audio system, or nothing sounds. Up to 8 strikes can be in flight; a ninth is dropped. The pending
strikes are saved, so a save in the middle of a storm resumes with the thunder still on its way. A scene without a
`ThunderDelay` sounds exactly as before.

## 9.19c Rain splashes (M-RAINFX)

`mce/kit/rain_splash.h` adds the `RainSplash` component, which makes drops splash where they land while it rains. Put
it on an entity that also has a `ParticleEmitter3D` and a `LocalTransform` (the same shape as `Precipitation3D`);
installing precipitation adds its system `kit_rain_splash`. Its fields are rate (1, scales how many splashes land per
tick), height_m (0.15, how high a splash jumps), life_seconds (0.25, how long it lasts), radius_m (25, how far from the
primary 3D camera they land) and seed (1). While the weather is rain or a storm with intensity above 0, every tick
fires one burst of `round(4 * intensity * rate)` particles at a seeded random point within radius_m of the camera,
at the entity's own height: set the entity's y to the floor height, since the system moves only x and z. It never
creates entities, and the only state it keeps is a saved burst counter, so a save mid-storm resumes on the same
splash pattern. In clear weather, snow, or a scene with no camera the emitter is switched off and nothing spawns; a
scene without a `RainSplash` is unchanged.

## 9.19d Wind direction (M-WINDDIR)

`mce/kit/wind_direction.h` adds the `WindDirection` component, one optional entity per scene (the first in ECS order
drives; registered with the weather) that gives the wind a heading. `degrees` (0) is the heading, 0 = +x and
counter-clockwise seen from above, so 90 points along +z; `drift_degrees_per_minute` (0, clamped to +-3600) turns it
slowly, negative the other way. The system `kit_wind_direction` keeps the heading as the saved integer `angle_q` (in
1/65536 degree) and a saved remainder, so a long drift loses nothing, 360 degrees of drift returns to the start
exactly and a save in the middle of a turn resumes on the same state. With a drift of 0 the heading is simply
`degrees`, so editing it takes effect on the next tick; with a drift the heading starts at `degrees` and turns from
there. The snow lee side and the foliage sway follow the heading: the snow cover's `wind_dir` becomes the heading's
unit vector (a scene without a `WindDirection` keeps +x), and every `Foliage3D` and `TerrainFoliage3D` batch turns its
authored `wind_direction` by the heading relative to +x, so foliage authored along +x sways along the scene wind and one
authored along another axis keeps its offset (authored +z at heading 90 sways along -x). The foliage turn needs a
`Weather` as well, like the wind sway; without one the batches keep their authored direction. It touches the batch
fields only, so changing the heading never re-scatters. The falling rain and snow particles of `Precipitation3D`
(section 9.20) lean along the heading too (their direction and gravity sideways part follows the same unit vector, so
heading 90 slants them toward +z and 180 toward -x), again only with a `Weather`. A scene without a `WindDirection`
is unchanged, its particles still lean toward +x.

## 9.19a Rain ripples (M-RAINFX)

An optional `RainRipples` entity (one per scene, the first in ECS order is read; it is registered with the weather)
makes rings spread on the puddles while it rains. Its fields are strength (1, scales the ripples), density (0.5, how
many drops land per puddle area), speed (1, the ring animation rate) and ring_width (0.15, the ring thickness), all
saved with the scene. The renderer gets ripples only when the scene has a Weather, a WetSurfaces and a RainRipples and
the weather intensity is above 0: the strength is the field times that intensity, so the rings fade in and out with
the rain. A scene missing any of the three stays ripple-free and renders exactly as before. The rings show on
puddles only, so the puddles of the wet surfaces (9.19) decide where. The Low quality tier and `r.rain_ripples 0`
switch them off.

## 9.19e Clouds (M-CLOUDS)

An optional `CloudLayer` entity (one per scene, the first in ECS order is read; it is registered with the weather)
draws a procedural cloud layer in the sky that follows the weather. Its fields are strength (1, scales the cover),
fair_coverage (0.35, the sky cover in clear weather), overcast_coverage (0.9, the cover at full overcast), scale_m (900,
the size of one cloud cell in metres), speed_scale (1, scales the drift speed) and height_m (2000, the height of the
layer above the camera), all saved with the scene. The renderer gets clouds only when the scene has a Weather and a
CloudLayer: the cover goes from fair_coverage toward overcast_coverage with the weather kind (clear stays fair,
overcast, rain and storm reach the full overcast cover, snow 0.9 of it) times the weather intensity, times strength.
The clouds drift along the scene `WindDirection` (section 9.19d; +x when the scene has none) at the weather's wind times
speed_scale, and the clock is the scene time, so a paused or replayed frame draws the same sky. A scene without a
`Weather` or a `CloudLayer` has no clouds and renders exactly as before. The Low quality tier and `r.clouds 0` switch
them off.

From Luau, `weather.cloud_strength()`, `cloud_fair_coverage()`, `cloud_overcast_coverage()`, `cloud_scale()`,
`cloud_speed_scale()` and `cloud_height()` read the fields and `weather.set_clouds{...}` writes them (section 27.3), for
example `weather.set_clouds{overcast_coverage = 1, height = 1500}` for a lower, fuller deck in a storm. The `12_weather`
sample carries a `CloudLayer`, so its sky clouds over as the forecast turns to rain.

## 9.19f Rain and wind ambience (M-WEATHERAUDIO)

An optional `WeatherAudio` entity (one per scene, the first in ECS order drives; it is registered with the weather)
makes two looping sounds follow the weather. You place two ordinary `AudioSource` entities (`loop`, `play_on_start`,
a bus, an optional `fade_s`, and your own rain and wind clips) and point `rain_source` and `wind_source` at them; the
engine ships no clips, and a source without a clip stays silent. The system `kit_weather_audio` only writes their
`gain_db`: it never starts or stops a source and never writes the `Weather`. The rain level is the weather kind
(clear 0, overcast 0.15, rain 1, storm 1, snow 0.35) times the weather intensity, and the wind level is `Weather.wind`
(0..1, so a calm wind is a quiet bed). A level moves toward its target by a fixed step per tick, so a bed fades in and
out over `fade_seconds` (default 3, 0..60) as a straight ramp, and the gain is `rain_db` or `wind_db` (the gain at
full level, default 0, -60..12) plus 20 log10 of the level, floored at -60 dB (silent weather is exactly -60). The two
levels are saved with the scene (`rain_q` and `wind_q`, integers out of 2^24), so a save in the middle of a fade
resumes on the same ramp. A null, destroyed or non-source entity is skipped, `enabled` 0 switches the system off, and a
world with no audio, no `Weather` or no `WeatherAudio` is left untouched, so a scene without one hashes and sounds as
before. A scene with one hashes differently by design, because the source gains are scene data.

From Luau, `weather.rain_level()` and `weather.wind_level()` read the two bed levels (0 to 1, read-only),
`weather.audio_rain_db()`, `audio_wind_db()`, `audio_fade()` and `audio_enabled()` read the authoring fields and
`weather.set_audio{rain_db=, wind_db=, fade=, enabled=}` writes them (section 27.3), for example
`weather.set_audio{fade = 8}` for a slower change of the beds. The `12_weather` sample carries a `WeatherAudio` and two
clip-less looping `AudioSource` entities, so it stays silent; give them your own rain and wind clips to hear the beds.

## 9.19g Strikes with a place (M-LIGHTNINGBOLT)

An optional `LightningBolt` entity (one per scene, the first in ECS order drives; it is registered with the weather)
gives every lightning strike a position, so thunder arrives after the real distance. Without it a strike only has a
delay of 30 to 300 ticks. With it, `kit_lightning` picks a bearing and a distance from the camera for each strike
(`min_range_m` 150 and `max_range_m` 900, 10..5000, swapped if inverted) from the `Lightning` seed and strike count,
on its own hash stream, so the strike times of a scene do not move. The point lies on the plane `ground_y` around the
primary camera as it was at the strike, and stays there. The `LightningStruck` event then carries `has_pos`, `pos` and
`distance_m`, and the delay is `ceil(distance_m / 343 / dt)` ticks (no 30..300 clamp). `ThunderDelay` uses that
distance for its delay, at its own `speed_mps`, and for its gain; an event without a position is handled as before.
The last strike is saved on the component (`strike_x`, `strike_z`, `strike_dist_m`, `strike_id`, `active`), so a save
during a flash resumes on the same state. `enabled` 0 switches positions off; the other fields (`height_m`, `width_m`,
`segments_log2`, `jitter`, `fork`, `light_intensity`, `light_radius_m`) author the bolt that is drawn.

While a flash lasts (`active` above 0, the weather is a Storm, `enabled` 1 and the quality tier is not Low) the scene
draws the bolt: a jagged line from `height_m` above the strike down to it, made of `2^segments_log2` emissive box
segments (`jitter` is the sideways wander as a fraction of a segment, 0 is a straight line; `width_m` is the width at
100 m and grows with the distance so far bolts stay readable) and, with `fork` 1, one shorter side branch. The shape is
fixed by the strike number, so a given strike always looks the same, and it is drawn with the existing mesh path (no
new shader), bright enough to bloom. It is brightest on the first flash tick and fades with the flash; on some strikes,
chosen by the strike number, the second tick flashes again. With `light_intensity` above 0 a blue-white point light
sits halfway up the bolt, `light_radius_m` wide. Drawing never changes the scene, so saves and state hashes are
unaffected. A scene without a `LightningBolt` draws exactly as before.
A scene without a `LightningBolt` hashes and sounds as before.

From Luau, `weather.bolt_min_range()`, `bolt_max_range()`, `bolt_ground_y()`, `bolt_height()`, `bolt_width()`,
`bolt_segments()`, `bolt_jitter()`, `bolt_fork()`, `bolt_enabled()`, `bolt_light()` and `bolt_light_radius()` read the
authoring fields, `weather.bolt_distance()`, `weather.bolt_pos()` (the ground point as `x, z`) and
`weather.bolt_active()` (flash ticks left) read the last strike (read-only), and
`weather.set_bolt{min_range=, max_range=, ground_y=, height=, width=, segments=, jitter=, fork=, light=, light_radius=, enabled=}`
writes the authoring (section 27.3), for example `weather.set_bolt{light = 3000}` for a flash of light at the strike. The
`12_weather` sample carries a `LightningBolt` (light off) on its Lightning entity, so its storm strikes have a place and
draw a bolt; its script logs the last strike every 150 ticks.

## 9.19h Wind gusts (M-WINDGUST)

An optional `WindGust` entity (one per scene, the first in ECS order drives; it is registered with the weather) makes
the wind come in gusts: a calm gap, a rise, a fall, then the next gap, all drawn from the `seed` so the same seed always
gives the same gusts. Each gust has its own gap (`min_gap_s` 2 to `max_gap_s` 8), rise (0.4 to 1.5 s), fall (1 to 3 s)
and height (`min_peak` 0.4 to `max_peak` 1); an inverted pair is swapped and a value outside its range is clamped. The
level of the gust is saved as integers (`gust_q` out of 2^24 plus the counters of the current gust), so a save in the
middle of a gust resumes on the same curve and a change of the tick rate cannot make it jump. A `WindGust` never changes
`Weather.wind`; it only defines the gusted wind `wind * (1 + strength * gust)` (`strength` 0.6, 0 to 3, clamped to 1),
which later steps feed to foliage sway, rain slant, cloud drift, the snow lee bias and the wind sound. A calm wind
gusts proportionally less (a clear 0.1 stirs, a storm at 1.0 is already at the clamp). `affects` is a bit mask of those
consumers (1 sway, 2 rain, 4 clouds, 8 snow, 16 audio, all by default) and `enabled` 0 switches the gusts off. Foliage
sway (bit 1) already follows the gusted wind (the batch wind fields scale with it; nothing is re-scattered) and so does
the snow lee bias (bit 8: `wind_bias * (1 + strength * gust)`, clamped to 0..1). Cloud drift is not gusted: the render
side computes the cloud offset as `speed * time`, so a changing speed would make the layer jump (bit 4 is unused).
Rain and snow slant (bit 2) and the wind sound (bit 16) follow the gusted wind too: `kit_precipitation` and
`kit_weather_audio` run after `kit_wind_gust` in the same tick. While a gust applies, the wind fed to the emitter is
rounded to steps of 1/64 so the emitter changes in steps, and the wind sound moves toward the gusted level through its
usual fade (`fade_seconds`). A scene without a `WindGust` is unchanged, bit for bit.

From Luau, `weather.gust_strength()`, `gust_min_gap()`, `gust_max_gap()`, `gust_min_rise()`, `gust_max_rise()`,
`gust_min_fall()`, `gust_max_fall()`, `gust_min_peak()`, `gust_max_peak()`, `gust_seed()`, `gust_affects()` and
`gust_enabled()` read the authoring fields, `weather.gust_level()` (the current level, 0 to 1) and
`weather.effective_wind()` (the gusted wind with every consumer bit applied, so it ignores `affects`; `weather.wind()`
stays the smooth value) are read-only, and
`weather.set_gust{strength=, min_gap=, max_gap=, min_rise=, max_rise=, min_fall=, max_fall=, min_peak=, max_peak=, seed=, affects=, enabled=}`
writes the authoring (section 27.3), for example `weather.set_gust{seed = 7, strength = 1.2}`. It never adds a
`WindGust`: a scene without one raises an error. The `12_weather` sample carries a `WindGust` on its Weather entity and
its script logs the level and the effective wind every 150 ticks. In its storm (wind near 1) the clamp hides the gusts; in the rain
and snow they show.

## 9.19i Morning mist (M-GROUNDMIST)

An optional `GroundMist` entity (one per scene, the first in ECS order is read; it is registered with the weather) turns
the hour of day and the wetness left by rain into a mist level from 0 to 1. The level is thickest at `peak_hour` (6),
builds up over `rise_hours` (3) before it and burns off over `burn_hours` (3) after it, as a smooth curve that wraps over
midnight. It is `base_humidity` (0.25, the mist on a dry day) plus `wet_gain` (0.75) times `Weather.wetness`, times
`strength`, and rain, storm and snow take it away (`weather_damp` 1: overcast 0.8, rain 0.2, storm 0, snow 0.4 of the mist
at full weather intensity; 0 ignores the weather). Without a `Weather` the wetness counts as 0 and nothing damps. The hour
is `Environment3D.time_of_day`, written by a `WorldClock`: a scene with no clock (`time_of_day` -1) has no mist. The level
is a pure function of these values and nothing of it is saved, so a save in the middle of a dawn gives the same level.
The remaining fields (`density`, `thickness_m`, `base_height_m`, `exp2_density`, `color`, `enabled`) say how the mist is
drawn. Each frame the extract applies the level to its own copy of the fog, never to `Environment3D`. With
`volumetric_fog` on (and a tier that keeps it) the level adds `density` to the volumetric density, raises the height
falloff toward `1 / thickness_m`, puts the base at `base_height_m` and mixes the albedo toward `color` by half the level,
so the mist lies low and the sun shines through it as shafts. With volumetric fog off, or on the Low tier, it adds
`exp2_density` times the level to the exp2 fog and mixes the fog colour toward `color`; that fog is uniform in height.
Exactly one of the two is used per frame, the weather's own fog adds to it, and a level of 0, `enabled` 0 or no
`GroundMist` leaves the frame bit for bit as it was. The mist is a smooth haze, not banks with edges.

From Luau, `weather.mist_strength()`, `mist_peak_hour()`, `mist_rise()`, `mist_burn()`, `mist_base_humidity()`,
`mist_wet_gain()`, `mist_weather_damp()`, `mist_density()`, `mist_thickness()`, `mist_base_height()`,
`mist_exp2_density()`, `mist_color()` (three numbers `r, g, b`) and `mist_enabled()` read the authoring,
`weather.mist_level()` (0 to 1) is the level the mist has now from the current `time_of_day` and the first `Weather`
(read-only; 0 when the scene has no clock), and
`weather.set_mist{strength=, peak_hour=, rise_hours=, burn_hours=, base_humidity=, wet_gain=, weather_damp=, density=, thickness=, base_height=, exp2_density=, color={r, g, b}, enabled=}`
writes the authoring (section 27.3), for example `weather.set_mist{peak_hour = 7, color = {0.8, 0.8, 0.85}}`. It never
adds a `GroundMist`: a scene without one raises an error. A scene needs a clock (`WorldClock`) for any mist, so set one
before expecting `mist_level()` above 0. The `12_weather` sample carries a `GroundMist` on its Weather entity and starts
its clock at 5:00 (a 600 s day, 1 h = 1500 ticks) so the mist builds toward its 6:00 peak while the weather turns to
rain; its script logs the level every 150 ticks.

## 9.19j Moon and phases (M-NIGHTSKY)

An optional `Moon` entity (one per scene, the first in ECS order is read; it is registered with the weather) turns the
`WorldClock` day count and hour into a moon phase, a direction and a brightness. The phase is 0 at a new moon, 0.5 at a
full moon and wraps at 1: it follows the clock over `cycle_days` (29.53, so a short in-game day still cycles through all
the phases) starting from `phase_offset` (0.5, a full moon on day 0 hour 0), or is pinned by `freeze_phase` (0 to 1; a
negative value follows the clock), which is handy for a screenshot. The lit fraction of the disk is 0 at new and 1 at
full. The moon sits `phase * 24` hours behind the sun on the sun's own arc, so a full moon rises at sunset and a new moon
is up with the sun, and `inclination_deg` (5) tilts its path off that arc. The level, from 0 to 1, is the lit fraction
times a smooth rise as the moon climbs above the horizon, times a weather factor (overcast 0.5, rain 0.3, storm 0.15,
snow 0.5 at full weather intensity; no `Weather` means no damping). The hour is `Environment3D.time_of_day`, written by a
`WorldClock`: a scene with no clock (`time_of_day` -1) has no moon, and the day is 0 without one. Phase, direction and
level are pure functions of these values and nothing of them is saved, so a save at midnight gives the same phase. The
`light_*` fields are read by the 3D extract. At night `Kit3DExtract` hands the scene's one directional light to the
moon: the sun keeps it while `sw * sun_intensity` is at least `mw * light_intensity`, where `sw` is a smooth 0 to 1
ramp of the sun's elevation from -0.05 to 0.1 and `mw` is `(1 - sw)` times the moon level; otherwise the light takes
the moon's direction, `light_color`, `light_intensity * mw` and `light_shadows`, and the ambient gets a small cool lift
(`light_color * mw * 0.02`). Only the extract's copy of the light changes, `Environment3D` is never written, and a scene
with no `Moon`, `enabled` 0, no clock or a zero level (new moon, moon below the horizon) is unchanged. Pair the `Moon`
with a `DayCycle` so the sun is dim when the hand-off happens: a sun that stays bright below the horizon is handed over
at full strength and pops. The sky fields feed the night sky: `Kit3DExtract` fills `render::NightSky` (8.5 "Night sky") from the `Moon`, the clock,
the clouds and the fog. `moon_dir` is the direction toward the moon (the light's travel direction negated, the same
value the light uses when the moon holds it), `moon_phase` the phase, `moon_cos_size` is `disk_size`, `moon_intensity`
is `disk_intensity`, `moon_color` is `light_color`, and `star_density`, `star_brightness` and `star_twinkle` are the
`Moon`'s; `star_time` is the scene clock in seconds. The whole layer is scaled by
`night_fade = (1 - smoothstep(-0.1, 0.1, sun_elevation)) * (1 - cloud_coverage) * (1 - smoothstep(0, 0.05, fog_density))`:
0 by day, 1 on a clear unfogged night, and lower under a cloud deck (the coverage the clouds block derived, 0 when the
tier or `r.clouds` turns the clouds off) and in fog (the exp² fog density the extract hands on, weather and morning mist
included, 0.05 and up hides the sky entirely; the volumetric fog is not counted). Rain or a storm dim the sky through
the `CloudLayer` coverage and the weather's fog density, not through `moon_level`, which only dims the light. The night
sky is not set (density and intensity 0, as a scene without a `Moon`) with no `Moon`, `enabled` 0, no clock
(`time_of_day` below 0) or an equirect `sky_texture` on the environment (the texture wins). It goes through
`render::apply_quality`: the Low tier keeps the moon and drops the stars. While the moon holds the directional light the
extract also drops the sky's own sun disk (it would sit on the phased moon; the extract's copy of `Sky.sun_intensity`
becomes 0, `Environment3D` is not written); by day and whenever the sun holds the light the disk is as before. The
disk is drawn larger than the real moon so its phase can be read.

From Luau, `weather.moon_phase_offset()`, `moon_cycle_days()`, `moon_freeze_phase()`, `moon_inclination()`,
`moon_light_intensity()`, `moon_light_color()` (three numbers `r, g, b`), `moon_light_shadows()`, `moon_disk_size()`,
`moon_disk_intensity()`, `moon_star_density()`, `moon_star_brightness()`, `moon_star_twinkle()` and `moon_enabled()`
read the authoring, and `weather.set_moon{phase_offset=, cycle_days=, freeze_phase=, inclination=, light_intensity=, light_color={r, g, b}, light_shadows=, disk_size=, disk_intensity=, star_density=, star_brightness=, star_twinkle=, enabled=}`
writes it (section 27.3), for example `weather.set_moon{freeze_phase = 0.1, star_density = 1}` for a crescent over a full
star field. `inclination` is the `inclination_deg` field in degrees. The four read-only values come from the same
helpers the extract uses, on the current `Environment3D.time_of_day`, the first `WorldClock`'s day (0 without a clock) and
the first `Weather`: `weather.moon_phase()` (0 new, 0.5 full), `weather.moon_illumination()` (0 to 1),
`weather.moon_direction()` (three numbers `x, y, z`, the unit direction the moonlight travels, so negate it for the
direction toward the moon) and `weather.moon_level()` (0 to 1; 0 when the scene has no clock or the moon is below the
horizon). `set_moon` never adds a `Moon`: a scene without one raises an error. A scene needs a clock for any moon, so
set one before expecting `moon_level()` above 0. The `12_weather` sample carries a default `Moon` on its Weather entity
and, since its clock starts at 5:00 on day 0, the moon is a full one in the western sky that sinks through the dawn
(level 1.000 at tick 0, 0.469 at tick 450 with the rain damping it, 0.000 from tick 1500); the sample was not moved to
a night hour so the mist keeps its dawn numbers, and the script logs `moon:` every 150 ticks.

## 9.19k Water surface data (M-WATER)

An optional `Water` entity (one per scene, the first in ECS order is read; it is registered with the weather) describes
one horizontal water plane: the calm `level_y`, the `half_extent` of the camera-following square, `wave_count` (1 to 8)
Gerstner waves of up to `amplitude` metres and `wavelength` metres, `steepness` (0 to 1), a main direction `wind_dir_deg`
(0 is +x, counter-clockwise from above, like `WindDirection`) with `wind_follow`, `speed_scale`, the two tint colours,
`reflectivity`, `roughness` and the shore foam. This step only stores the data and ships the pure helpers
`water_height` and `water_normal` in `mce/kit/water.h`; nothing draws or reads the component yet. The wave set comes from
fixed tables, all waves repeat together after 64 s so the time wraps seamlessly, `steepness` is shared out so the surface
never folds over, and a zero `amplitude` gives exactly the level and an up normal. Nothing is stored between ticks and
the weather, clock and wind are never written.

## 9.19l Drawing the water (M-WATER)

The renderer draws the plane of 9.19k in the main pass, after the opaque geometry and before the sky and the
transparent draws, as a flat square that follows the camera (normals only: the surface never moves up and down, so its
silhouette stays flat and close to the horizon it shows no parallax). The waves shade it with the same Gerstner sum
`water_normal` returns; each wave fades to flat where it is under a few pixels long, so the far sea is smooth and the sun
path widens instead of sparkling. It reflects the sky through the sky's own code: the gradient or sky texture, the
clouds, the moon and the stars, and the horizon haze, more toward the horizon (fresnel), blurred by `roughness` and
scaled by `reflectivity`; the sun (or, at night, the moon holding the light) adds a glint. Geometry is not reflected.
The body runs from `shallow_color` to `deep_color` over `depth_scale_m` metres of water between the surface and the
ground seen through it, and `foam_amount` lays foam within `foam_width_m` of that ground along the view (the shore line on
a steep shore; on a gentle beach the band widens where the water is shallow). Both read the opaque depth of a half-size
prepass, which the renderer also runs for SSAO; with the SSAO, SSR and contact-shadow resources all off in the renderer's
creation settings the water has no depth to read: it takes the deep tint (lighter near the camera) and draws no foam. The
Low tier draws a flat plane with the same deep tint and no foam (and so does a calm sea authored with `amplitude` and
`foam_amount` both 0: the renderer cannot tell the two apart); `r.water 0` draws no plane. Fog and mist cover the water
as they cover the ground. A scene without a plane renders exactly as before. Transparent things under the surface (glass,
particles) are hidden by it, and with temporal anti-aliasing the moving waves can smear a little, since the plane is not
in the motion pass.

## 9.19m Water from Luau (M-WATER)

Scripts read and tune the water of 9.19k through the `water` table (section 27.3). `water.height_at(x, z)`,
`water.normal_at(x, z)` (three numbers) and `water.submerged(x, y, z)` evaluate the same wave sum the renderer draws, at
the scene time `mce.tick() * mce.dt` and with the main direction the renderer uses (the `WindDirection` heading when
`wind_follow` is above 0 and the scene has one, else `wind_dir`), so a float or a boat that follows `height_at` bobs on
exactly what is on screen; the engine applies no buoyancy force, the script moves the object. `submerged` is true when `y`
is below the surface there. `water.set{...}` and the getters (`water.level()`, `water.shallow_color()` and so on, named
like the set keys) act on the first `Water`; every set key is optional and clamped to the component's range, an unknown
key, a value that is not a number or a scene without the component raises an error that names the call, and nothing is
written. The setter never adds the component. The queries work with `enabled` 0 too, and they pause with the scene time.
The `13_water` sample (`ctest -R water`) is the worked example: a default `Water` off a sloped sand island, and a crate whose
script sets its `y` to `water.height_at(x, z)` and tilts it to `water.normal_at(x, z)` every tick, logging `water:` every 150 ticks.

## 9.20 Precipitation and lightning (M-WEATHER)

`mce/kit/precipitation.h` turns the weather into rain, snow and lightning (installing it installs the weather). Both
components are opt-in; scenes without them are unchanged.

- **Rain and snow.** Put `Precipitation3D` on an entity that also has a `ParticleEmitter3D` and a transform. Each tick
  the system `kit_precipitation` sets the emitter from the first `Weather`'s `current`, `intensity` and `wind`: rain
  `rate = rain_rate * intensity` (a storm x1.6; default `rain_rate` 3000), fast thin bluish streaks falling with
  `rain_speed` (18), slanted by the wind; snow `rate = snow_rate * intensity` (800), slow large white flakes drifting
  sideways with the wind. The slant and drift point along the scene `WindDirection` (section 9.19d; +x when the scene
  has none). Clear and overcast set `emitting` 0 and `rate` 0 (live particles play out). The values are
  pure functions of the weather and the component, written only when they change, so a quiet scene is untouched; the
  fields it sets (rate, gravity, direction, speeds, lifetimes, colours, sizes, box, `max_particles`, `additive` 0)
  replace the authored ones. The exact table is in the header. Without a `Weather` nothing is written.
- **Following the camera.** With `follow_camera` 1 (default) the entity moves to `height` (12) metres above the primary
  camera, so the spawn box (`area` 14 m half extent) always surrounds the player. With no camera the transform is left
  alone. Keep the entity out of any hierarchy.
- **Lightning.** One `Lightning` per scene (the first drives). Only while the weather is a storm it picks a gap between
  `min_gap_seconds` (6) and `max_gap_seconds` (20) from a deterministic hash of (`seed`, `counter`), then strikes:
  the sun and ambient light are multiplied by a boost that falls linearly from `flash_boost` (4) to 1 over
  `flash_ticks` (8). The boost is a field of the `Weather` (`flash_mul`) applied by `kit_weather` on top of the
  weathered environment, so nothing drifts: after the flash the environment is bit-identical to a run without
  lightning. It reaches the environment one tick after the strike. Outside a storm there are no strikes.
- **Event.** `LightningStruck {thunder_delay_ticks}` once per strike, 30 to 300 ticks (from the hash), so audio can play
  thunder after the delay.
- **Determinism.** `counter`, `timer` and `remaining` are saved; a save mid-storm resumes on the same strikes and the
  state hash is stable.

## 9.13 Wanted level and police (M-OPENWORLD C3)

`mce/kit/wanted3d.h` has two components. `Wanted3D` goes on the offender (the player): it holds the `heat`,
the `level` made from it (0..`max_level`, `heat_per_level` heat for each) and how crimes count. `Police3D` goes
on a police unit (give it a `Health3D` with the police `team`, a `Weapon3D` and a `NavAgent3D` as needed).

- **Crimes.** A `Damage3D` or `Died3D` (9.12) whose instigator is the offender (or hangs under it) adds heat when
  the victim's `Health3D::team` is `civilian_team` (`hit_civilian`, `kill_civilian`) or `police_team`
  (`hit_police`, `kill_police`). A hit on police is always known. A civilian hit counts only when a living
  `Police3D` within `witness_range` of the victim has a clear line to it (`require_witness = 0`: always).
  `report_crime(world, offender, heat, position)` adds heat from a script or another system; both push
  `Crime3D`. Crimes from combat events are counted one tick after the hit. `clear_wanted` ends the chase (a pay-and-spray). `WantedChanged3D` fires when the level changes.
- **Cool-down.** Once no unit has seen the offender for `cooldown_delay` seconds the heat falls by `decay`
  a second; a new crime restarts the wait.
- **Units.** A unit answers from `min_level`. It looks for the nearest offender within `sight_range` with a clear
  line. Seen, it is `Pursue`, or `Attack` within `attack_range` from `attack_level` up. Lost, it is `Search`
  for `search_time` seconds at the last place it saw the offender, then `Idle`. In `Attack` it turns to face the
  target and fires its own `Weapon3D` (`aim`); with `drive_nav` a `NavAgent3D` follows the target, walks to the
  last known place, and stands still in `Attack` and `Idle`.
- **Behavior trees.** A unit with an `AiBrain` gets the blackboard keys `wanted` (number), `police_state` (number,
  the `PoliceState` index), `target` (entity, cleared when none) and `last_known` (vec3). Turn `drive_nav` and
  `aim` off and let the tree do the moving and shooting with the stock `move_to` and `look_at` tasks.

- **Arrests (M-ARREST).** A unit that sees the offender within `arrest_range` (2.5 m), at a wanted level of at most
  `arrest_max_level` (1), with the offender moving no faster than `arrest_speed` (1.5 m/s), builds
  `arrest_progress`; after `arrest_time` (2 s) the offender is arrested: `Arrested3D { offender, by, position }`
  is pushed and the wanted level cleared. The progress falls back whenever the offender moves off, runs or the unit
  loses it. What an arrest costs the player (weapons, a respawn at the station) is the game's, in a handler of that
  event. `arrest_range = 0` or `arrest_max_level = 0` make a unit that only chases and shoots.

- **Reinforcements (M-ARREST 2).** A unit with `dispatch_range` above 0 (default 0, off) that is idle, whose `min_level`
  the offender has reached and which is within that range of the offender's last known place, goes there and Searches,
  even without seeing it. Place units with a higher `min_level` and a long range around the map: they stay idle at level 1
  and arrive as the level rises.

- **Police cars (M-ARREST 3).** `drive_car` 1 on a unit that has a `Vehicle3D` (with `use_input` 0) drives it at the target:
  Pursue and Attack at the offender, Search at the last known place, Idle stops. It steers at the point, slows in turns
  and inside 12 m, and brakes to a stop inside 4 m. There is no path following, so a wall between the car and the target
  stops it. A roadblock is a car with `drive_car` 0 placed across the road.

- **Busted and wasted (M-WASTED).** Put `Respawn3D` (`delay`, default 3 s) on the player and `RespawnPoint3D` on a few
  entities (`kind` Hospital or Station). After the player dies it wakes up at the nearest Hospital, after an arrest at the
  nearest Station (any point when the wanted kind is missing; where it stands when there is none), with full health and
  no wanted level, and `Respawned3D { entity, reason, position }` is pushed (`reason` 1 busted, 2 wasted). Fines and lost
  weapons are the game's, on that event. With captions installed (the stock player does) the wait also shows "WASTED" or
  "BUSTED" through the subtitle view; `caption` 0 turns that off.

Not in this step: spawning new units, and cars that follow roads.
