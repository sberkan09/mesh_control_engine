# M5b — Physics 3D + play-in-editor + hot reload (milestone record)

**Status:** the physics half (ADR-0003 + `mce_physics3d`) is merged into `dev_claude`. Play-in-editor, the
hot-reload matrix and the editor orbit-camera collision gate are editor work and are tracked separately; the
`04_cubes` capsule walk waits for the M5a renderer sample. See *Open* below.
**Measured on:** the Linux CI container (4 cores), gcc 13, `scripts/ci/local_ci.sh`.

## What shipped

| ROADMAP M5b item | Where | Evidence |
|---|---|---|
| ADR-0003 before the first `JPH::` include | `docs/adr/0003-embed-jolt-for-3d.md` | committed alone, ahead of any Jolt include (git history of this branch) |
| Jolt 5 embed | `cmake/Dependencies.cmake` (Jolt `v5.6.0`, FetchContent) | builds clean under the no-exceptions / no-RTTI / `-ffp-contract=off` policy in every Linux preset |
| `mce_physics3d`: `World3D` facade | `src/engine/physics3d/` | no Jolt type in `mce/physics3d/world3d.h`; pimpl over `vec3`/`quat`/`Entity` |
| `World3D::move_character` (FR-PHY-080 scaffolding) | `CharacterVirtual` through `ExtendedUpdate` | `test_physics3d` controller suite: ground, walls + sliding, 0.25 m step climbed / 0.6 m ledge refused, 20° ramp walked / 65° refused, ceiling, moving-platform carry, pushing a prop, character-vs-character |
| `World3D::camera_sweep` (UC-B camera gate, physics side) | sphere cast against static + kinematic geometry | `test_physics3d`: the boom stops 0.2 m in front of a wall, ignores a crate and a character on the boom, stops above the floor |
| Queries (`raycast`, `shape_cast`) | closest hit, ties by creation order, layer mask / exclude list | `test_physics3d` query cases, including a triangle mesh |
| JPH wall | `scripts/ci/check_walls.py` | the include/symbol wall (Jolt only under `src/engine/physics3d/`) plus a new CMake link wall (only `mce_physics3d` links `Jolt`) |
| 2D/3D mirror signatures (physics.md §2.2) | `tests/physics3d/test_mirror.cpp` | compile-time: both worlds spell `step`, `create_body`, `destroy_body`, `has_body`, `set_linear_velocity`, `teleport`, `body_state`, `raycast`, `shape_cast`, `controller_state`, `hash_state`, `stats`, `config` the same |
| Same-binary determinism, 3D poses (physics.md §6 test 13) | `samples/physics3d_headless` + the determinism stage | 3 runs × Debug and Release give one hash (2000 ticks, 134 bodies, a character, a camera boom) |

## Budget table

| Measure | Target | Result |
|---|---|---|
| `sample_physics3d_headless` tick (step + character + camera sweep), 134 bodies, Debug | — | ≈ 0.6 ms |
| Jolt temp allocator | 4 MiB fixed (ADR-0003 §5) | 4 MiB, created with the world |
| Jolt build (132 TUs, 4 cores) | — | ≈ 20 s per tree |

## Open

| Item | State |
|---|---|
| Play-in-editor (embedded host, pause/step/slow-mo, ≤ 1 s enter, Play→Stop→Play hash) | editor work, not in this landing |
| Hot-reload matrix (textures/materials/scenes ≤ 1 s, game.dll ≤ 10 s) | editor work, not in this landing |
| Editor orbit camera collision gate | needs the editor viewport camera; `camera_sweep` is ready for it |
| `04_cubes` capsule walk | waits for the M5a `04_cubes` sample |
| ECS bridge (`install_physics3d`, `RigidBody3D`/`Collider3D`/`CharacterController3D` components, contact events) | next physics step; the facade is shaped for it |
| Cooked mesh colliders (ADR-0003 §9) | runtime-built meshes for now; the cook path replaces them without changing the facade |
| MSVC build of Jolt with these switches | not yet built on Windows (ADR-0018); ADR-0003 §2 records it as open |
