# M-PHYSDRIVE2D — pushing 2D bodies from components (milestone record)

**Status:** done (both steps merged). **Owner: the "Geliştirme rutini 3" thread.** Defined 2026-10-29 on the coordinator's instruction to take a device-free gap from the assessments when no milestone is free (no gameplay modules, outside M-WASTED and M-VISUAL). No Linear issues (Linear is at its free issue limit); this record is the tracker.
Source: `assessments/motor-erisim-bosluklari.md`, section 5 (Physics, 2D): "raycast/overlap queries, impulse/force and initial velocity are C++ only; `PhysicsConfig` such as gravity is fixed".

**Why.** After M-JOINT2D a scene can build a hinge, but a body can still only be pushed from game C++ (`World2D::apply_impulse`, `set_linear_velocity`), and gravity is the `PhysicsConfig` default. An agent working through MCP or a person in the Inspector cannot launch a ball, add a wind or a conveyor force, or run a low-gravity level.

**Design (step 1).**
- `LaunchVelocity2D` (initial velocity) and `Impulse2D` (a one-shot push in world space, optionally at a point) components in `mce/physics/`, reflected, on a body's entity. The system applies the velocity or impulse on the first tick the body exists, then clears the impulse (`Transient`); both are no-ops on static bodies.
- `ConstantForce2D` (a force in N; no torque, `World2D` has no torque call) applied every tick while present; a body can carry wind or a conveyor push without a system.
- Degrees/units follow the existing components. Nothing here changes bodies without these components, so existing hashes do not move.

**Step 2.** Dropped as a feature: gravity is already a scene component, `WorldSettings.gravity_2d` (M-DATA-REACH 3, applied by the kit), and the solver counts stay `PhysicsConfig` defaults. Step 2 instead closes the two follow-ups from the coordinator's review of step 1: `applied` is a save-game field (`Save`), so a load does not launch the body again, and a zero `ConstantForce2D` skips `apply_force` so it no longer wakes a sleeping body.

**Not in scope:** raycast/overlap queries (a query is not state, it needs an MCP tool design); 3D bodies; per-body gravity direction.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `LaunchVelocity2D`, `Impulse2D`, `ConstantForce2D` and their system in `physics2d`, with tests, registered in the editor, `mce_mcp` and cook component lists, `capabilities.md`, the user guide | done |
| 2 | Follow-ups from the review of step 1 (launch flag in save games, zero force skipped); gravity needs nothing, `WorldSettings` has it | done |

**Gates:** a `test_force2d.cpp` for step 1; capabilities, sample-coverage and the transcript golden by the walls stage. Both steps touch the physics module, so their PRs wait for the coordinator's review.
