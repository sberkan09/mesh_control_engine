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
**2D joints** are a component too (M-JOINT2D). Put a `Joint2D` on a body's entity and set `connected` to the
other body's entity (a joint to the world uses a static body). `kind` is `Distance` (a rod; `min_length < max_length`
makes a rope; `hertz > 0` makes a spring, `damping_ratio` 0..1) or `Revolute` (a hinge at `anchor`, in this entity's
frame; `limits` keeps the angle within `lower` .. `upper` degrees from the rest angle, `motor` drives it at
`motor_speed` degrees per second up to `max_motor_torque`). Distance joints run from `anchor` to `connected_anchor`
(the connected entity's frame); a length below zero means the distance when the joint was built. The joint is built
the first tick both bodies exist; changing a setting rebuilds it from the current poses, except `motor_speed`,
`lower` and `upper`, which update in place. `enabled` 0 removes the joint and keeps the component. `length_now` and
`angle_now` are read-only readouts.

**Pushing 2D bodies** also needs no code (M-PHYSDRIVE2D). `LaunchVelocity2D` sets a body's velocity (`linear` in m/s,
`angular` in degrees per second) once, the first tick the body exists, for a thrown ball or a bullet; it is ignored on a
static body. `Impulse2D` gives a one-shot push (`impulse` in N*s, world space, at `point` in the body's frame): the tick
it is applied `impulse` goes back to zero, so writing a new value pushes again. `ConstantForce2D` applies `force` (N,
world space) every tick while the component is there, for wind, a conveyor or a thruster. Impulses and forces act on
dynamic bodies only, and NaN or infinity counts as zero. `LaunchVelocity2D.applied` is kept in save games (not in
scenes), so loading a save does not launch the body again. Gravity is a scene setting: `WorldSettings.gravity_2d`.

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

### Horn and headlights

`mce/kit/vehicle_extras.h` gives the car the player drives a horn and a headlight switch. Put `VehicleHorn3D` (`clip`,
`gain_db`, `min_gap_s`, `attenuation_id`) and/or `VehicleLights3D` on a car that has a `VehicleSeat3D`. While a `Rider3D`
with `use_input` sits in it, the `Horn` action plays the clip once at the car (3D, never faster than `min_gap_s`; `honks`
counts them) and the `Lights` action cycles `VehicleLights3D.mode`: `Auto` (the day cycle decides, `TimedLight3D`), `On`
(the car's `SpotLight3D` at its authored intensity) and `Off`. Setting `VehicleHorn3D.honk` to 1 honks once without
input (scripts, tests). Traffic and police cars have no rider with `use_input`, so they never respond. `samples/09_openworld`
binds Horn to G (stick press) and Lights to L (D-pad right).

### Hit marker

`mce/kit/hit_marker.h`: put `HitMarker3D` on the shooter (the player). A shot of its weapon that lands on another living
entity with a `Health3D` sets `HitMarker3D.timer` to `duration` seconds (`hits` counts them); a death it caused sets
`kill_timer`. Both count down to 0, so a HUD widget bound to `HitMarker3D/timer` or `HitMarker3D/kill_timer` with
`range` `[0.001, 10, 0]` shows only while the marker is up. `hit_clip` and `kill_clip` play a 2D tick on a hit and a ding on a kill. `samples/09_openworld` draws a white x and a red X on the crosshair.

### Facing the movement

`mce/kit/face_velocity.h`: put `FaceVelocity3D` on a model entity (usually the child that holds the character mesh) and
name the entity with the `CharacterController3D` in `source`. While that character moves faster than `min_speed`, the
model turns about Y towards its ground velocity at `turn_rate_deg` degrees a second; standing still keeps the heading, a
seated rider is ignored. `yaw_offset` (pi) is for a mesh that faces +Z. Only the rotation is written.
`samples/09_openworld` puts it on the `Player model`.

### Speed and gear on the HUD

`mce/kit/drive_readout.h`: put `DriveReadout3D` on the entity that holds the `Rider3D` (the player). While it is seated
in a car, `kmh`, `gear` (-1 reverse, 0 neutral, 1.. forward) and `rpm` mirror the car's `Vehicle3D`; on foot they read
-1000, so a HUD widget bound to one with a `range` starting above that (for example `[-0.5, 1000, 0]`) shows only in a car.
`samples/09_openworld` draws "N KM/H" and "GEAR n" at the bottom right.

### Impact sparks

`mce/kit/impact_fx.h`: put `ImpactFx3D` (`count`, `lift`) on an entity with a `ParticleEmitter3D` that has `rate` 0. Each
`WeaponHit3D` moves it to the hit point (lifted along the surface normal) and, one tick later, bursts `count` particles
there. One emitter serves every shooter; two hits in one tick show the last. `samples/09_openworld` has a spark
emitter that the player's shots light.

### Fall death

`mce/kit/fall_death.h`: put `FallDeath3D` (`min_y`) on an entity that has a `Health3D`. When its y drops below `min_y` it
takes lethal damage through `apply_damage`, so `Damage3D` and `Died3D` fire and a `Respawn3D` on it wakes it at the
nearest hospital. A hole in the ground or a launch off the map ends in WASTED instead of an endless fall; an
invulnerable entity keeps falling. `samples/09_openworld` gives the player `min_y` -40.

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
a crate. `Weapon3D` fires from its entity, or from another entity's pose when `aim_from` is set (a third-person gun on the player that aims along the camera: the shot leaves at the camera's position plus `muzzle` and points where it looks; if that entity is destroyed the weapon uses its own pose). `Projectile3D` is a shot in flight.

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
- **With the atmosphere sky** (`Environment3D.atmosphere` 1; the renderer's copy only, `Environment3D` is not written):
  the DayCycle's ambient wins over the atmosphere's own (which is dim in the shade). As the sun sets a gradient in the
  current `fog_color` fades in under the physically based sky (from 3° above to 6° below the horizon; the zenith at 15 %
  of the fog colour), because that sky goes black within a few degrees under the horizon while the dusk keys last hours;
  by night it is the fog-coloured night sky behind the stars. When a `Moon` takes the light the sky keeps following the
  sun itself, so the hand-off does not show in the sky. With `sky_ibl` 1 the image-based diffuse light is scaled so that
  its upward irradiance matches the DayCycle's `ambient_sky` (the sky keeps its colour and direction, the keys set how
  bright the shade is). `samples/09_openworld` uses all of this with `auto_exposure` clamped to 0.8..2.

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

## 9.19n Aurora data (M-AURORA)

The optional `Aurora` component (`mce/kit/aurora.h`, one per scene, the first in ECS order is read) draws a curtain aurora in the night sky: a few soft ribbons of light, green at the lower edge and violet toward the top, that sway slowly, shimmer with vertical ray striations and pulse in brightness along an arc of the sky. Scenes without it are unchanged bit for bit. It is authored, never derived: `latitude_deg` (68, -90..90) is the scene's magnetic latitude, because the engine has no globe and no place on it, and `gate_low_deg` (55) and `gate_high_deg` (65) open the effect smoothly between them on the absolute latitude (the sign only mirrors the side; an inverted or zero pair falls back to the defaults). A scene near the equator therefore shows nothing, and a script that sets `latitude_deg` as the player travels fades the aurora in and out with no pop. `night_only` (1) scales the whole effect by the night fade, so it never shows by day; the fade comes from the sun's elevation on the `WorldClock`, so a scene with no clock has no night and shows nothing unless `night_only` is 0, which forces it (for a screenshot, or for a scene with a fixed sky).

The rest shapes the curtains: `strength` (0..4, the brightness gain), `curtain_count` (1..6 ribbons), `base_height` (0.18) and `height_span` (0.35), the lower edge and the reach as sines of the elevation, `azimuth_deg` (0 = +z, counter-clockwise from above) and `width_deg` (10..180) of the arc, `speed` (0..4, in quarter steps so the pattern repeats every 64 s), and the linear `low_color` (green) and `high_color` (violet). With `base_height` 0.6 and `height_span` 0.8 the arc reaches the zenith and pinches into it, leaving a dark dot at the exact zenith pixel; keep the top below that. The curtains fade out toward the horizon (the same haze that dims the stars), clouds and fog drawn in front dim or hide them (an overcast sky dims the level, a dense volumetric fog hides it), and the water reflects them through the shared sky function. They are a function of the view direction only, like the stars and the moon: there is no parallax, walking or a floating-origin rebase does not move them, and the aurora does not light the ground, the water or the fog. A scene whose sky is an equirect `sky_texture` shows no aurora (the texture wins), and reflection probes, the IBL bake and the irradiance volume do not see it. The renderer's `r.aurora 0` and the Low quality tier turn it off (section 8.4). The pure helpers `aurora_latitude_gate`, `aurora_level`, `aurora_wrap_time`, `aurora_curtain_height`, `aurora_curtain_intensity` and `aurora_band_color` compute the gate, the gain, the curtain edges, the brightness and the colour by height from the authoring, the night fade, the cloud coverage and the time alone; a closed gate, a zero `strength` or `enabled` 0 gives exactly 0. There is no system and nothing is stored or hashed beyond the authored fields.

Scripts read and tune it through the `weather` table (section 27.3), next to the moon: `weather.set_aurora{...}` and the getters (`weather.aurora_latitude()`, `weather.aurora_low_color()` and so on, the set keys' names with an `aurora_` prefix) act on the first `Aurora`, and moving `latitude` across the gate is how a scene fades the aurora in and out as a player travels (`samples/12_weather` walks it from 45 to 90 degrees and logs `aurora:` every 150 ticks). `weather.aurora_gate()` is the latitude gate (0 to 1) and `weather.aurora_level()` is the brightness gain the renderer gets now, both from the same kit helpers as the renderer's feed: the level uses the night fade of the current hour (`Environment3D.time_of_day`, the sun's elevation; 0 with no clock, so only `night_only` 0 shows) and the cloud coverage of the first `Weather` and `CloudLayer`, before the quality tier. Every set key is optional and clamped to the component's range, an unknown key, a value that is not a number, a colour that is not three numbers or a scene without the component raises an error that names the call, and nothing is written. The setter never adds the component, and the queries read the authoring even when `enabled` is 0 (the level is then 0).

## 9.19o Rainbow data (M-RAINBOW)

The optional `Rainbow` component (`mce/kit/rainbow.h`, one per scene, the first in ECS order is read) draws a rainbow in the sky when the rain has stopped and the sun is out: a primary bow about 42 degrees from the anti-solar point (the point opposite the sun, so the bow stands in front of you when the sun is behind you), red on the outside and violet on the inside, and a fainter secondary bow about 51 degrees out with the colours reversed and a darker gap (Alexander's band) between them. Scenes without it are unchanged bit for bit. Authoring: `strength` (1, 0..4, the brightness gain), `band_width` (1, 0.5..2, scales the angular width of both bows), `secondary` (1, 0..1, the strength of the secondary bow; 0 draws none), `min_wetness` (0.25, 0..1, the ground wetness below which no bow shows), `max_sun_elevation_deg` (42, 10..60, the sun height above which the bow sinks below the horizon and fades out over 10 degrees) and `enabled`.

| Gate | Opens when | Closes when |
|---|---|---|
| Sun | the sun is above the horizon (full from 3 degrees) | night, or the sun above `max_sun_elevation_deg` (a noon scene shows none) |
| Wetness | the ground wetness (`Weather.wetness`) is above `min_wetness` | the ground has dried (a dry ground is always 0) |
| Rain | the weather is not Rain or Storm; while it rains the level scales by `1 - rain intensity` | it is raining hard |
| Cloud | clear sky; the level scales by `(1 - cloud coverage)^2` | a thick deck |

The level is `enabled * strength * sun gate * wet gate * (1 - rain) * (1 - cloud)^2`, so the bow follows the drying of the ground, not a timer: `Weather` has no "just stopped raining" event. It needs a `Rainbow` and a `Weather` in the scene; a scene with a `WorldClock` takes the sun's height from the hour, a scene without one from its fixed `sun_direction` (a sun fixed below the horizon shows none by design). The pure helpers `rainbow_level`, `rainbow_intensity` (the two bows at an angle from the anti-solar point) and `rainbow_color` (the seven-colour spectral ramp) are fixed-order f32 functions; NaN and inverted ranges fall back to the defaults, and a zero level, a zero `strength` or `enabled` 0 gives exactly 0. There is no system and nothing is stored or hashed beyond the authored fields.

The renderer draws the bows as additive sky emission after the aurora and under the clouds and the fog (a deck in front dims them, a dense fog hides them), fading out toward the horizon, and the water reflects them through the shared sky function. They depend on the view direction and the sun direction only, like the stars: no parallax, so walking or a floating-origin rebase does not move them, and the bows are not tied to a place on the ground. A scene whose sky is an equirect `sky_texture` shows no rainbow (the texture wins), the bow does not light the ground, and reflection probes, the IBL bake and the irradiance volume do not see it. The renderer's `r.rainbow 0` and the Low quality tier turn it off (section 8.4).

Scripts read and tune it through the `weather` table (section 27.3), next to the aurora: `weather.set_rainbow{...}` and the getters (`weather.rainbow_strength()`, `weather.rainbow_max_sun_elevation()` and so on, the set keys' names with a `rainbow_` prefix) act on the first `Rainbow`. `weather.rainbow_level()` is the gain the renderer gets now, from the same `rainbow_level` helper as the renderer's feed: the sun elevation of the current hour (`Environment3D.time_of_day`, else its `sun_direction`), the first `Weather`'s wetness and, while it rains or storms, its intensity, and the cloud coverage of the first `Weather` and `CloudLayer`, before the quality tier; it is 0 without a `Weather`. Every set key is optional and clamped to the component's range, an unknown key, a value that is not a number or a scene without the component raises an error that names the call, and nothing is written. The setter never adds the component, and the queries read the authoring even when `enabled` is 0 (the level is then 0). `samples/12_weather` logs `rainbow:` every 150 ticks: the level is 0 by night and in the rain and rises after sunrise over the still wet ground. `weather.rainbow_level()` is the level before the quality gate, so it reads the cloud cover before the renderer's tier is applied and does not see `r.env` overrides; the renderer's own level can be lower.

## 9.19p Meteor data (M-METEOR)

The optional `Meteor` component (`mce/kit/meteor.h`, one per scene, the first in ECS order is read) draws shooting stars in the night sky: short bright streaks that cross the sky in a fraction of a second, each a glowing head with a fading tail along a great-circle arc, a few in view at a time. Scenes without it are unchanged bit for bit. Authoring: `rate` (4, 0..30, meteors per minute across the whole visible sky; 0 draws none), `brightness` (1, 0..4), `length` (1, 0.5..2, scales the streak), `speed` (1, 0.5..2, a faster meteor lasts a shorter time), `min_darkness` (0.6, 0..1, the night fade below which none shows), `night_only` (1; 0 shows them by day too), `seed` (0..65535, so two scenes differ) and `enabled`. `rate` is an average: the sky is cut into 5 s slots, each holding rate / 12 meteors (the fractional part is a seeded draw per slot), so a rate of 4 gives one every 15 s on average but not on a metronome. A meteor starts in the first half of its slot and always ends inside it, and the pattern repeats exactly every 60 s (12 slots), whatever `speed` and `length` are. The pure helpers turn a slot and an index into a streak with an integer hash (start direction, travel direction, start time, duration and a fast-rise, slow-fall envelope) and `meteor_level` folds the night fade, the cloud cover and the rain into the gain; a zero `rate`, `brightness`, level or `enabled` 0 gives exactly 0. There is no system and nothing is stored or hashed beyond the authored fields.

The level is `enabled * night gate * (1 - cloud)^2 * (1 - rain)`. The night gate is the night fade smoothed from `min_darkness` to 1, so no meteor shows by day or at dusk; `night_only` 0 skips it. The night fade comes from the sun's elevation on the `WorldClock`, so a scene with no clock has no night and shows nothing unless `night_only` is 0. A thick cloud deck or rain (the first `Weather` while it rains or storms, scaled by its intensity) closes the level.

The renderer draws the streaks as additive sky emission after the stars, the aurora and the rainbow and under the clouds and the fog (a deck in front dims them, a dense fog hides them), and the water reflects them through the shared sky function. They depend on the view direction and the time only, like the stars: no parallax, so walking or a floating-origin rebase does not move them, and the moonlight is not read. A scene whose sky is an equirect `sky_texture` shows no meteors (the texture wins), they do not light the ground, and reflection probes, the IBL bake and the irradiance volume do not see them. The renderer's `r.meteor 0` and the Low quality tier turn them off (section 8.4).

Scripts read and tune it through the `weather` table (section 27.3), next to the rainbow: `weather.set_meteor{...}` and the getters (`weather.meteor_rate()`, `weather.meteor_min_darkness()` and so on, the set keys' names with a `meteor_` prefix) act on the first `Meteor`. `weather.meteor_level()` is the level the renderer gets now, from the same `meteor_level` helper as the renderer's feed: the night fade of the current hour (`Environment3D.time_of_day`; 0 with no clock, so with no clock only `night_only` 0 shows), the cloud coverage of the first `Weather` and `CloudLayer`, before the quality tier, and the first `Weather`'s rain intensity while it rains or storms. Every set key is optional and clamped to the component's range (`seed` is rounded down), an unknown key, a value that is not a number or a scene without the component raises an error that names the call, and nothing is written. The setter never adds the component, and the queries read the authoring even when `enabled` is 0 (the level is then 0). `samples/12_weather` logs `meteor:` every 150 ticks: the run starts at 5:00, so the level is non-zero only before sunrise (about tick 1500), damped by the cloud cover and the rain, and 0 from then on. `weather.meteor_level()` is the level before the quality gate, so it reads the cloud cover before the renderer's tier is applied; the renderer's own level can be lower.

## 9.19q Season data (M-SEASONS)

The optional `Season` component (`mce/kit/season.h`, one per scene, the first in ECS order is read) turns the `WorldClock` day count into a position in the year, the season phase, and pure helpers turn the phase into a foliage tint, a foliage density scale and a snow bias. Scenes without it are unchanged bit for bit, and there is no system: the phase is a pure function of the authoring, the clock's `day` and its `hour`, so nothing is stored or hashed beyond the authored fields. Authoring: `year_days` (12, 1..3650, days in a year; the short default shows all four seasons in minutes), `start_phase` (0.1, 0..1, the phase at day 0 hour 0, so a scene can start in any season), `spring_tint` (0.75 1.05 0.7), `autumn_tint` (1.35 0.7 0.35) and `winter_tint` (0.7 0.72 0.75) (linear multipliers of the authored foliage colour at the centre of each season; summer is white, the authored colour), `winter_density` (0.4, 0..1, the share of copies left in the deepest winter), `snow_cover_bias` (0.5, 0..1, the extra snow cover drawn at the centre of winter), `cold_bias` (0.25, 0..1, the shift of the snow lee bias), `strength` (1, 0..1, scales every effect; 0 is off exactly) and `enabled`.

The year is four equal quarters, spring `[0, 0.25)`, summer `[0.25, 0.5)`, autumn `[0.5, 0.75)` and winter `[0.75, 1)`, each centred in its quarter. `season_phase` is `fract(start_phase + (day + hour / 24) / year_days)`, summed in f64 so a long run does not drift. The helpers blend between neighbouring centres with a smoothstep, so everything is continuous through the year and across the wrap from the end of winter to the start of spring: `season_foliage_tint` (the multiplier, white at the centre of summer), `season_foliage_density` (1 until late autumn, the winter density at the centre of winter, back to 1 at the next spring) and `season_snow_bias` (a cover floor and a lee shift, both 0 from the start of the year to the end of summer and largest at the centre of winter). A zero `strength`, `enabled` 0 and a missing component give white, 1 and 0 exactly. A scene with no `WorldClock` has no day count (the extract and the getter use day 0, hour 0), so its season is fixed at `start_phase`: a still scene picks its season with that one number.

What the renderer does with it, in `Kit3DExtract` and on the render-side copies only. The foliage tint is a multiplier of the authored colour: after the wind sway, each `Foliage3D` and `TerrainFoliage3D` batch's tint is multiplied component-wise by `season_foliage_tint` (alpha and `tint_variation` untouched), so summer is exactly the authored look, spring a lighter green, autumn orange and red, winter a bare grey, and a foliage kind that is already autumn-coloured shifts further. The authored `Foliage3D.tint` is never written, the wind fields are independent of it, and a day change moves the colour with no re-scatter. There is no per-entity opt-out yet, so evergreens and rocks change too. The density helper `season_foliage_density` exists in `season.h` but the extract does NOT thin the foliage, so winter is the grey tint, not bare branches: thinning was dropped in 19b because it would pass a shorter prefix of the cached copies, and `TerrainFoliage3D` copies are laid out row by row, so a prefix is a bare strip rather than an even thinning; a real thinning needs a per-copy keep test on the render side, a separate milestone.

The snow bias is a drawn floor, not a simulated target. Where the extract builds the snow cover (a `Weather` and a `SnowCover` exist), winter raises the drawn cover to at least `snow_cover_bias` times the `SnowCover` strength and shifts the lee bias by `cold_bias`, on the local copy before the quality tier (`r.snow_cover 0` still turns snow off). So a clear winter day shows frost, but `SnowCover.cover`, `Weather` and the Luau `weather.snow_cover()` getter still read the simulated value, nothing gameplay reads changes, and with no `SnowCover` or no `Weather` the bias does nothing.

Scripts read and tune it through the `weather` table (section 27.3), next to the meteor: `weather.set_season{enabled=, year_days=, start_phase=, spring_tint={r,g,b}, autumn_tint={r,g,b}, winter_tint={r,g,b}, winter_density=, snow_cover_bias=, cold_bias=, strength=}` and the getters `weather.season_<key>()` (the three tints return `r, g, b`) act on the first `Season`. `weather.season_phase()` is read-only, from the same `season_phase` helper on the first `WorldClock`'s day and hour (the fixed `start_phase` with no clock), 0..1. Every set key is optional and clamped to the component's range (tint channels 0..4); an unknown key, a value that is not a number, a tint that is not a table of three numbers or a scene without the component raises an error that names the call, and nothing is written. The setter never adds the component. `samples/12_weather` carries a `Season` on the Weather entity with `year_days` 1 (its 600 s day would make the default 12-day year too slow to show) and logs `season:` (the phase and the tint) every 150 ticks: the phase climbs from late spring to the centre of summer over the run.

## 9.19r Water ice data (M-FREEZE)

The optional `WaterIce` component (`mce/kit/water_ice.h`, one per scene, the first in ECS order is read) turns the `Season` winter weight into a freeze amount 0..1, and a pure helper turns the amount into an ice look for the water: calmer waves, a pale colour, a shorter depth scale, more roughness, less sky reflection and less shore foam. Scenes without it are unchanged bit for bit, and there is no system: the amount is a pure function of the authoring, the `Season` and the year phase, so nothing is stored or hashed beyond the authored fields. Authoring: `freeze_start` (0.5, 0..1) and `freeze_full` (0.9, 0..1), the winter weight at which ice starts to form and at which the water is fully frozen (an inverted pair is swapped, an empty range falls back to the defaults), `manual` (0, 0..1, a floor for the amount, so a scene with no `Season` can still freeze), `ice_color` (0.62 0.78 0.85, linear), `ice_roughness` (0.35, 0..1), `ice_reflectivity` (0.55, 0..1, the reflection of fully frozen water as a share of the authored one), `ice_depth_m` (0.4, 0.05..10, short so the tint is opaque), `calm` (1, 0..1, how far the waves are flattened at full freeze), `strength` (1, 0..1, scales every effect; 0 is off exactly) and `enabled`.

**The freeze amount and its two drivers.** `water_ice_amount` is `max(manual, smoothstep(freeze_start, freeze_full, winter weight))` times `strength`. The two drivers are the `Season` and the `manual` floor. The winter weight is the one `Season` uses (0 until phase 0.5, rising through late autumn to 1 at the centre of winter, 0.875, and back to 0 at the end of the year), so summer is exactly 0 and the ice forms in late autumn, is full through early winter and thaws before spring. `manual` is a floor, not a switch: a scene with no `Season` (or a script) sets it to freeze the water permanently, and with a `Season` it keeps the water at least that frozen all year. A `Season` with `enabled` 0 counts as no `Season`: the amount is the `manual` floor. `enabled` 0, `strength` 0 and a missing component give an amount of 0 and the identity look exactly. The phase comes from the first `WorldClock`'s day and hour exactly as in 9.19q; a scene with no `WorldClock` has day 0, hour 0, so its season is fixed at `start_phase` and the amount is constant (a still scene picks its season with that number, or uses `manual`).

**The ice look, field by field.** `water_ice_look` takes the amount `a` and returns the scales the renderer applies to the water; at `a` = 0 every one is the identity. `Kit3DExtract` applies them to the local `render::WaterSurface` before the quality tier:
- `amplitude`, `steepness` and `speed_scale` are multiplied by `1 - a * calm`: at full freeze with `calm` 1 the waves are flat and still; a lower `calm` leaves rough ice.
- `shallow_color` and `deep_color` are each mixed toward `ice_color` by `a`.
- `depth_scale_m` moves from the authored value toward `ice_depth_m` by `a`, so the tint becomes opaque and the sea floor is not seen.
- `roughness` becomes the larger of the authored value and `ice_roughness * a`.
- `reflectivity` is multiplied by `1 - a * (1 - ice_reflectivity)`, less sky reflection.
- `foam_amount` is multiplied by `1 - a`, so the shore foam fades first as ice forms and is gone when frozen.
The surface time, the wave count and wavelength, the wind direction, the level and the extent are untouched. Because the look goes in before `apply_quality`, `r.water 0` and the Low tier still win. There is no new shader or render field: the ice is a recolouring and a calming of the existing water, with no cracks, frost texture or refraction, and it needs a `Water` to draw anything.

**Drawn only.** The freeze is a rendering effect. `Water`, `Season` and `WorldClock` are never written, and gameplay still sees the authored waves: `water.height_at`, `water.normal_at` and `water.submerged` read the unfrozen `Water`, so a crate that bobs on the sea keeps bobbing on frozen water, and the shoreline (cut by the terrain depth test) does not advance. A script that wants the ice to matter reads `weather.water_frozen()` and stops the bobbing, blocks a boat or switches a skating rule itself.

Scripts read and tune it through the `weather` table (section 27.3), next to the season: `weather.set_water_ice{enabled=, freeze_start=, freeze_full=, manual=, ice_color={r,g,b}, ice_roughness=, ice_reflectivity=, ice_depth_m=, calm=, strength=}` and the getters `weather.water_ice_<key>()` act on the first `WaterIce`; the colour getter is `weather.water_ice_ice_color()` (the key `ice_color` after the `water_ice_` prefix) and returns `r, g, b`. `weather.water_frozen()` is read-only: the freeze amount 0..1 from the same `water_ice_amount` helper on the first `Season` at the first `WorldClock`'s day and hour (the manual floor with no `Season`, the fixed start phase with no clock), the same number the renderer draws. Every set key is optional and clamped to the component's range (colour channels 0..4); an unknown key, a value that is not a number, a colour that is not a table of three numbers or a scene without the component raises an error that names the call, and nothing is written. The setter never adds the component.

`samples/13_water` carries a `WaterIce` with defaults, a `Season` (30-day year, start phase 0.6) and a `WorldClock` (2 s day, so a 60 s year; `drive_environment` 0, so the clock does not move the fixed sun and there is no `DayCycle`), all on the Water entity, and logs `ice:` (the amount and the phase) every 150 ticks. At 60 ticks a second: the sea is open at tick 0 (late autumn is still below `freeze_start`), `ice: frozen` is about 0.544 at tick 450 as the ice forms, it is 1.000 from tick 750 through 900 (flat, pale, opaque, no foam), and it has thawed to open water by tick 1200 and stays open for the rest of the year. The `water:` lines of the crate are the ones the sample had before the ice.

## 9.19s Leaf fall data (M-LEAFFALL)

The optional `LeafFall` component (`mce/kit/leaf_fall.h`, one per scene, the first in ECS order is read) turns the `Season` autumn into a strength 0..1, and a pure helper turns the strength into the look of an emitter of drifting leaves that `Kit3DExtract` adds around the camera. Scenes without it are unchanged bit for bit, and there is no system: the strength is a pure function of the authoring, the `Season` and the year phase, so nothing is stored or hashed beyond the authored fields. Authoring: `autumn_start` (0.5) and `autumn_full` (0.6), where the leaves start to fall and reach full strength, `autumn_end_full` (0.65) and `autumn_end` (0.75), where they start to thin and are gone (all 0..1; an inverted pair is swapped, an empty one falls back to the defaults), `manual` (0, 0..1, a floor for the strength, so a scene with no `Season` can still drop leaves), `rate` (24, 0..500, leaves per second at full strength), `box_xz_m` (14, 1..100, the half extent of the spawn box around the camera) and `height_m` (7, 1..50, the spawn height above the camera), `size_m` (0.14, 0.02..1), `lifetime_s` (7, 1..30), `fall_speed` (0.9, 0..5, the terminal speed in m/s), `wind_follow` (1, 0..2), `leaf_color` (0.95 0.5 0.15, linear), `tint_follow` (1, 0..1, how far the colour follows the season foliage tint), `max_particles` (1024, 64..8192), `seed` (1), `strength` (1, 0..1, scales every effect; 0 is off exactly) and `enabled`.

**The autumn window and the manual floor.** `leaf_fall_amount` is `max(manual, window)` times `strength`. The `window` is a trapezoid of smoothsteps, rising over `autumn_start..autumn_full` and falling over `autumn_end_full..autumn_end` on the year phase, times the `Season` strength. It is 0 until phase 0.5, 1 from 0.6 to 0.65 (the centre of autumn is 0.625) and 0 again from 0.75, so winter, spring and summer are exactly 0 and the leaves thin out before the snow. `manual` is a floor, not a switch: a scene with no `Season` (or a script) sets it to drop leaves all year, and with a `Season` it keeps the amount at least that high outside autumn too, so a winter `manual` shows leaves over snow. A `Season` with `enabled` 0 counts as no `Season`: the amount is the `manual` floor. `enabled` 0, `strength` 0 and a missing component give an amount of 0 and the inactive look exactly. The phase comes from the first `WorldClock`'s day and hour exactly as in 9.19q; a scene with no `WorldClock` uses day 0, hour 0, so the season is fixed at `start_phase` and the amount is constant. A scene with a `Season` and no clock and a `start_phase` outside autumn drops nothing unless `manual` is set.

**The emitter look, field by field.** `leaf_fall_look` takes the amount `a`, the wind speed, the wind direction and the foliage tint and returns the emitter description; at `a` = 0 it is inactive and every field is zero. `Kit3DExtract` copies it into one `render::ParticleEmitter`:
- `rate` is `rate * a` leaves per second; the emitter has `burst` 0 and always emits while the amount is above 0.
- `direction` is the unit vector of (wind x, -1, wind z), where the wind vector is the horizontal wind direction times `w = wind * wind_follow` and `wind` is capped at 6 m/s so a storm cannot blow the leaves out of the box. At wind 0 the leaves fall straight down. `speed_min` is `0.5 * fall_speed + 0.25 * w` and `speed_max` is `fall_speed + 0.5 * w` (m/s); `spread` is 0.6 rad, wide so the leaves do not fall in lines.
- `gravity` is (0, -2, 0) and `drag` is 2 / `fall_speed`, so the terminal fall speed `gravity / drag` is `fall_speed` (a `fall_speed` below 0.05 is read as 0.05). The leaf starts at the launch speed and slows or speeds up toward it, a slow fall, not a drop.
- `size_start` and `size_end` are both `size_m` (the leaf keeps its size), and the lifetime is drawn between 0.7 and 1 times `lifetime_s`.
- The spawn box is `box_xz_m` half extent in x and z and no thickness in y, centred `height_m` above the camera position, and it follows the camera every frame. The leaves also fall in a clearing, indoors and over water: there is no foliage lookup. They do not collide (`collide` false) and pass through floors; they end by lifetime.
- Colour. `leaf_color` is multiplied by the foliage tint of the season at the phase, blended by `tint_follow` (0 is the plain `leaf_color`, 1 the full tint; with no `Season` the tint is white, so the colour is `leaf_color`). The real alpha is `color_start.a` = 0.9 * `a` and `color_end.a` = 0 (same rgb): a leaf is born at 90% of the amount, so a thin fall is also fainter, and it fades out over its life. The emitter is not additive, so the leaves are alpha blended, with no texture.
- `max_particles` and `seed` come straight from the authoring. On the Low quality tier the extract halves both the rate and `max_particles`; there is no particle switch in the quality settings, so the other tiers draw the full emitter.

**Drawn only, and why it is not a `ParticleEmitter3D`.** The leaves are a rendering effect. The extract adds one local `render::ParticleEmitter` with the reserved id `0xFFFF0001`, far above any `ParticleEmitter3D` id (those are the entity index plus one), and nothing is written: no component is added or changed, and no `ParticleEmitter3D` entity exists for them. That is on purpose. A `ParticleEmitter3D` is a component, so it would sit in the state hash and the save file and a system would have to write it every tick (rain and snow do that). The leaf amount is exact from the clock, so it needs no state; two runs agree on the emitter description every frame, not on where an individual leaf is, which is the same for every GPU particle effect. A headless run draws no leaf, and `Season`, `Weather`, `WindDirection`, `WorldClock` and `LeafFall` itself are never written.

**The wind.** The drift reads the first `Weather`'s wind through `effective_wind` with the sway bit of the `WindGust` (`kGustSway`), so a `WindGust` that sways the foliage also swells the leaf drift, and the horizontal direction of the first `WindDirection` (+x when the scene has none). With no `Weather` the wind is 0 and the leaves fall straight down. `wind_follow` 0 turns the coupling off.

Scripts read and tune it through the `weather` table (section 27.3), next to the water ice: `weather.set_leaf_fall{enabled=, autumn_start=, autumn_full=, autumn_end_full=, autumn_end=, manual=, rate=, box_xz_m=, height_m=, size_m=, lifetime_s=, fall_speed=, wind_follow=, leaf_color={r,g,b}, tint_follow=, max_particles=, seed=, strength=}` and the getters `weather.leaf_fall_<key>()` act on the first `LeafFall`; the colour getter is `weather.leaf_fall_leaf_color()` and returns `r, g, b`. `weather.leaf_falling()` is read-only: the leaf amount 0..1 from the same `leaf_fall_amount` helper on the first `Season` at the first `WorldClock`'s day and hour (the manual floor with no `Season`, the fixed start phase with no clock), the number the renderer draws before the Low tier is applied. Every set key is optional and clamped to the component's range (colour channels 0..1); an unknown key, a value that is not a number, a colour that is not a table of three numbers or a scene without the component raises an error that names the call, and nothing is written. The setter never adds the component.

`samples/12_weather` carries a `LeafFall` on the Weather entity with `manual` 0.6, and logs `leaves:` (the amount from `weather.leaf_falling()`) every 150 ticks. Its `Season` (`year_days` 1) only climbs from phase 0.308 to 0.375 in the run and never reaches the autumn window, so the window is 0 and the manual floor keeps every line at `leaves: 0.600`; the camera-following leaves are drawn in all frames. To see the window itself, set the `Season` `start_phase` near 0.6 on a scene.

## 9.19t One minute, one year (M-SEASONTOUR)

`samples/14_seasons` is the demo that puts the seasonal line (`Season`, `WaterIce`, `LeafFall`, `SnowCover`) in one run, with no C++ and no new asset. Its `WorldClock` has a 2 s day (`drive_environment` 0, so the fixed sun does not move) and its `Season` a 30-day year starting at phase 0, so the year is 60 s, 3600 ticks at 60 Hz, and the phase is `tick / 3600`. The scene is a ground with a pit, a pond (`Water` plus `WaterIce`), five trunk boxes under `Foliage3D` canopies, a clear `Weather` with a `WindDirection`, `WindSway` and `SnowCover`, and a `LeafFall` with `manual` 0: every leaf and every bit of ice comes from the season. The script moves the camera in a circle (16 m radius, 7 m high, one lap per year) and logs `season: phase .. frozen .. leaves .. at tick N` every 150 ticks, reading `weather.season_phase()`, `weather.water_frozen()` and `weather.leaf_falling()`; it writes nothing else.

The log of a real run, at the four season centres (the script's tick N shows the state after N + 1 system ticks): tick 450, spring, `phase 0.125 frozen 0.000 leaves 0.000`; tick 1350, summer, `phase 0.375 frozen 0.000 leaves 0.000`; tick 2250, autumn, `phase 0.625 frozen 0.000 leaves 1.000`; tick 3150, winter, `phase 0.875 frozen 1.000 leaves 0.000`. Between them the leaves are non-zero from about tick 1950 to 2550 (the `LeafFall` window is 0.5 to 0.75 of the year), the ice from about tick 2550 to 3300 (0.114 at 2550, 0.654 at 2700, 1.000 from 3000 to 3150, 0.640 at 3300) and it has thawed by tick 3450; the ice starts while the last leaves fall, so for a few seconds leaves drift over a freezing pond. At tick 3600 the phase wraps to 0.000. The leaf box follows the camera (section 9.19s), so the leaves fall around the view wherever the orbit is. `ctest -R seasons` plays the whole year headless, pins these lines and checks that two plays give the same `state_hash`; the two windowed captures `sample_seasons_gpu_autumn` and `sample_seasons_gpu_winter` (ticks 2250 and 3150, `slow`, `gpu`) write `seasons_2250.ppm` and `seasons_3150.ppm` for a look.

## 9.19u Firefly data (M-FIREFLIES)

The optional `Firefly` component (`mce/kit/firefly.h`, one per scene, the first in ECS order is read) turns the `Season` summer, the night and the weather into a strength 0..1, and a pure helper turns the strength into the look of an emitter of slowly drifting glow points. Scenes without it are unchanged bit for bit, and there is no system: the strength is a pure function of the authoring, the `Season`, the year phase, the night fade and the rain, so nothing is stored or hashed beyond the authored fields. Authoring: `summer_start` (0.2) and `summer_full` (0.3), where the fireflies start to show and reach full strength, `summer_end_full` (0.45) and `summer_end` (0.55), where they start to thin and are gone (all 0..1, centred on the summer centre 0.375; an inverted pair is swapped, an empty one falls back to the defaults), `manual` (0, 0..1, a floor for the strength, so a scene with no `Season` can still show fireflies), `night_only` (1) and `min_darkness` (0.6, 0..1, the night fade below which none show, as `Meteor`), `rate` (18, 0..200, per second at full strength), `box_xz_m` (14, 1..100) and `height_m` (3, 1..20), the spawn box around the camera, `size_m` (0.08, 0.02..0.5), `lifetime_s` (6, 1..30), `drift_speed` (0.25, 0..2, m/s), `color` (0.8 1.0 0.35, linear), `max_particles` (256, 16..4096), `seed`, `strength` (1, 0..1, scales every effect; 0 is off exactly) and `enabled`.

`firefly_amount` is `max(manual, window)` times the night gate times `1 - rain` times `strength`, with `window` a trapezoid of smoothsteps rising over `summer_start..summer_full` and falling over `summer_end_full..summer_end`, so it peaks at the centre of summer and the other seasons are exactly 0. The night gate is the `Meteor` smoothstep of the night fade from `min_darkness` to 1 (1 when `night_only` is 0), so by day the amount is 0 and rain closes it. With no `Season` the strength is the `manual` floor (still gated by the night and the rain). `firefly_look` takes the amount and returns the emitter description: the rate (`rate` times the amount), a slow omnidirectional hover (tiny gravity, high drag, speeds from `drift_speed` halved to full), the size, the lifetimes, the box, and the glow colour with alpha `0.9 * amount` fading to 0 at the end of life. At amount 0 the look is inactive and all zero. `enabled` 0, `strength` 0 and a missing component give an amount of 0 and the inactive look exactly. The extract feed (23b) draws the fireflies as a render-side additive emitter that follows the camera. Scripts read and tune them through the `weather` table (section 27.3): `weather.set_firefly{enabled=, summer_start=, summer_full=, summer_end_full=, summer_end=, manual=, night_only=, min_darkness=, rate=, box_xz_m=, height_m=, size_m=, lifetime_s=, drift_speed=, color={r,g,b}, max_particles=, seed=, strength=}` (every key optional, clamped to the Range, a scene without a `Firefly` raises and nothing is added), the getters `weather.firefly_<key>()` (`weather.firefly_color()` returns `r, g, b`) and the read-only `weather.fireflies_glowing()`, the amount now (0 to 1) that the renderer draws. `samples/12_weather` carries a `Firefly` with `manual` 0.5 and logs `fireflies:` every 150 ticks.

## 9.19v Pollen data (M-POLLEN)

The optional `Pollen` component (`mce/kit/pollen.h`, one per scene, the first in ECS order is read) turns the `Season` spring, the daylight and dry weather into a strength 0..1, and a pure helper turns the strength into the look of an emitter of slowly falling pale specks that drift on the wind. Scenes without it are unchanged bit for bit, and there is no system: the strength is a pure function of the authoring, the `Season`, the year phase, the night fade and the rain. Authoring: `spring_start` (0.95) and `spring_full` (0.02), where the pollen starts to show and reaches full strength, `spring_end_full` (0.22) and `spring_end` (0.32), where it starts to thin and is gone (all 0..1, centred on the spring centre 0.125; the window wraps the year start, so the helper turns every threshold and the phase into a signed circular distance from the centre, with no trig, and the year start is just a point on the rising edge; an inverted pair is swapped, an empty one falls back to the defaults), `manual` (0, 0..1, a floor for the strength, so a scene with no `Season` can still show pollen), `day_only` (1) and `min_daylight` (0.5, 0..1, the daylight, one minus the night fade, below which none show), `rate` (14, 0..200, per second at full strength), `box_xz_m` (18, 1..100) and `height_m` (5, 1..30), the spawn box around the camera, `size_m` (0.06, 0.02..0.5), `lifetime_s` (12, 1..30), `fall_speed` (0.08, 0..2, m/s), `wind_follow` (0.6, 0..2), `color` (1.0 0.95 0.7, linear), `max_particles` (512, 16..4096), `seed`, `strength` (1, 0..1, scales every effect; 0 is off exactly) and `enabled`.

`pollen_amount` is `max(manual, window)` times the day gate times `1 - rain` times `strength`, with `window` a trapezoid of smoothsteps, so it is 1 from `spring_full` to `spring_end_full` and the other seasons are exactly 0. The day gate is a smoothstep of the daylight from `min_daylight` to 1 (1 when `day_only` is 0), so by night the amount is 0 and rain closes it. With no `Season` the strength is the `manual` floor (still gated by the daylight and the rain). `pollen_look` takes the amount, the scene wind speed and its horizontal direction and returns the emitter description: the rate (`rate` times the amount), a very slow fall (gravity and drag give the terminal speed `fall_speed`) tilted along the wind by `wind_follow`, the size, a long lifetime, the box, and the pale colour with alpha `0.8 * amount` fading to 0 at the end of life; the sprite is alpha blended, not additive. At amount 0 the look is inactive and all zero. `enabled` 0, `strength` 0 and a missing component give an amount of 0 and the inactive look exactly. The extract feed draws the pollen as one render-side alpha-blended emitter (id `0xFFFF0003`) in a box that follows the camera, with the wind read as the leaf feed does; the Low quality tier halves its rate and particle budget. Scripts read and tune the pollen through the `weather` table (section 27.3): `weather.set_pollen{enabled=, spring_start=, spring_full=, spring_end_full=, spring_end=, manual=, day_only=, min_daylight=, rate=, box_xz_m=, height_m=, size_m=, lifetime_s=, fall_speed=, wind_follow=, color={r,g,b}, max_particles=, seed=, strength=}` (every key optional, clamped to the Range, a scene without a `Pollen` raises and nothing is added), the getters `weather.pollen_<key>()` (`weather.pollen_color()` returns `r, g, b`) and the read-only `weather.pollen_drifting()`, the amount now (0 to 1) that the renderer draws. `samples/12_weather` carries a `Pollen` with `manual` 0.4 and logs `pollen:` every 150 ticks.

## 9.19w Wind-blown dust data (M-DUSTWIND)

The optional `Dust` component (`mce/kit/dust.h`, one per scene, the first in ECS order is read) turns the scene wind strength (`Weather.wind`, 0..1, with the gust) and dry weather into a strength 0..1, and a pure helper turns the strength into the look of an emitter of fast, small tan specks streaming sideways along the wind. The extract feed (25b) draws the specks and the Luau API (25c) is `weather.set_dust{...}`, the getters `weather.dust_<key>()` (`weather.dust_color()` returns `r, g, b`) and the read-only `weather.dust_blowing()`, the amount the renderer draws (section 27.3). Scenes without it are unchanged bit for bit, and there is no system: the strength is a pure function of the authoring, the gusted wind strength (0..1) and the rain. Authoring: `wind_start` (0.3) and `wind_full` (0.8), both 0..1 on the scale of the scene wind (not m/s), where the dust starts to blow and reaches full strength (an inverted or empty pair falls back to the defaults), `manual` (0, 0..1, a floor for the strength, so a desert scene can show dust in a calm), `rate` (40, 0..400, per second at full strength), `box_xz_m` (14, 1..100) and `height_m` (3, 1..20), the spawn box around the camera, `size_m` (0.05, 0.01..0.5), `lifetime_s` (2.5, 0.5..10), `speed` (6, 0..30, the sideways speed in m/s at full wind), `wind_follow` (0.8, 0..2, how much of that speed the specks take), `color` (0.78, 0.66, 0.46, linear), `max_particles` (1024, 16..4096), `seed`, `strength` (1, 0..1, scales every effect, 0 is off exactly) and `enabled` (1).

`dust_amount` is `max(manual, smoothstep(wind_start, wind_full, wind))` times `1 - rain` times `strength`, so it is 0 below `wind_start` (apart from the manual floor), 1 at `wind_full` in dry weather, and rain closes it. `dust_look` takes the amount, the scene wind speed and its horizontal direction and returns the emitter description: the rate (`rate` times the amount), a fast, almost flat stream along the wind direction with speeds scaled by `speed` and `wind_follow`, the size, a short lifetime, the box, and the tan colour with alpha `0.6 * amount` fading to 0 at the end of life; the sprite is alpha blended, not additive. At amount 0 the look is inactive and all zero. `enabled` 0, `strength` 0 and a missing component give an amount of 0 and the inactive look exactly. The extract feed draws the dust as one render-side alpha-blended emitter (id `0xFFFF0004`) in a box that follows the camera, with the wind and its direction read as the leaf feed does and the rain of the first `Weather` (only Rain and Storm); the Low quality tier halves its rate and particle budget. `Dust`, `Weather` and every `ParticleEmitter3D` are never written. `samples/12_weather` carries a `Dust` with `manual` 0.4.

## 9.19x Bird flock data (M-BIRDFLOCK)

The optional `Flock` component (`mce/kit/flock.h`, one per scene, the first in ECS order is read) turns the migration seasons, the twilight and dry weather into a strength 0..1, and a pure helper turns the strength into the look of an emitter of a few small dark specks that cross a wide, high box around the camera in a fixed heading. The extract feed (26b) draws the specks and the Luau API (26c) is `weather.set_flock{...}`, the getters `weather.flock_<key>()` (`weather.flock_color()` returns `r, g, b`) and the read-only `weather.flock_flying()`, the amount the renderer draws (season phase, sun height and rain as the extract reads them); the `12_weather` script logs it as `flock:`. Scenes without it are unchanged bit for bit, and there is no system: the strength is a pure function of the authoring, the season phase, the sun height and the rain. Authoring: `migration_full` (0.06) and `migration_end` (0.12), the half-widths in season phase of the migration windows around the spring centre 0.125 and the autumn centre 0.625 (full strength inside the first, none beyond the second, a smoothstep between; an inverted or empty pair falls back to the defaults), `manual` (0, 0..1, a floor, so a scene can show birds all year), `twilight_width` (0.25, the sun height `|y|` below which the twilight gate opens), `rate` (3, 0..60, per second at full strength), `box_xz_m` (60, 1..200) and `height_m` (45, 1..120), the spawn box around the camera, `size_m` (0.35), `lifetime_s` (14), `speed` (9, m/s), `heading_deg` (70, 0..360, the direction of travel on the ground plane, 0 is +z and 90 is +x), `color` (0.08, 0.07, 0.07, linear), `max_particles` (64, 8..1024), `seed`, `strength` (1, 0..1, scales every effect, 0 is off exactly) and `enabled` (1).

`flock_amount` is `max(manual, migration)` times the twilight gate times `1 - rain` times `strength`, where `migration` is the larger of the two trapezoids (measured as a signed circular distance of the phase from the centre, so the year start wraps safely) and the twilight gate is `1 - smoothstep(0, twilight_width, |sun_y|)`: 1 with the sun on the horizon, 0 by day and by night. With no `Season` the amount is the `manual` floor (still gated). `flock_look` takes the amount and returns the emitter description: the rate (`rate` times the amount), a fast, straight stream along the heading with no gravity and almost no drag, the size, a long lifetime, the box, and the dark colour with alpha `0.85 * amount` fading to 0 at the end of life; the sprite is alpha blended, not additive. At amount 0 the look is inactive and all zero. `enabled` 0, `strength` 0 and a missing component give an amount of 0 and the inactive look exactly. The extract feed draws the flock as one render-side alpha-blended emitter (id `0xFFFF0005`) in a box that follows the camera, `height_m` above it, reading the first `Season` and `WorldClock` for the phase as the pollen feed does, the sun height from the environment (`time_of_day` through the day-night sun, or, when `time_of_day` is negative, the y of the fixed `sun_direction`), and the rain of the first `Weather` (only Rain and Storm); the Low quality tier halves its rate and particle budget (`max_particles` is clamped to 16..1024 first). `Flock`, `Season`, `WorldClock`, `Weather` and every `ParticleEmitter3D` are never written. `samples/12_weather` carries a `Flock` with `manual` 0.4.

## 9.19y Butterfly data (M-BUTTERFLY)

The optional `Butterfly` component (`mce/kit/butterfly.h`, one per scene, the first in ECS order is read) turns the summer window, the daylight, calm wind and dry weather into a strength 0..1, and a pure helper turns the strength into the look of an emitter of a few small bright specks that drift slowly and erratically in a low box around the camera. This step adds the data and the helpers only; nothing reads them yet (the extract feed comes in 27b and the Luau API in 27c, so scenes are unchanged and no butterfly is drawn). Scenes without it are unchanged bit for bit, and there is no system: the strength is a pure function of the authoring, the season phase, the night fade, the wind and the rain. Authoring: `window_full` (0.07) and `window_end` (0.14), the half-widths in season phase of the summer window around the summer centre 0.375 (full strength inside the first, none beyond the second, a smoothstep between; an inverted or empty pair falls back to the defaults), `manual` (0, 0..1, a floor, so a scene can show butterflies all year), `min_daylight` (0.6, the daylight, one minus the night fade, below which none fly), `wind_calm` (0.25) and `wind_still` (0.7), the wind below which they fly fully and above which none fly, `rate` (5, 0..60, per second at full strength), `box_xz_m` (18, 1..100) and `height_m` (3, 1..20), the spawn box around the camera, `size_m` (0.12), `lifetime_s` (8), `speed` (0.8, m/s), `lift` (0.2, a slight upward acceleration), `color` (1.0, 0.75, 0.2, linear), `max_particles` (48, 8..512), `seed`, `strength` (1, 0..1) and `enabled`.

`butterfly_amount` is `max(manual, window)` times the day gate times the calm gate times `1 - rain` times `strength`, where the day gate is `smoothstep(min_daylight, 1, daylight)` and the calm gate is `1 - smoothstep(wind_calm, wind_still, wind)`. With no `Season` the amount is the `manual` floor (still gated). `butterfly_look` takes the amount and returns the emitter description: the rate (`rate` times the amount), slow speeds scaled by `speed`, an upward direction with a wide spread, a slight lift as gravity, strong drag, the size, the lifetime, the box, and the warm colour with alpha `0.9 * amount` fading to 0 at the end of life; the sprite is alpha blended, not additive. At amount 0 the look is inactive and all zero. `enabled` 0, `strength` 0 and a missing component give an amount of 0 and the inactive look exactly. NaN inputs and authoring fall back to the defaults. `samples/12_weather` carries a `Butterfly` with `manual` 0.4.

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
the `level` made from it (0..`max_level`, `heat_per_level` heat for each) and how crimes count (`kills` counts the deaths the offender caused). `Police3D` goes
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
  and inside 12 m, and brakes to a stop inside 4 m. With `drive_car` 1 there is no path following, so a wall between the car and the target
  stops it; `drive_car` 2 (M-ARREST 4) drives along the road network (see 29-roads-and-traffic) instead: the quickest lane route from the car's lane to the
  lane nearest the target (`road_route`), steered by pure pursuit, falling back to the straight line when no lane is within 30 m of
  the car or 80 m of the target. A driven car that pushes the throttle for 2 s without moving (a wall, a dead end) reverses for 1.5 s and tries again (`stuck_time` and `reverse_left` hold the timers). A roadblock is a car with `drive_car` 0 placed across the road.

- **Roadblocks that come with the wanted level (M-OPENWORLD-POLICE 3).** Put a `Roadblock3D` on a marker entity where the block
  should be: its local X runs across the road (the line of cars), its local +Z is the side `officer_back` metres away that the
  officers stand on (negative: in front of the cars). Give it a `car` prefab (a police car whose root is a static body, so it
  blocks the road) and an `officer` prefab (a root with `Police3D`, `Health3D` and a weapon). While an offender's wanted level is at
  `min_level` (default 3) or above and it is between `spawn_min` and `spawn_max` metres away, the marker spawns `cars` cars side by
  side `car_spacing` apart and `officers` officers, each tagged with `RoadblockPiece3D`. `hold` seconds after the level drops
  below `min_level` they are destroyed, but never while the offender is closer than `spawn_min` (nothing vanishes in sight).
  A block waits while a living thing (a `Health3D`) or a vehicle is within 3 m of a piece's place, and after a spawn that made nothing (no prefab or hook, a prefab that does not load) it waits 5 s before trying again. Which markers are up is read from the pieces that exist; the pieces are made at run time and not saved, so a loaded game puts the block up again from the wanted level. Without prefabs a game spawns the pieces itself through the `RoadblockSpawnHook` resource (kind 0 car, 1 officer). Ordinary
  `Police3D` units placed in the scene with a higher `min_level` are the other way to have more police at a high level; a
  `Roadblock3D` is for units that appear only then.

- **Busted and wasted (M-WASTED).** Put `Respawn3D` (`delay`, default 3 s) on the player and `RespawnPoint3D` on a few
  entities (`kind` Hospital or Station). After the player dies it wakes up at the nearest Hospital, after an arrest at the
  nearest Station (any point when the wanted kind is missing; where it stands when there is none), with full health and
  no wanted level, and `Respawned3D { entity, reason, position }` is pushed (`reason` 1 busted, 2 wasted). Fines and lost
  weapons are the game's, on that event. With captions installed (the stock player does) the wait also shows "WASTED" or
  "BUSTED" through the subtitle view; `caption` 0 turns that off.
- **Money (M-CASH).** Put `Wallet3D` (`cash`) on whoever carries money and `CashDrop3D` (`min`, `max`) on an entity with
  a `Health3D`: when it dies, its killer's wallet is credited a random amount in `[min, max]` (the same every replay),
  and `CashChanged3D { entity, delta, cash }` is pushed. From a system, `give_cash(world, e, n)` adds and
  `take_cash(world, e, n)` takes up to `n` and returns what it took; a wallet never goes below zero. `Respawn3D` also takes a fine from the player's wallet when it wakes
  up (`fine_busted` 100, `fine_wasted` 200 by default; never more than the wallet holds) and shows "-$N" through the
  captions.
- **Pay-and-spray (M-PAYSPRAY).** Put `PaySpray3D` (`radius` 3 m, `price_per_level` 100) on a placed entity, a garage. A
  player with a `Wanted3D` at level 1 or more standing within `radius`, whose `Wallet3D` holds `price_per_level` times the
  level, pays that and loses the wanted level; `Sprayed3D { entity, garage, paid, level }` is pushed. Too poor, too far or
  `enabled` 0: nothing happens. Whether police in sight should block it is the game's rule.
- **Mission pay (M-MISSIONPAY).** Put `MissionReward3D` (`cash`, default 100) on a Mission entity (see missions): when the
  mission completes, its `actor`'s `Wallet3D` is credited that much and `CashChanged3D` is pushed. A failed mission, an
  actor without a wallet or no `MissionReward3D` pays nothing. `fail_fee` (default 0, M-MISSIONFEE) is taken from the wallet
  (never below zero) when the mission fails, whatever the reason.
- **Car theft (M-CARJACK).** Put `CarOwner3D` (`heat`, default 8) on a parked car that belongs to someone (it also needs a
  `VehicleSeat3D`). The first time a rider with a `Wanted3D` boards it, that heat is added as a crime (no police witness
  needed) and the car is marked `stolen`, so it counts once. Cars without it cost nothing. Set `needs_witness` to 1 and the theft counts only when a police
  unit within the rider's `witness_range` can see the car.
- **Ammo shop (M-AMMOSHOP).** Put `AmmoShop3D` (`radius`, `price_per_round`, `stock_to`) on a shop entity. A player who has a
  `Weapon3D` (with a `magazine`, not `infinite_ammo`) and a `Wallet3D` and stands within `radius` has the weapon's `reserve`
  filled up to `stock_to` and pays `price_per_round` for each round; with too little cash it buys as many as it can afford.
- **Cash pickups (M-CASHPICKUP).** Put `CashPickup3D` (`cash`, `radius`, `respawn`) on a placed entity: the first `Wallet3D` holder that
  comes within `radius` is paid `cash` and `CashPickedUp3D` is pushed. With `respawn` 0 the pickup is destroyed; otherwise it
  waits `respawn` seconds before it can be taken again (hide it from the event). A holder with a full wallet takes nothing.
- **Mission givers and wanted-level failure (M-MISSIONSTART).** Put `MissionMarker` (`mission`, `radius`) on a placed entity: when the
  `actor` of that Idle `Mission` stands within `radius`, the mission starts, as if `start` were set. Add `MissionWantedFail`
  (`level`, default 3) to a Mission entity and it fails (`MissionFailed` reason 3) once its actor's `Wanted3D` reaches that level.
- **Retrying a failed mission (M-MISSIONRETRY).** Add `MissionRetry` (`delay`, default 3 s) to a Mission entity: `delay` seconds after
  it fails the mission goes back to Idle with its objectives Pending, so its `MissionMarker` (or `start`) runs it again.
- **Chop shop (M-CHOPSHOP).** Put `ChopShop3D` (`radius`, `price`) on a scrapyard entity. A stolen car (`CarOwner3D.stolen` 1, with a
  `VehicleSeat3D`) within `radius` whose driver holds a `Wallet3D` is bought: the driver is paid `price`, the car is destroyed, the
  driver steps out and `CarSold3D` is pushed. Needs `install_seats3d` for the seats.
- **Repair shop (M-REPAIRSHOP).** Put `RepairShop3D` (`radius`, `max_speed`, `price`, `price_per_level`, `wanted_drop`, `need_unseen`) on a
  garage pad. A car with a `VehicleSeat3D` driver who holds a `Wallet3D`, within `radius` and slower than `max_speed`, is mended for
  `price` when it is hurt or wrecked (`repair_vehicle`: full `Health3D`, the wreck drives again, its `ParticleEmitter3D` stops), and
  while no police sees the driver the wanted level drops by `wanted_drop` levels (0 clears it) at `price_per_level` each. All or
  nothing, once per visit; `Repaired3D` is pushed.
- **Confiscation (M-ARRESTLOSS).** Put `Confiscate3D` (`ammo_kept`, 0 to 1) next to `Respawn3D` and `Weapon3D` on the player: waking up
  busted leaves `reserve * ammo_kept` spare rounds (rounded down). Dying keeps them all.
- **Medic (M-HEALSHOP).** Put `HealShop3D` (`radius`, `price_per_hp`) on a clinic entity. A living player with a `Health3D` below
  its `max` and a `Wallet3D` who stands within `radius` is healed one hit point per `price_per_hp` cash, up to `max`; with
  too little cash it buys as many hit points as it can afford.

Not in this step: spawning new units, and cars that follow roads.

`ItemHotkey3D` (on an entity with an `Inventory`) lets the `UseItem` input action use the first consumable in the bag; `LootDrop` can draw its pickups with `mesh`, `tint`, `mesh_scale` and `lift` (`samples/09_openworld`: walkers drop bandages and medkits, H uses one).
