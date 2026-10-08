# ADR-0016 — Box2D v3 as the test-time oracle for `mce_physics2d`

**Status:** Accepted (2026-10-08, M3.5).

**Context.** `physics.md` D9 and ARCHITECTURE CF13 make Box2D v3 a test-time behavior oracle for the hand-written
2D solver, in the same dependency class as the DirectXMath math oracle: it may be linked into `test_physics2d` and
nowhere else, and this ADR has to exist before the first include. M3.5 (solver hardening: warm starting, islands,
restitution tuning, platform carry through the solver) is the milestone that needs it.

**Decisions.**

1. **Version and fetch.** Box2D `v3.1.1`, pinned by tag through FetchContent (ADR-0017), static, with its samples,
   unit tests, benchmarks, docs, Tracy profiling and AVX2 off. It is fetched only when `MCE_BUILD_TESTS` and
   `MCE_PHYSICS_ORACLE` (default ON) are both set.
2. **One consumer.** Only `test_physics2d` links `box2d::box2d`, and only `tests/physics2d/oracle/` may name Box2D
   symbols or include `<box2d/...>`: the Box2D wall in `scripts/ci/check_walls.py` fails every other path under
   `src/`, `samples/`, `games/`, `tests/` and `tools/`. Runtime, editor, cook and shipped targets never see it.
3. **What is compared.** Five deterministic scenes are built identically in both engines (same shapes, density 1,
   friction/restitution, gravity, 60 Hz; Box2D at its default 4 substeps, sleeping off) and stepped side by side:
   drop, slide, restitution (e = 0.3 / 0.6 / 0.9), pyramid-lite (a 2-1 pyramid plus a 3-box stack) and platform
   ride (a crate on a kinematic platform that ramps, orbits a 1.5 m circle and stops). Tolerances, chosen from the
   algorithms rather than tuned to pass:

   | Scene | Compared | Tolerance |
   |---|---|---|
   | drop, free fall | height after 0.5 s | exactly the 1-vs-4-substep Euler gap, 3/8 · g · dt · t, ± 1 mm |
   | drop, at rest | resting height, x, angle; both speeds | 1 cm (2 × linear slop), 1 mm, 1e-3 rad; < 1 cm/s |
   | slide | stop distance vs Box2D and vs v² / (2 μ g) | 5 % of the analytic distance |
   | restitution | first rebound apex | 10 % of Box2D's rebound + 2 cm |
   | pyramid-lite | every box's final pose; speeds | 2 cm, 0.02 rad; < 2 cm/s |
   | platform ride | crate offset from the platform | 5 cm; |Δx| < 5 cm (no slip) |

   The two solvers are different algorithms on purpose (ours is Box2D 2.x sequential impulse with NGS position
   correction; v3 is a soft-step solver with substeps and relax/restitution passes), so the oracle checks
   behavior, not bits. It does not replace the hand-computed cases (`test_manifold_cases.cpp`, `test_world.cpp`,
   `test_solver.cpp`), which stay the per-change gate.
4. **Where it runs.** The spec puts oracle diffs in a nightly lane to keep the link time off the per-PR path. There
   is no hosted CI (GitHub Actions was removed), so the oracle runs in every `scripts/ci/local_ci.sh` preset stage;
   Box2D builds in seconds, which makes a separate lane unnecessary for now.

**Consequences.** Solver changes are checked against Catto's production solver as well as against paper. A
contributor without network access to GitHub configures with `-DMCE_PHYSICS_ORACLE=OFF` and loses only the oracle
file. Upgrading Box2D is a tag bump here plus a re-check of the tolerance table.
