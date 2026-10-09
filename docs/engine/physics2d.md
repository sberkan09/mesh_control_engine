# physics2d — hand-written 2D rigid-body physics, character controller and queries

`mce_physics2d` is an L2 library in `src/engine/physics2d/`. Its public dependencies are
`mce_core`, `mce_refl` and `mce_ecs` (see `src/engine/physics2d/CMakeLists.txt`). It is written
from scratch in the style of Box2D 2.x (Catto); Box2D itself is never linked here and appears only
as a test-time oracle (ADR-0016). It is linked by `mce_gameplay_kit`, `mce_editor_core`, the
`mce_engine` umbrella (it is in `MCE_ENGINE_LIBS`), `mce_player_runtime` on platforms without game
modules, `tools/cook`, and the samples `03_platformer` and `physics2d_headless`.

The library has two layers. `World2D` is a plain physics world that knows only the `Entity` handle.
`install_physics2d` bridges it into the ECS as one system in the `FixedUpdate_Physics` phase.

## Public surface

Headers live under `include/mce/physics/`.

`components.h`: reflected plain structs, the authored data a scene carries.

- `RigidBody2D`: body type (`Static`, `Kinematic`, `Dynamic`), `fixed_rotation`, collision `layer`
  (0..15) and `mask`, `gravity_scale`, `linear_damping`. One entity is one body.
- `Collider2D`: one `ColliderShape` per body plus density, friction, restitution, `is_sensor` and
  `one_way`.
- `ColliderShape`: tagged `Box`, `Circle` or `Convex` (up to `kMaxPolygonVertices` = 16 points; the
  hull is computed at body creation). Factories `box()`, `circle()`, `convex()`.
- `CharacterController2D`: opts an entity into the swept controller (`max_slope_deg`,
  `snap_down_distance`, `skin_width`). Implies a kinematic body.
- `CharacterGround2D`: controller output (`ControllerFlags` bits `Ground`, `WallLeft`, `WallRight`,
  `Ceiling`, `OnMovingPlatform`, plus ground normal, platform velocity, ground body). Fields are
  `ReadOnly, Transient`; physics adds the component to controllers that lack it.
- Events: `TriggerEnter`, `TriggerExit`, `ContactBegin2D`, `ContactEnd2D`.

`world2d.h`: the world.

- `PhysicsConfig`: gravity, `fixed_dt`, solver iterations (8 velocity / 3 position), warm starting,
  restitution threshold, `max_contact_events_per_tick`, and the opt-in v0.5 switches
  `enable_sleep` and `block_solver`.
- `World2D`: body lifecycle (`create_body`, `destroy_body`, `set_body_config`, `set_collider`,
  `set_controller`), `step(dt)`, body mutation (`set_linear_velocity`, `apply_impulse`,
  `apply_force`, `teleport`, `set_enabled`), sleeping (`is_awake`, `set_awake`), joints,
  `move_and_collide` and `controller_state`, queries, per-step event spans, `debug_draw`,
  `hash_state`, `each_body`, `stats`, `validate_broadphase`.
- Joints: `DistanceJointDesc` (rod, rope with `min_length < max_length`, or spring with
  `hertz > 0`) and `RevoluteJointDesc` (optional angle limit and motor), addressed by a generational
  `JointId`.
- Queries: `raycast`, `raycast_all`, `shape_cast`, `overlap_point`, `overlap_aabb`, filtered by
  `QueryFilter` (layer mask, exclude span, sensors on/off, optional predicate). Results are
  `RayHit2D`.
- `DebugDrawSink` and `DebugDrawFlags`: physics fills line and circle primitives; the editor wires the
  sink to a renderer. Physics never includes renderer headers.
- `BodyState`, `MoveResult2D`, `World2D::Stats`: read-only snapshots for the ECS export and tools.

`module.h`: the ECS bridge.

- `install_physics2d(world, config)`: registers the components and events, the `World2D` resource,
  the `on_remove` hooks that free bodies, and the `physics_step` system.
- `physics_step(Frame&)`: IMPORT (create or update bodies from components in entity-index order),
  STEP, EXPORT (poses to `LocalTransform`, controller results to `CharacterGround2D`), EVENTS (into
  `EventQueue<T>`, readable in `FixedUpdate_Post`).

Internals in `src/`: `collision.{h,cpp}` (SAT and clipping manifolds, GJK distance, casts) and
`dynamic_tree.{h,cpp}` (the dynamic AABB tree broadphase). Tests include them directly as white-box
code.

## Rules and invariants

- Determinism (physics.md D8): single-threaded, float only, every loop in body-slot order, every
  query result sorted (by `t`, then body slot). `hash_state()` folds all body state and is part of
  the FR-RUN-040 hash harness. Physics is single-threaded through v1.0 (physics.md §4.1).
- Transform authority (D2): during play the physics world owns the pose of bodied entities and
  mirrors it into `LocalTransform` each fixed tick. Move such entities with `teleport`, not by
  writing `LocalTransform`. Pose and velocity are not components.
- Body ids never leave `World2D`; callers name bodies by `Entity`. Stale handles are a no-op.
- Authoring conventions a scene needs (M9): `RigidBody2D::layer` is a layer index (0..15) and
  `mask` a bit set of layers, so a sensor that should see only the player on layer 1 has `mask: 2`.
  A character is a `Kinematic` body with `fixed_rotation`, `gravity_scale: 0`, a box `Collider2D`
  and `CharacterController2D`; gameplay applies gravity itself and moves it with
  `move_and_collide`. Static and kinematic colliders take `density: 0`. A body exists only after the
  first `physics_step` (IMPORT), so `has_body` is false during the first tick's `FixedUpdate_Pre`.
- `World2D::set_enabled(e, false)` is how gameplay switches a body off (crumbling blocks, Kindle's
  colour blocks): the body keeps its slot, neither collides nor raises sensor events, and the ECS
  bridge leaves it disabled: IMPORT touches an existing body only when its `RigidBody2D`,
  `Collider2D` or `CharacterController2D` bytes change, so gameplay should not rewrite those
  components on a disabled body.
- Body mutation belongs in `FixedUpdate` systems (gameplay uses `FixedUpdate_Pre` through
  `f.resource<physics::World2D>()`). Queries are read-only and allowed in any phase.
- Malformed shapes (degenerate box, fewer than 3 hull points) make `create_body` return false; the
  entity simply has no body until its components change.
- Component layouts are padding-free on purpose, because `World::state_hash` hashes component
  bytes. Keep explicit `pad_` fields when adding members.
- `enable_sleep` and `block_solver` default to off so M3/M3.5 replays and hashes stay
  bit-identical. ADR-0026 §3 names `block_solver = true` with `velocity_iterations = 16` the
  stacking profile and says new games should turn both on.
- Walls (`scripts/ci/check_walls.py`): the Box2D wall allows `b2*` and `<box2d/...>` only under
  `tests/physics2d/oracle/`. The engine container ban applies (no `std::vector`, `std::string`,
  `<iostream>` and similar under `src/engine/`).

## How it is tested

ctest `test_physics2d` (no GPU), sources in `tests/physics2d/`:

- `test_collision.cpp`: manifolds, convex hull, mass properties, GJK, raycast, shape cast, dynamic
  tree ("dynamic tree: insert/move/remove keeps a valid balanced tree; queries are exact").
- `test_manifold_cases.cpp`: "manifold table: 20 hand-computed cases (FR-PHY-010)".
- `test_world.cpp`: free fall, restitution, Coulomb friction, sensors exactly once, queries
  ("world: 10k seeded queries are reproducible (FR-PHY-040)"), stale handles, pile determinism.
- `test_solver.cpp`: stacks and pyramids with warm starting, islands, platform carry.
- `test_controller.cpp`: ground, walls, slopes, snap-down, one-way and drop-through, "rides an
  accelerating moving platform for 600 ticks with zero net slide".
- `test_joints_sleep.cpp`: distance and revolute joints against hand-computed values, sleeping,
  "FR-PHY-060: a 20-body stack settles in under 1 s", the 2,000-body budget, block solver.
- `test_module.cpp`: the ECS bridge, trigger events in `FixedUpdate_Post`, malformed colliders,
  "module: full-tick determinism hash is stable across runs (FR-RUN-040)".
- `oracle/test_box2d_oracle.cpp`: drop, slide, restitution, pyramid-lite and platform-ride scenes
  compared with Box2D v3. Built only with `MCE_PHYSICS_ORACLE` (default ON).

Also: `sample_physics2d_headless_300` (300 headless steps), and the platformer sample tests that run
the full gameplay stack.

## Design records

- [Physics architecture](../architecture/physics.md): §0 decisions D1–D9, §2.1 components, §3.2
  internals, §3.3 body lifecycle, §3.6 controller, §4 threading, §6 test plan.
- ADR-0016 Box2D v3 as the test-time oracle (`docs/adr/0016-box2d-test-oracle.md`)
- ADR-0026 M6 subsystems (`docs/adr/0026-m6-subsystems.md`) §3: joints, sleeping, block solver,
  2,000 bodies.
- M6 subsystems milestone (`docs/milestones/M6-subsystems.md`), "Physics2d v0.5" section.
- ROADMAP M3 (rung-6 shape) and M3.5 (solver hardening).

## Known gaps

- Not built (ADR-0026 §3): prismatic, wheel, weld and mouse joints; joint break forces; continuous
  collision between dynamic bodies; a persistent contact graph (contacts are rebuilt each step).
- One collider per body. Compound shapes are planned as child entities (comment on `Collider2D`).
- The ECS bridge detects component changes by comparing against a cached copy per entity
  (`SyncState` in `src/module.cpp`), not with ECS change ticks.
