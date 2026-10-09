# physics3d — World3D, a facade over embedded Jolt Physics 5

`mce_physics3d` is an L2 library in `src/engine/physics3d/`. Its public dependencies are `mce_core`
and `mce_ecs`; it links `Jolt` privately, so Jolt's include paths, defines and `-msse4.2` stay inside
the target (`src/engine/physics3d/CMakeLists.txt`). Jolt is `v5.6.0`, fetched by tag through
FetchContent (ADR-0003 §1, ADR-0017). The library is linked by `mce_editor_core` (orbit camera
collision), the samples `04_cubes`, `05_ashen_court` and `physics3d_headless`, and `test_physics3d`.
It is in the `mce_engine` umbrella's `MCE_ENGINE_LIBS` list (the editor, an `UMBRELLA` target, needs it).

There is one class, `World3D`, and no ECS bridge yet. Gameplay owns a `World3D` and calls it from its
own systems (for example `samples/05_ashen_court`).

## Public surface

One header, `include/mce/physics3d/world3d.h`, namespace `mce::physics`. It names no Jolt type.

- `Shape3D`: one shape per body in the body's local frame: `box`, `sphere`, `capsule`, `convex`
  (hull points) and `mesh` (vertices plus counter-clockwise triangle indices). Convex and mesh data
  only has to outlive `create_body()`.
- `BodyDesc3D`: body type (`BodyType3D::Static`, `Kinematic`, `Dynamic`), shape, pose, friction,
  restitution, density, gravity scale, damping, and the same 16-layer `layer` / `mask` model as 2D.
- `PhysicsConfig3D`: gravity, `fixed_dt` (1/60 s), Jolt capacity limits, and the fixed 4 MiB temp
  allocator size.
- `World3D` lifecycle: `create_body`, `create_character`, `destroy_body`, `has_body`.
- `step(dt)`: asserts `dt == config().fixed_dt`.
- Mutation: `set_linear_velocity`, `apply_impulse` (centre of mass), `teleport`; `body_state` reads a
  `BodyState3D`.
- Character: `ControllerParams3D` (capsule radius and half height, `max_slope_deg`, `step_height`,
  `stick_to_floor`, `padding`, mass and push force, layer and mask). `move_character(e, delta,
  params)` drives a Jolt `CharacterVirtual` and returns `MoveResult3D` with `ControllerFlags3D`
  (`Ground`, `SteepGround`, `Wall`, `Ceiling`, `OnMovingBody`). `controller_state` returns the last
  result. Gravity, jumps and input are folded into `delta` by gameplay.
- `camera_sweep(from, to, radius)`: sphere sweep along a camera boom against static and kinematic
  geometry. Dynamic props and characters never pull the camera in. Returns `ShapeHit3D`, whose
  `position` is the safe camera centre.
- Queries: `raycast` and sphere `shape_cast`, closest hit, ties broken by creation order, filtered by
  `QueryFilter3D` (layer mask, exclude span, `include_dynamic`).
- `hash_state(seed)` and `stats()` (bodies, active bodies, characters).

`World3D` mirrors `World2D`'s method spelling (`step`, `create_body`, `destroy_body`, `has_body`,
`set_linear_velocity`, `teleport`, `body_state`, `raycast`, `shape_cast`, `controller_state`,
`hash_state`, `stats`, `config`). The 3D additions are `move_character` and `camera_sweep`.

## Rules and invariants

- JPH wall (`scripts/ci/check_walls.py`): `JPH::` and `<Jolt/...>` may appear only under
  `src/engine/physics3d/`; the scan covers `src/`, `samples/`, `games/` and `tests/`. The Jolt link
  wall allows `Jolt` in a CMakeLists only under `src/engine/physics3d/`.
- Body ids never leave the facade; callers name bodies by `Entity`.
- Determinism (ADR-0003 §8): single-threaded stepping on `JobSystemSingleThreaded`, Jolt built with
  `CROSS_PLATFORM_DETERMINISTIC`, every loop in creation order. Same binary and same inputs give the
  same `hash_state`; Debug and Release agree. Cross-machine determinism is not claimed.
- Jolt build switches (ADR-0003 §2): no exceptions, no RTTI, no FMA or AVX, no Jolt asserts,
  profiler or debug renderer, single precision, `OBJECT_LAYER_BITS` 32.
- Layers (ADR-0003 §4): layer index in bits 0–3, a moving bit (bit 4) and the mask in bits 16–31 of
  Jolt's `ObjectLayer`. A pair collides only when each layer is in the other's mask.
- Memory (ADR-0003 §5): Jolt's general allocations go through `mce::mem::heap()`. Jolt's global
  state is reference-counted by live `World3D` instances; create and destroy worlds on the main
  thread only.
- Malformed shapes (non-positive extents, fewer than 4 hull points, empty or out-of-range mesh)
  make `create_body` return false.
- Characters are not rigid bodies: they collide with the world and each other, are not hit by
  queries, and push dynamic bodies through contact impulses.

## How it is tested

ctest `test_physics3d` (no GPU), sources in `tests/physics3d/`. No test names Jolt.

- `test_world3d.cpp`: "world3d: body lifecycle, stale handles and malformed shapes", falling box,
  kinematic velocity and teleport, "step asserts on a non-fixed dt", layers and masks, same-binary
  hash, "Jolt allocations go through the MCE heap and are released with the last world".
- `test_queries.cpp`: raycast (including a triangle mesh), sphere cast, "camera_sweep: the boom
  stops in front of walls, ignores props and characters".
- `test_character.cpp`: ground, walls and sliding, step climbing up to `step_height`, slope limits,
  ceiling, moving-platform carry, pushing props, "same-binary determinism of a scripted walk".
- `test_mirror.cpp`: "mirror: 2D and 3D worlds share method spelling (compile-time)".

Also: `sample_physics3d_headless_300`, the determinism stage (3 runs, Debug and Release, of
`sample_physics3d_headless`), `sample_04_cubes_walk_300`, and the editor's "orbit camera:" cases in
`test_editor`.

## Design records

- ADR-0003 Embed Jolt Physics 5 for 3D (`docs/adr/0003-embed-jolt-for-3d.md`): version, build flags,
  facade, layers, simulation contract, character, camera sweep, determinism, deferrals, Plan B.
- [Physics architecture](../architecture/physics.md): D1, D4, D7, §2.2 world facades, §3.7 the Jolt
  embed, §6 tests 6, 7, 10 and 13.
- ADR-0029 Play-in-editor (`docs/adr/0029-play-in-editor.md`) §3: the editor orbit camera collision gate.
- M5b milestone (`docs/milestones/M5b.md`) and M7 Ashen Court (`docs/milestones/M7-ashen-court.md`)
  (third-person character and camera on `World3D`).

## Known gaps

From ADR-0003 §9 and the M5b "Open" table:

- No ECS bridge: no `install_physics3d`, no `RigidBody3D` / `Collider3D` / `CharacterController3D`
  components, no contact events.
- No cooked mesh colliders: triangle meshes and convex hulls are built at body creation from
  caller memory. The cook path is meant to replace this without changing the facade.
- No debug draw and no sensors.
- `QueryFilter3D` and the 2D `QueryFilter` are separate types; unifying them is left for the ECS
  bridge work (ADR-0003 §4).
- Jolt has not been built with MSVC yet (ADR-0003 §2, ADR-0018).
