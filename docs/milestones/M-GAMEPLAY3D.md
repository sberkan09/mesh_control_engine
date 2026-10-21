# M-GAMEPLAY3D — 3D gameplay systems (milestone record)

**Scope:** ROADMAP "Engine gap tracks", M-GAMEPLAY3D (added 2026-10-15). Linear SBA-69 … SBA-75, one issue per
step. Off-tier, opt-in, gates nothing. **Measured on:** the Linux CI container, `scripts/ci/local_ci.sh`.

| Step | Linear | State |
|---|---|---|
| 1. Navigation (navmesh, agents, obstacles, debug draw) | SBA-69 | done |
| 2. Joints over Jolt | SBA-70 | done |
| 3. Ragdolls | SBA-71 | done |
| 4. Vehicles | SBA-72 | done |
| 5. Animation (blend spaces, layers, IK, root motion) | SBA-73 | done |
| 6. Sensor queries as data | SBA-74 | done |
| 7. MCP and docs | SBA-75 | done |
| Demo `samples/07_gameplay3d` | SBA-75 | done |

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

## Step 4 — vehicles (SBA-72)

| Item | Where | Evidence |
|---|---|---|
| Wheeled vehicles over Jolt's `VehicleConstraint` and `WheeledVehicleController`: any number of wheels (suspension, steering, brakes, tyre grip), axles through differentials, an engine and an automatic gearbox, a tilt limit; wheels cast cylinders against what the body's layer collides with | `World3D::create_vehicle(VehicleDesc3D)`, `set_vehicle_input`, `vehicle_state` (`mce/physics3d/world3d.h`) | `test_physics3d` vehicles cases: refused descriptions, gone with the body, a 1.2 t car rests on four wheels at one radius, reaches 8+ m/s and 2nd gear in 4 s, stops on its brakes, turns right, reverses; replay hash |
| `Vehicle3D` component: a four-wheel car on a `RigidBody3D`, inputs or the player's Move / Jump / TriggerL, throttle against the motion brakes first, wheel models placed on the wheels | `mce/kit/vehicles3d.h`, systems `kit3d_vehicles` and `kit3d_vehicle_state`, installed by `install_stock`; registered in the editor, MCP and cook | `test_kit` vehicles3d cases: drive, wheel models, brake then reverse, Move steering and the handbrake, rebuild on a settings change, a kinematic body refused, removal, replay hash |
| Docs | user guide 9.9, `docs/engine/capabilities.md` | `capabilities.py --check` |

### Design notes

- **Forward is −Z.** The kit's car faces the body's local −Z like cameras and `NavAgent3D`'s
  `face_motion`; `World3D` takes any forward and up.
- **Grip is the ground's too.** Jolt combines each tyre's friction with the ground body's
  (square root of the product), so a road collider's `friction` is part of the car's feel.
- **Hashes.** `World3D::hash_state` folds vehicles (engine rpm, gear, clutch, each wheel's spin,
  angle, steering and suspension) only once a world has had one.

### Not built (step 4)

- Motorcycles and tracked vehicles (Jolt has both controllers), anti-roll bars, a manual gearbox,
  engine torque curves as data.
- Vehicles with other than four wheels from the component (the physics layer takes any number).

## Step 5 — animation (SBA-73)

| Item | Where | Evidence |
|---|---|---|
| `.mcanim` version 2: `blend2d` states, `root_motion`, `layers` (mask, weight, param, additive, loop, speed), `ik` chains (`two_bone`, `look_at`, `foot`); version 1 files read and write as before | `mce/cook/animgraph_text.h`, `animgraph_importer.cpp` (importer version 2) | `test_kit` "version 2 graphs round-trip": text and chunk identical after a write and read, eight refusals; a plain graph stays version 1 with no extension |
| CHNK_ANIMG extension (minor 1): `AnimGraphExtension`, 2D points, layers, mask bones, IK chains; every index checked | `mce/asset/formats.h`, `AnimGraphView::read` | the same case: the view's spans, a cut chunk refused |
| Local-pose sampling for the pose stack | `render::sample_local`, `sample_node`, `compose_local` (`mce/render/mesh.h`) | single-clip sampling matches `pose_nodes`; `test_render3d` unchanged |
| 2D blend spaces (gradient-band weights, same phase) | `extended_pose`, `run_graph` (`kit/src/anim3d.cpp`) | "a 2D blend space": on a point, between two, between three (a third each) |
| Layers: override and additive, bone masks with subtrees, only the bones the clip animates, weight by parameter, own playheads (`Animator.layer_time`) | `extended_pose` | "layers": the head pitches under a spine mask while the hips stay, half weight gives half the pitch, the additive bob adds its motion; the extract poses through the same path |
| Root motion: the bone's ground motion and turn move the entity, a `CharacterController3D`'s `move`, or are only reported (`root_motion`, `root_velocity`, `root_turn`); the pose keeps the clip's first position and heading | `apply_root_motion`, the root lock in `extended_pose` | "root motion": 2 m/s across a loop, a turned entity walks its forward, report-only and off, a controller carried, a 45 degree turn in half a second |
| IK: two-bone (law of cosines, swing, pole twist, weight), look-at (axis, limit), foot placement (rays down, ground tilt, pelvis drop); `IKTarget3D` goals; system `kit3d_ik` before `kit3d_attach` | `mce/kit/anim3d.h`, `solve_two_bone`, `solve_look_at`, `kit3d_ik` | "two-bone IK": the hand on its goal with 0.4 m bones kept, the elbow on the pole's side, out of reach straight, half weight halfway, an unknown chain; look-at inside and past its 60 degree limit; "foot IK": a 0.2 m step up and down (pelvis 0.2 m lower) |
| Determinism | goals rebuilt every tick in query order | "replay": root motion, a controller, foot and hand IK, same state hash twice |
| Editor | the Animation panel shows 2D blend states and the graph's layers and chains (read-only), and parameter removal keeps their indices | `test_editor` animgraph cases unchanged |
| Docs | user guide 8.9 "Blend spaces, layers, root motion and IK", `docs/engine/kit.md`, `docs/engine/asset.md`, `docs/engine/capabilities.md` | `capabilities.py --check` |

### Design notes

- **Old graphs, old bytes.** A graph without the extras writes version 1 text and the same
  CHNK_ANIMG bytes as before, and an Animator on it takes the old pose path, so existing scenes
  and hashes do not move.
- **One pose for everyone.** `animated_pose` (AttachToBone, ragdolls) and the extract both go
  through `extended_pose`, so a prop attached to a hand follows the IK the renderer draws.
- **Goals are model space, once per tick.** `kit3d_ik` turns IKTarget3D positions and foot rays
  into model-space goals after the animators; the extract's interpolated entity pose reuses them.
- **Feet against the entity's origin.** A foot chain's goal is the animated foot moved by the
  ground's height under it relative to the entity's origin (a character stands on its origin).

### Not built (step 5)

- Editing 2D blend points, layers and IK chains in the Animation panel (they are edited in the
  file; the panel shows them).
- Layers with their own state machines, sync groups, IK chains longer than two bones (FABRIK),
  foot locking during the stance phase, root motion on the vertical axis.

## Step 6 — sensor queries as data (SBA-74)

| Item | Where | Evidence |
|---|---|---|
| `World3D::overlap_sphere` / `overlap_box` (turned): every touching body in creation order, the full count past the span's end, filtered like the other queries | `mce/physics3d/world3d.h`, `world3d.cpp` (Jolt `CollideShape`) | `test_physics3d` "queries: sphere and box overlaps list every touching body in creation order": two side boxes, the floor and the short-span count, dynamic, exclude and layer filters, a 45 degree box, a flat box refused |
| Characters in rays and overlaps (`QueryFilter3D::include_characters`, off by default): capsules tested directly since a `CharacterVirtual` is not a body; a body wins a tie | `ray_capsule`, `character_capsule`, `overlap_shape` | "queries: characters show in rays and overlaps only when asked": the capsule's side at 4.6 m and its top cap, a closer wall winning, sphere and box overlaps on both sides of the edge, excluded |
| `Raycast3D`: a ray or a sphere cast from the entity (entity-space offset and direction), `hit`, `point`, `normal`, `hit_distance`, `hits` | `mce/kit/sensors3d.h`, system `kit3d_sensors` (after `kit3d_triggers`), installed by `install_stock` | `test_kit` "a Raycast3D reports the closest hit…": the hit and its events, turned away, too short, switched off; "a sphere cast hits beside the ray's line…": the cast, a character on the line, `characters: 0` |
| `Overlap3D`: a sphere or a box turned and scaled with the entity, `count`, `enters`, `last_other` | the same | "an Overlap3D counts what touches it…": a crate falling through (one enter, one exit, `PhysicsEvents` on both), a plate seeing the floor, a crate and a character, turned and scaled, removal letting everything out |
| Events: `TriggerEnter3D` / `TriggerExit3D` with the sensor as `trigger`, so `PhysicsEvents` and `FireOn::TriggerEnter` effects see sensors with no new event type | `diff` in `sensors3d.cpp` | "an effect fires on a sensor's entry; a replay hashes the same": a `ParticleBurst` fires once, identical state hash twice |
| A sensor never sees its own entity or its `ChildOf` ancestors | `self_and_parents` | "a sensor never sees itself or what it hangs under": a ray down through its parent hits the floor at 1 m, an overlap inside its parent counts nothing |
| Docs | user guide 8.7 (the two rows and the events) and 9.5 (the 3D queries), `docs/engine/kit.md`, `docs/engine/physics3d.md`, `docs/engine/capabilities.md` | `capabilities.py --check` |

### Design notes

- **Trigger events, not new ones.** A sensor's change is the same `TriggerEnter3D` /
  `TriggerExit3D` a `Trigger3D` pushes, so everything that already listens (`PhysicsEvents`, the
  effect components, game code) works on sensors unchanged.
- **Characters by hand.** Jolt's narrow-phase queries never see a `CharacterVirtual`, so rays and
  overlaps test the capsules analytically (a ray against the cylinder and caps, a sphere by
  segment distance, a box by a ternary search along the capsule's axis). Sphere casts stay bodies
  only.
- **World poses lag one tick.** Sensors read `WorldTransform`, which `transform_propagate` writes
  in `FixedUpdate_Post`; a `LocalTransform` edit reaches the sensor on the next tick, like the
  other kit3d systems.

### Not built (step 6)

- Sphere casts against characters, capsule and box casts, all hits along a ray (`raycast_all` in
  3D), and contact points for overlaps.
- More than 64 entities in one `Overlap3D` (the rest are not tracked).

## Step 7 — MCP and docs (SBA-75)

| Item | Where | Evidence |
|---|---|---|
| `navmesh_bake`: the editor's File > Bake navmesh over MCP; returns the asset, its content hash, tiles, layers, triangles, bytes | `mce_mcp` (`tools.cpp`) | `test_mcp_gameplay3d`: the `.mcnav` is named after the saved scene, and a re-bake has the same content hash |
| `gen_mesh` `humanoid`: a skinned figure (13 bones, rigid skinning, `Idle` and `Walk` clips), centred like the other shapes so a `CharacterController3D` carries it | `tools_content.cpp` (`build_humanoid`, `write_humanoid_glb`) | `test_mcp_content` "gen_mesh: a skinned humanoid…": 13 joints and 2 animations in the `.glb`, it cooks, walks (the Animator's time advances, a prop pinned to `forearm_l` follows) and goes limp with 11 or more parts |
| Schemas: `asset_schema` `AnimGraph` describes version 2 (`blend2d`, `root_motion`, `layers`, `ik`) | `asset_tools.cpp` | `test_mcp` asset_create / asset_write cases (every kind created from its template and cooked) |
| `run_headless` `inspect` of agents, joints, ragdolls, vehicles and sensors: their engine-written fields come back | the existing inspect, documented in user guide 14.2 | `test_mcp_gameplay3d` reads `NavAgent3D.state` / `target`, `Joint3D.position`, `Ragdoll3D.parts`, `Vehicle3D.wheels_on_ground`, `Overlap3D.enters` |
| Gameplay without code for the demo: `NavAgent3D.follow` (chase an entity, re-plan after `follow_repath` metres) and `Ragdoll3D.on_trigger` (limp on a `TriggerEnter3D` naming it, its agent stopped) | `mce/kit/nav3d.h`, `mce/kit/ragdolls3d.h` | `test_kit` "nav3d: an agent follows an entity…", "ragdolls3d: on_trigger goes limp…" |
| Fix: a scene whose `NavMeshSurface3D` points at a baked `.mcnav` failed to cook ("unknown asset type"); the scene importer now knows `NavMesh` | `scene_importer.cpp` | `test_mcp_gameplay3d`: `cook_run` after `navmesh_bake` |
| Docs | user guide 8.8 (gameplay without code), 9.7 (`follow`, `navmesh_bake`), 9.8 (`on_trigger`), 14.2 (`navmesh_bake`, `gen_mesh` humanoid, what inspect reads), 18 (the sample), `docs/engine/physics3d.md`, `docs/engine/capabilities.md` | `capabilities.py --check` |

## Demo — `samples/07_gameplay3d` (SBA-75)

| Item | Where | Evidence |
|---|---|---|
| The arena: a wall with a door hinged in its gap, a platform with a motor drawbridge, a cart (`Vehicle3D`, driven by Move) with a bumper `Overlap3D` on a child, three humanoid grunts (`CharacterController3D` steered by `NavAgent3D` following the cart, `Ragdoll3D` with `on_trigger`), a follow camera, a baked navmesh | `samples/07_gameplay3d/assets/` (no code) | `sample_07_gameplay3d_player_600`: `mce_player` plays the cooked pack headless with `drive.input` (600 ticks, 13 entities, no errors) |
| Built over MCP alone, from an empty project | `tests/mcp/test_mcp_gameplay3d.cpp`; `MCE_GAMEPLAY3D_EXPORT=<dir>` writes the project the sample was copied from | "M-GAMEPLAY3D demo: the 07_gameplay3d arena built over MCP alone": `gen_mesh`, `entity_create`, `navmesh_bake`, `scene_save`, `cook_run`, `run_headless` |
| Behaviour | the same test | after 1 s the grunts are Moving towards the cart, at least 0.8 m apart, and the bridge is lowering; after 5 s of throttle the door has swung past 20 degrees (about 87), the bridge sits at -104 degrees, the cart is through the gap with its wheels down, the bumper has entered at least once and a grunt it hit is a ragdoll of 11 or more parts with its agent stopped |

### Gates

| Gate | Evidence |
|---|---|
| The navmesh bake is byte-identical run to run | `navmesh_bake` twice: the same content hash (`test_mcp_gameplay3d`); `test_kit` nav3d "a bake through the helper is byte-identical" (step 1) |
| Replay hashes hold with joints, ragdolls and vehicles | the demo run twice in `run_headless` gives one hash; `scripts/ci/local_ci.sh` determinism stage plays `07_gameplay3d` 3 times each in Debug and Release (`95583ae52a5b13e4`, all six equal); the replay cases of steps 2–6 |
| The 2,000-body budget still holds | `test_physics3d` "budget3d: 2,000 dynamic bodies step within budget": 0.47 ms per step in clang Release (budget 3 ms); FR-PHY-050's 2D budget unchanged |
| `local_ci.sh` green on every step | `--task` before each PR; `--batch` after steps 3 and 6 and at the milestone's end |

### Design notes

- **One scene, three routes.** The sample's files are the MCP-built project copied out, `mce_player`
  plays them, and the test rebuilds them from nothing on every ctest run, so the MCP route cannot
  rot behind the files.
- **Death without code.** `on_trigger` reuses the trigger events every sensor already raises, so
  a trap (`Trigger3D`), a bumper (`Overlap3D`) or a shot (`Raycast3D`) kills the same way.
- **Layers keep a ragdoll off its own capsule.** The grunts' controllers are on layer 1 and ignore
  layer 2; their ragdolls are on layer 2 and ignore layer 1.

### Not built (step 7)

- Editor UI for the humanoid's proportions and clips beyond Idle and Walk; a get-up after a ragdoll.
- Cart wheel models in the sample (the wheels are simulated, not drawn).
- A Windows/MSVC or macOS run of the demo: it runs only in the Linux CI container (no Mac check
  was made; see the Linear issue).
