# M-PHYSDRIVE2D — pushing 2D bodies from components (milestone record)

**Status:** step 1 in review. **Owner: the "Geliştirme rutini 3" thread.** Defined 2026-10-29 on the coordinator's instruction to take a device-free gap from the assessments when no milestone is free (no gameplay modules, outside M-WASTED and M-VISUAL). No Linear issues (Linear is at its free issue limit); this record is the tracker.
Source: `assessments/motor-erisim-bosluklari.md`, section 5 (Physics, 2D): "raycast/overlap queries, impulse/force and initial velocity are C++ only; `PhysicsConfig` such as gravity is fixed".

**Why.** After M-JOINT2D a scene can build a hinge, but a body can still only be pushed from game C++ (`World2D::apply_impulse`, `set_linear_velocity`), and gravity is the `PhysicsConfig` default. An agent working through MCP or a person in the Inspector cannot launch a ball, add a wind or a conveyor force, or run a low-gravity level.

**Design (step 1).**
- `LaunchVelocity2D` (initial velocity) and `Impulse2D` (a one-shot push in world space, optionally at a point) components in `mce/physics/`, reflected, on a body's entity. The system applies the velocity or impulse on the first tick the body exists, then clears the impulse (`Transient`); both are no-ops on static bodies.
- `ConstantForce2D` (a force in N; no torque, `World2D` has no torque call) applied every tick while present; a body can carry wind or a conveyor push without a system.
- Degrees/units follow the existing components. Nothing here changes bodies without these components, so existing hashes do not move.

**Step 2 (if step 1 lands cleanly).** Gravity and solver settings as project settings (`.mceproj` keys read at `install_physics2d`), with the guide and capabilities rows.

**Not in scope:** raycast/overlap queries (a query is not state, it needs an MCP tool design); 3D bodies; per-body gravity direction.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `LaunchVelocity2D`, `Impulse2D`, `ConstantForce2D` and their system in `physics2d`, with tests, registered in the editor, `mce_mcp` and cook component lists, `capabilities.md`, the user guide | in review |
| 2 | Gravity and solver settings from the project file | todo |

**Gates:** a `test_force2d.cpp` for step 1; capabilities, sample-coverage and the transcript golden by the walls stage. Both steps touch the physics module, so their PRs wait for the coordinator's review.
