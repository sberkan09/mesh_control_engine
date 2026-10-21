# M5b — Physics 3D + play-in-editor + hot reload (milestone record)

**Status:** the physics half (ADR-0003 + `mce_physics3d`) and the editor half (ADR-0029: play-in-editor, the
hot-reload matrix, the editor orbit-camera collision gate) are merged into `dev_claude`. The
`04_cubes` capsule walk landed on top of the M5a renderer sample. See *Open* below.
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
| `04_cubes` capsule walk (ROADMAP M5b demo) | `samples/04_cubes` (`-walk 1`, or `C` in the window) | a capsule (`walker.glb`, the default `ControllerParams3D` shape) walks the floor through `move_character` against a `World3D` mirror of the layout (floor, 28 panel cubes, 14 spheres, orrery stand), hops every 2.5 s, and a 5 m third-person boom is `camera_sweep`-ed every tick: 600 ticks walk 40 m with 105 pulled-in boom ticks (closest 0.53 m). Arrows walk camera-relative, Space jumps, no input runs the patrol. `sample_04_cubes_walk_300` ctest requires a pulled-in boom; the determinism stage adds `-walk 1 -ticks 600` (World3D hash + boom folded in) across 3 runs × Debug/Release. Walk off keeps the M5a hash (`a31c44f2772f1e8f` at 600 ticks) |

## Editor half (ADR-0029)

| ROADMAP M5b item | Where | Evidence |
|---|---|---|
| Play-in-editor: embedded host, pause / step / slow-mo (FR-EDT-060) | `mce_editor_core`: `PlaySession` (`mce/editor/play_session.h`), `cook::cook_document`; layer: Play panel, menu-bar Play/Pause/Step/Stop (F5/F6/F10), viewport through the game camera | `test_editor` "play:" cases; `editor_play_smoke` (GPU): the scene runs in the viewport for 90 frames, document untouched |
| Play enter ≤ 1 s (NFR-M-03) | in-memory cook + `scene::instantiate`, no save, no pack | `scene_demo` (6 entities): 1.1 ms, Debug |
| Play→Stop→Play reproduces the tick-0 hash | the play world is rebuilt from the document each time; nothing writes the document while playing | "play: Play -> Stop -> Play…": same post-instantiate, first-tick and 300-tick hashes |
| Same world as the shipped runtime | `mce_player -headless`'s setup (physics2d, audio bridge on a Silent device, `scene::instantiate`), the app loop's fixed step | "play: enter builds the player's world…": 600 ticks in the editor = `mce_player -headless 1 -ticks 600` on the cooked pack |
| Hot reload: textures / materials / scenes ≤ 1 s (FR-AST-090) | `mce_editor_core`: `AssetWatcher` (`mce/editor/hot_reload.h`), `EditorProject::reload` → `AssetSystem::commit_reload`; layer: polled every `editor.hot_reload_interval` s, File → Hot reload now | `test_editor` "hot reload:" cases: texture 2.7 ms, material 6.4 ms, scene 6.3 ms (Debug); a broken texture keeps the previous output; play keeps running across a swap |
| Hot reload: game module ≤ 10 s | module host (`mce_devhost`), ADR-0029 §2.6 | `scripts/module_reload_stopwatch.py build/linux-gcc-debug`: `03_platformer` edit → rebuild + load + tick, median 2.66 s, worst 2.71 s |
| Editor orbit camera collision gate (`World3D::camera_sweep`) | `mce_editor_core`: `OrbitCamera::resolve`, `OrbitColliders` (`mce/editor/orbit_camera.h`); layer: View → 3D orbit view (O), Orbit camera collision, Collider depth | `test_editor` "orbit camera:" cases: the boom stops in front of a wall (eye at x = −2.3 for a face at −2.5, 0.2 m probe) and above the floor, slides back out when the wall moves, sensors never stop it |

## Budget table

| Measure | Target | Result |
|---|---|---|
| `sample_physics3d_headless` tick (step + character + camera sweep), 134 bodies, Debug | — | ≈ 0.6 ms |
| `04_cubes -walk 1` physics tick (move + step + camera sweep), 45 static bodies, Debug | — | ≈ 0.06 ms |
| Jolt temp allocator | 4 MiB fixed (ADR-0003 §5) | 4 MiB, created with the world |
| Jolt build (132 TUs, 4 cores) | — | ≈ 20 s per tree |

## Open

| Item | State |
|---|---|
| Play-in-editor (embedded host, pause/step/slow-mo, ≤ 1 s enter, Play→Stop→Play hash) | shipped, see *Editor half*; game modules did not load inside the editor process at M5b (ADR-0029 §7); *since superseded: the stock editor loads a module with `-module` (ADR-0029 amendment)* |
| Hot-reload matrix (textures/materials/scenes ≤ 1 s, game.dll ≤ 10 s) | shipped, see *Editor half* (ADR-0029 §2) |
| Editor orbit camera collision gate | shipped, see *Editor half* (ADR-0029 §3) |
| ECS bridge (`install_physics3d`, `RigidBody3D`/`Collider3D`/`CharacterController3D` components, contact events) | next physics step; the facade is shaped for it |
| Cooked mesh colliders (ADR-0003 §9) | runtime-built meshes for now; the cook path replaces them without changing the facade |
| MSVC build of Jolt with these switches | not yet built on Windows (ADR-0018); ADR-0003 §2 records it as open |
