# 9. Physics

2D physics is scene-authored: a body is an entity with `RigidBody2D` and `Collider2D`, the engine
steps the world every fixed tick in `FixedUpdate_Physics` and writes the resulting poses into
`LocalTransform`. Game code reads events and controller state, moves kinematic bodies by velocity,
and uses queries. 3D physics is the kit's `Physics3D` world with static `BoxCollider3D` / `MeshCollider3D` (sections 8.2 and 8.7).

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

## 9.6 Joints

From code: `DistanceJointDesc` (a rod, a rope with `min_length < max_length`, or a spring with
`hertz > 0`) and `RevoluteJointDesc` (a hinge with an optional angle limit and motor), created on
the world with `create_distance_joint(desc)` / `create_revolute_joint(desc)` and addressed by the
returned `JointId` (`destroy_joint`, `set_motor_speed`, `set_joint_limits`). Prismatic, wheel, weld and mouse joints are not built.
Joints are not scene-authored yet.

## 9.7 Configuration

The host installs physics with the default `PhysicsConfig`: gravity −9.81 on Y, 8 velocity and 3
position iterations, warm starting on. Two v0.5 switches, `enable_sleep` and `block_solver`, are
off by default to keep older replays bit-identical; a new game that stacks many bodies benefits from
`block_solver = true` with 16 velocity iterations. Changing the config means a custom host today
(the player uses the defaults).

Budget: 2,000 bodies is the tested ceiling; a 20-body stack settles within a second.

## 9.8 Debugging

The editor viewport overlays colliders. In code, `World2D::debug_draw(sink, flags)` fills line and
circle primitives for a renderer you provide; `stats()` reports body and contact counts;
`validate_broadphase()` checks the tree. `hash_state()` is part of the determinism hash, so a
physics change shows up in `-headless` runs immediately.
