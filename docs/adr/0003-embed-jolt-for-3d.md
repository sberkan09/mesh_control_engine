# ADR-0003 — Embed Jolt Physics 5 for 3D (`mce_physics3d`)

**Status:** Accepted (2026-10-08, M5b). Written before the first `JPH::` include, as ROADMAP M5b's gate and
`physics.md` §7 require.

**Context.** MCE hand-writes its 2D physics (`mce_physics2d`, physics.md D1) and does not hand-write a 3D
rigid-body engine (research/physics.md D2). Ashen Court (UC-B, FR-PHY-080) needs a capsule character that walks
slopes and steps without clipping walls, and a camera boom that never enters geometry. That is capsule-vs-trimesh
sliding with internal-edge rejection, step-up and stick-to-floor: the problem class Jolt's `CharacterVirtual`
already solves and documents. REQUIREMENTS N3 pre-approves Jolt 5 as the one embedded 3D exception; this ADR
records the terms of the embed.

**Adoption triggers** (research/physics.md D3b). The embed is justified once a 3D prototype needs (a) static
concave level geometry (trimesh), (b) a kinematic character controller against meshes, or (c) compound shapes.
M5b's demo (a capsule walking `04_cubes`' floor with `camera_sweep` collision) and M7's Ashen Court fire (a) and
(b), so the embed lands now. Nothing outside `mce_physics3d` may depend on it.

## Decisions

1. **Version and fetch.** Jolt Physics `v5.6.0`, pinned by tag through FetchContent (ADR-0017) in
   `cmake/Dependencies.cmake`, built static through Jolt's own `Build/CMakeLists.txt`. That file changes
   `CMAKE_CXX_FLAGS` only inside its own directory scope, so it cannot leak into MCE targets. Upgrading is a tag
   bump plus a re-run of the verification below, recorded here.

2. **Build flags (the due-diligence item, physics.md §3.7).** Every Jolt switch that would fight the compile policy
   in `cmake/MceTarget.cmake` is set explicitly:

   | Switch | Value | Why |
   |---|---|---|
   | `CPP_EXCEPTIONS_ENABLED`, `CPP_RTTI_ENABLED` | OFF | runtime binding: `-fno-exceptions -fno-rtti` / `/EHs-c- /GR-` |
   | `CROSS_PLATFORM_DETERMINISTIC` | ON | `-ffp-contract=off` (`/fp:precise` on MSVC), no FMA: ADR-0007 |
   | `USE_FMADD`, `USE_AVX*`, `USE_LZCNT/TZCNT`, `USE_F16C` | OFF | SIMD stops at SSE4.2 (any x86-64 since 2008); no `/arch:AVX*` in the determinism config |
   | `OVERRIDE_CXX_FLAGS`, `INTERPROCEDURAL_OPTIMIZATION` | OFF | our presets own optimization levels; no LTO surprises |
   | `USE_STATIC_MSVC_RUNTIME_LIBRARY` | OFF | one CRT across the engine |
   | `FLOATING_POINT_EXCEPTIONS_ENABLED`, `USE_ASSERTS` | OFF | no FP traps; Jolt asserts stay out (ours guard the facade) |
   | `DEBUG_RENDERER_*`, `PROFILER_*`, `ENABLE_OBJECT_STREAM` | OFF | debug draw goes through MCE's sink; Tracy is our profiler; no RTTI-style streams |
   | `JPH_USE_DX12/VK/MTL/CPU_COMPUTE` | OFF | no GPU compute in a physics library that must be headless |
   | `DOUBLE_PRECISION` | OFF | levels are tens of metres; single precision matches the rest of the engine |
   | `OBJECT_LAYER_BITS` | 32 | MCE's 16-bit layer group + 16-bit mask packed into one `ObjectLayer` (decision 4) |

   **Verification (2026-10-08):** with these switches all 132 Jolt sources compile cleanly under gcc 13 with
   `-fno-exceptions -fno-rtti -ffp-contract=off` (Debug and RelWithDebInfo) and under clang 18 (RelWithDebInfo,
   and the clang-tidy tree), and the ASan+UBSan preset runs the physics3d tests clean. Jolt's own warnings are
   silenced (`-w` / `/W0`) because its headers are `SYSTEM` and its sources are not ours to fix. MSVC has not
   been built yet (no Windows machine in the loop, ADR-0018); the same switches are the MSVC ones by design, and
   the first Windows build must confirm it here.

3. **One consumer, behind a facade.** Only `mce_physics3d` links `Jolt` (privately, so Jolt's include paths and
   its `-msse4.2` never reach dependents), and only `src/engine/physics3d/` may include `<Jolt/...>` or name
   `JPH::`: the JPH wall in `scripts/ci/check_walls.py` fails every other path under `src/`, `samples/`, `games/`
   and `tests/`. The public header `mce/physics3d/world3d.h` contains no Jolt type: `World3D` is a pimpl over MCE
   math types (`vec3`, `quat`) and `Entity`, and mirrors `World2D`'s method spelling (`step`, `create_body`,
   `destroy_body`, `set_linear_velocity`, `teleport`, `raycast`, `shape_cast`, `hash_state`, `stats`) plus the two
   3D additions `move_character` and `camera_sweep`. A compile-time test in `tests/physics3d` names both worlds'
   shared methods so the spelling cannot drift (physics.md §2.2). Body ids never leave the facade.

4. **Layers.** MCE's collision model (16 layers, a per-body layer index and 16-bit mask, as in 2D) is packed into
   Jolt's 32-bit `ObjectLayer`: layer index in bits 0–3, a moving bit (bit 4), the mask in bits 16–31. Three small
   filter classes inside `world3d.cpp` decode it: two broad-phase layers (static, moving), statics never tested
   against statics, and a pair collides only when each one's layer is in the other's mask. Queries take a
   `QueryFilter3D` (layer mask, exclude list, dynamic bodies on/off) that mirrors the 2D `QueryFilter`;
   unifying the two into one shared type needs an edit to `mce_physics2d` and is left for the ECS bridge work.

5. **Simulation contract.** `World3D::step(dt)` asserts `dt == config.fixed_dt` (1/60 s) and steps with one
   collision step on `JobSystemSingleThreaded`. The `TempAllocator` is a fixed 4 MiB `TempAllocatorImpl` created
   with the world; Jolt's general allocations (`Allocate`/`Free`/`AlignedAllocate`/`Reallocate`) are routed to
   `mce::mem::heap()`, so heap leak counters see them. Jolt's global state (allocator hooks, `Factory`, type
   registration) is reference-counted by live `World3D` instances and torn down with the last one; worlds are
   created and destroyed on the main thread only.

6. **Character controller.** `move_character(e, delta, params)` drives a `CharacterVirtual` with
   `velocity = delta / fixed_dt` through `ExtendedUpdate` (stick-to-floor and walk-stairs from `params`), then
   reports position, ground state, ground normal, ground entity and ground velocity as `MoveResult3D`, shaped like
   `MoveResult2D`. As in 2D, gravity, jump, coyote time and input belong to gameplay: the caller folds them into
   `delta`. Characters are not rigid bodies: they collide with the world and with each other
   (`CharacterVsCharacterCollisionSimple`), are not hit by queries, and push dynamic bodies through Jolt's
   character contact impulses.

7. **Camera sweep.** `camera_sweep(from, to, radius)` casts a sphere along the boom against static and kinematic
   geometry (dynamic props and characters do not pull the camera in) and returns the first hit's fraction, point,
   normal and the safe sphere centre. The caller places the camera at that centre.

8. **Determinism.** Same binary, same machine, single-threaded: identical inputs give identical
   `World3D::hash_state` (positions, rotations, velocities of every body in creation order, then every
   character's position, velocity and ground state). With `CROSS_PLATFORM_DETERMINISTIC` the Debug and Release
   trees also agree, which the determinism stage checks with `sample_physics3d_headless`. Cross-machine
   determinism is not claimed (same narrow definition as FR-RUN-040).

9. **Not in this ADR's first landing.** Cooked mesh colliders: physics.md wants `MeshShape`/convex hulls built in
   the cook tool and deserialized at runtime. Until the cook side lands, `World3D` accepts triangle meshes and
   convex point sets built at body creation (tests, samples and the editor scratch scene only); the cook path
   will replace it without changing the facade. Also deferred: the ECS bridge (`install_physics3d`, components,
   contact events), debug draw, and sensors.

## Plan B (escape hatch)

If a Jolt upgrade or the MSVC build fights the toolchain, the fallback is a hand-written kinematic-only 3D capsule
controller over `World3D`'s shape-cast API (the 2D `move_and_collide` algorithm lifted to 3D). It satisfies
FR-PHY-080's letter; it gives up dynamic props (FR-PHY-090, Could). Because gameplay only sees the facade, the
swap stays inside `src/engine/physics3d/`.

## Consequences

The engine gains one large embedded dependency (≈ 130 translation units, ~20 s on 4 cores), fetched in every
configuration because `mce_physics3d` is a runtime library. The surface MCE owns is the facade (~20 functions)
plus the layer mapping; an API break in a future Jolt is a bounded patch to one target.
