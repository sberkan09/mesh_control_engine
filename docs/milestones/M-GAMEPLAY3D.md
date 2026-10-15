# M-GAMEPLAY3D — 3D gameplay systems (milestone record)

**Scope:** ROADMAP "Engine gap tracks", M-GAMEPLAY3D (added 2026-10-15). Linear SBA-69 … SBA-75, one issue per
step. Off-tier, opt-in, gates nothing. **Measured on:** the Linux CI container, `scripts/ci/local_ci.sh`.

| Step | Linear | State |
|---|---|---|
| 1. Navigation (navmesh, agents, obstacles, debug draw) | SBA-69 | done |
| 2. Joints over Jolt | SBA-70 | done |
| 3. Ragdolls | SBA-71 | done |
| 4. Vehicles | SBA-72 | not started |
| 5. Animation (blend spaces, layers, IK, root motion) | SBA-73 | not started |
| 6. Sensor queries as data | SBA-74 | not started |
| 7. MCP and docs | SBA-75 | not started |
| Demo `samples/07_gameplay3d` | — | not started |

## Step 1 — navigation (SBA-69)

| Item | Where | Evidence |
|---|---|---|
| Recast & Detour v1.6.0 embedded (`mce_recast` object library, `-ffp-contract=off`) behind a wall | `cmake/Dependencies.cmake`, `src/engine/physics3d/CMakeLists.txt`, ADR-0040; "Recast wall" and "Recast link wall" in `check_walls.py` | `check_walls.py`: all walls hold |
| Bake: tiled, layered, area volumes, off-mesh links, a `.mcnav` blob | `physics::bake_navmesh` (`mce/physics3d/navmesh3d.h`) | `test_physics3d` navmesh cases: two bakes byte-identical; malformed blobs rejected |
| Queries: nearest point, area, path (complete / partial), surface raycast, area costs | `NavMesh3D` | paths around a wall, through a costly area only when cheaper, holes from area-0 volumes, links, stacked storeys |
| Obstacles carved at run time (turned boxes too) | `NavMesh3D::add_obstacle` / `remove_obstacle` (DetourTileCache) | obstacle cases: path blocked, re-opened when removed |
| Crowd: path following, avoidance, arrival, stop, teleport, replay hash | `NavMesh3D` agents (DetourCrowd) | crowd case: agents arrive and keep apart, identical hash on a second run |
| `.mcnav` asset type | `asset::NavMesh` (`CHNK` type `NavMesh`), the `navmesh` importer | the cook passes `.mcnav` through after checking its header |
| Components | `NavMeshSurface3D`, `NavArea3D`, `NavLink3D`, `NavObstacle3D`, `NavAgent3D`, the `Nav3D` resource, system `kit3d_nav` (`mce/kit/nav3d.h`); installed by `install_stock` | `test_kit` nav3d cases: walk around a wall, steer a `CharacterController3D`, costs, obstacles, links, replay hash, runtime bake identical to the helper bake |
| Editor | File > Bake navmesh (`EditorSession::bake_navmesh`, writes `navmesh/<scene>/nav_<id>.mcnav` with a sidecar, one undo step); the orbit view draws the mesh and links (`NavMeshPreview`) | `test_editor` navmesh bake case: bake from a box and a glTF mesh collider, preview segments, `show = 0`, undo, GUID kept on re-bake |
| Docs | user guide 9.7, ADR-0040, `docs/engine/capabilities.md` | `capabilities.py --check` |

### Design notes

- **Where the bake happens.** The ROADMAP text said "baked at cook time". As built, the editor (and, at step 7,
  MCP's `navmesh_bake`) bakes into a `.mcnav` asset that the cook only validates and passes through, and a scene
  with no baked asset bakes from its colliders when it starts. Baking in the editor keeps the cook free of the ECS
  and lets the preview show exactly what ships.
- **Arrival in a crowd.** Agents sent to one point push each other apart, so an agent counts as arrived within
  `arrive_distance`, or within 2 × radius once it has all but stopped.
- **No visibility shortcut.** Detour's `DT_CROWD_OPTIMIZE_VIS` raycast ignores area costs, so agents only use the
  topology optimisation; paths through costly areas stay costed.
- **Determinism.** Same bake bytes per platform and binary (the tile-cache header is zeroed before it is filled);
  the crowd runs on the caller's thread with the tile cache fully updated before each step.

### Not built (step 1)

- Sliced path queries, per-agent filters, cylinder obstacles, tile streaming (ADR-0040 §7).
- Terrain as bake input waits on M-TERRAIN.

## Step 2 — joints (SBA-70)

| Item | Where | Evidence |
|---|---|---|
| Six joint types over Jolt constraints: fixed, hinge, slider, distance, cone, six-DOF; to another body or the world | `World3D::create_joint(JointDesc3D)` (`mce/physics3d/world3d.h`) | `test_physics3d` joints cases: refused descriptions, joints destroyed with a body, ids not reused |
| Limits (hard and spring), friction, velocity and position motors with a force cap | `JointDesc3D`, `set_joint_motor` | hinge stops at its limit; a velocity motor spins a wheel at 2 rad/s; position motors reach and hold their targets; a slider stays on its rail |
| Break force and torque; switching a joint off and on | `World3D::step` checks every joint after the solve, in creation order; `set_joint_enabled` | a 200 kg bar breaks a 1 kN fixed joint and falls; a 10 kN one reports 1962 N and 981 N·m; a leash switched off lets go and catches again |
| Joined bodies do not collide unless asked | a group filter over body pairs (`collide_connected`) | two overlapping glued boxes fall as one; with `collide_connected` they push apart |
| `Joint3D` component and `JointBreak3D` event; systems `kit3d_joints`, `kit3d_joint_state` | `mce/kit/joints3d.h`, installed by `install_stock`; registered in the editor, MCP and cook | `test_kit` joints3d cases: a door within its limits, a drawbridge raised and lowered by its motor, rebuild on a settings change, removal with a body or the component, one break event, replay hash |
| Editable in the inspector | reflected fields with units in their help text | the inspector and MCP `component_set` see `Joint3D` like any reflected component |
| Docs | user guide 9.6, `docs/engine/capabilities.md` | `capabilities.py --check` |

### Design notes

- **Angles belong to the joint's own body.** Jolt measures body 2 against body 1, so `connected` (or the
  world) is Jolt's body 1: a hinge's angle and a slider's position are how far the `Joint3D`'s body has
  turned or slid, and a positive motor target turns it the right-handed way about `axis`.
- **Breaking is ours.** Jolt has no breakable constraints; after each step the world reads each joint's
  solved impulses (motors excluded), divides by the step, and switches the joint off past its rating.
- **Hashes.** `World3D::hash_state` folds joints only once a world has had one, so every existing golden
  hash stays as it was.

### Not built (step 2)

- Six-DOF motors, cone twist limits, gear, pulley and path joints.
- The editor draws no joint gizmo yet (anchor and axis are edited as numbers).

## Step 3 — ragdolls (SBA-71)

| Item | Where | Evidence |
|---|---|---|
| Ragdolls over Jolt's `Ragdoll`: capsule, sphere and box parts in a tree, swing-twist joints, neighbours and overlapping parts filtered, one collision group per ragdoll | `World3D::create_ragdoll(RagdollDesc3D, parts)`, `ragdoll_state`, `ragdoll_impulse`, `ragdoll_active` (`mce/physics3d/world3d.h`) | `test_physics3d` ragdolls cases: refused descriptions, ids not reused, a shoved figure falls, holds its pivots within 2 cm and sleeps, the swing limit at 0.3 and 1.0 rad, an impulse spread by mass, queries report the owner, ragdolls collide with each other |
| Per-bone overrides in the glTF sidecar | `"ragdoll": { "<bone>": { exclude, include, radius, swing, twist, mass } }`; `MeshFlags::Ragdoll`, `MeshRagdoll`, chunk minor 3 in reserved `MeshJoint` fields; `GpuJoint` carries them | `test_cook` gltf ragdoll case (values land on the named joints, unknown bones and bad values fail); `test_render` skin case (overrides need the flag, unknown bits refused) |
| `Ragdoll3D` component: parts from skin 0, animation to physics with the animation's velocities, physics to animation over `blend_time`, impulses on a bone, `root_position` and `settled` | `mce/kit/ragdolls3d.h`, systems `kit3d_ragdolls` and `kit3d_ragdoll_pose`, installed by `install_stock`; registered in the editor, MCP and cook | `test_kit` ragdolls3d cases: a 7-part stick figure (its socket rides the head) falls forward and lies on the floor, a hit on the head, the blend back to the rest pose in 15 steps, sidecar overrides, a mesh without a skeleton refused, removal, replay hash |
| The mesh follows the bodies | `Ragdoll3DPoses` blended over the Animator's pose in the extract (skinning) and `AttachToBone` | the kit cases read bone positions through `AttachToBone` |
| Docs | user guide 9.8, 5 (sidecar key), `docs/engine/asset.md`, `docs/engine/capabilities.md` | `capabilities.py --check` |

### Design notes

- **Jolt's ragdoll, our parts.** `World3D` takes parts in world space and builds Jolt's
  `RagdollSettings` (skeleton, `Stabilize`, `DisableParentChildCollisions` with the creation pose), so
  the physics layer never sees a mesh. The kit turns the skeleton into parts.
- **Shapes from the skin.** Each bone's part is sized from the bind-space bounds of the vertices it
  moves, so no authoring is needed for a typical humanoid; the sidecar only corrects outliers. Bones
  that move no vertex (sockets) and very short bones ride their parent.
- **The entity stays put.** The ragdoll poses the mesh in model space relative to the entity, whose
  transform is not touched; games read `root_position` to move it before getting up. This keeps
  controllers, triggers and cameras that follow the entity predictable.
- **Hashes.** As with joints, `World3D::hash_state` folds ragdolls only once a world has had one.

### Not built (step 3)

- Powered ragdolls (motors driving toward the animation, active ragdolls) and partial ragdolls
  (one limb limp while the rest animates).
- Skins other than skin 0, and per-part collision layers.
- An editor gizmo for the parts (the editor sees `Ragdoll3D` as numbers).

