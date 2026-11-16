# M-DRIVEFEEL — camera, driving and roadblock polish in the open-world sample (2026-11-16)

Status: planned. Owner: the routine 3 thread. No Linear issue (the Backlog holds only device-bound work).
Source: defects seen in the `09_openworld` gameplay video of 2026-11-16. Each one is checked against the code below
before a step is built; a defect the code does not support is dropped from the plan, not patched blind.

Modules: `kit` (`effects.cpp` camera follow, `vehicles3d`, `roadblock`), `physics3d` (`camera_sweep`, vehicle input),
the `09_openworld` generator. The character animation work (Animator/Locomotion3D, PR #1268) is routine 2's; this
milestone does not touch those files.

## Findings from the code (verified 2026-11-16)

1. **Camera inside leaves and obstacles.** `CameraFollow3D.collide` is already on in the sample (`gen_assets.py`, the
   Player's camera). `World3D::camera_sweep` casts a sphere against static bodies only (`include_dynamic = false`).
   Street trees are placed "no collider: the crowd walks there" (`street_furniture`), and where a tree has a collider it is
   the trunk only, the Blender canopy is a child without one. So the sweep can never see a canopy, and parked cars and
   props (dynamic) are ignored too. The spring arm works; what it can see is the gap.
2. **Uncontrolled driving.** Input goes straight to `World3D::set_vehicle_input` (forward, right, brake, handbrake, each
   clamped to [-1, 1]) with no speed-dependent steering. Not yet read: what `kit3d_vehicles` maps the stick to, and the
   steer limits in the car prefab. Step 2 starts with that read and a measured scenario before changing anything.
3. **Roadblock not noticeable / not on the player's path.** `Roadblock3D` markers sit at fixed places and spawn when the
   wanted level is at `min_level` and the offender is `spawn_min`..`spawn_max` m away; nothing chooses a marker that the
   offender is heading towards. Step 3 measures how often a block ends up ahead of the player in a scripted chase.

## Steps

1. **Camera sees canopies and parked cars.** Give the camera arm something to hit without making the crowd stop at trees:
   either a camera-only collider layer on canopies (and an option to include dynamic bodies above a mass limit in
   `camera_sweep`), or a cheap sphere approximation per tree. Test: a sweep from inside a canopy shell and past a parked
   car shortens the arm; walking under a tree still does not stop an NPC.
2. **Steering that does not leave the road.** Speed-sensitive steer limit and a small assist setting on the vehicle
   component (off by default for existing scenes), with a test that a scripted full-lock run at speed stays on the road.
3. **Roadblock on the offender's way.** Choose the marker nearest the offender's heading within the spawn window; a test
   with a scripted chase that a block stands ahead, and a visible light/siren cue if the kit already has one to reuse.
4. **Record.** ROADMAP row (the coordinator batches it), the sample notes, and a last eyeball pass on a Mac or a GPU pod.

Each step is its own PR with a test that fails before the change. Limits: no new art, no new Luau API, nothing saved
to scenes except new component fields with defaults that keep today's behaviour.
