# M-JOINT2D — 2D joints as components (milestone record)

**Status:** step 1 in review. **Owner: the "Geliştirme rutini 3" thread.** Defined 2026-10-28 on the coordinator's instruction to take a device-free gap from the assessments when no milestone is free (no gameplay modules, outside M-WASTED and M-VISUAL). No Linear issues (Linear is at its free issue limit); this record is the tracker.
Source: `assessments/motor-erisim-bosluklari.md`, section 5 (Physics): "2D joints (`world2d.h`) are C++ only"; `docs/user-guide/09-physics.md` still says "2D joints are not scene-authored yet".

**Why.** `World2D` has distance joints (a rod, a rope when `min_length < max_length`, a spring when `hertz > 0`) and revolute joints (a hinge with an angle limit and a motor), and the 3D side got `Joint3D` as a component in M-GAMEPLAY3D. In 2D the only way to build a swinging door, a rope bridge, a pendulum or a motorized wheel is game C++, so an agent working through MCP and a person in the Inspector cannot make one. The assessment lists this as one of the 2D physics gaps still open (contact events, queries and the character controller have since been opened by other milestones).

**Design (step 1).**
- `Joint2D` component in `mce/physics/components.h` beside `RigidBody2D`, reflected, on a body's entity: `connected` (the other body's entity: `World2D` needs two bodies, at least one dynamic, so a joint to the world is made with a static body), `kind` (Distance, Revolute), the two local anchors, `length`/`min_length`/`max_length`/`hertz`/`damping_ratio` for Distance, `limits`/`lower`/`upper` (degrees) and `motor_speed` (deg/s)/`motor_max_torque` for Revolute, `collide_connected`, `enabled`, and read-only `length_now` / `angle_now`.
- A system in `install_physics2d` builds the joint the first tick both bodies exist, rebuilds it when a setting changes, destroys it with the component, and mirrors `joint_length` / `joint_angle` back. The motor speed and the limits update in place (`set_motor_speed`, `set_joint_limits`) without a rebuild.
- Degrees in the component and radians in the world, the unit `Joint3D` uses.
- Prismatic, wheel, weld and mouse joints stay unbuilt (`World2D` does not have them).

**Not in scope:** new joint kinds in `World2D`; breakable 2D joints; a scene gizmo for anchors (the debug-lines overlay can draw them later).

## Steps

| # | Step | State |
|---|---|---|
| 1 | `Joint2D` component and its system in `physics2d`, with tests (rod, rope, spring, hinge with limit and motor, rebuild on change, removal with the component or a body) | in review |
| 2 | Registered in the editor and `mce_mcp` component lists, `capabilities.md` and the user guide (the "not scene-authored yet" line), a sample scene (a rope bridge and a swinging door) | todo |

**Gates:** `test_joints2d.cpp` (step 1); the capabilities row and the guide are checked by the walls stage (step 2). Both steps touch the physics module, so their PRs wait for the coordinator's review.
