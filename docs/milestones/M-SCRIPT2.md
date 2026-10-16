# M-SCRIPT2 — A Luau API that reaches the whole kit (milestone record)

**Status:** active (added 2026-10-16, step 1 in progress). Plan: ROADMAP §M-SCRIPT2; Linear SBA-190 … SBA-194.

**Why:** a script reads and writes every reflected component, but the things a game does that are not data
(move a body, push, find a path, save, change the time scale, stop a sound) are functions, and `mce` has only about
twenty-six of them. M-PROCGEN found the first hole by accident: `LocalTransform.position` never moved a 2D body, so a
script could not respawn one and EmberScript's respawn silently did nothing. A script-only game is only as capable as
that list, so this milestone audits it against the C++ kit and closes the gaps that games hit.

**Design.** Every new function follows the rules of the existing ones (ADR-0041): a thin binding over a kit or
physics call, deterministic, no hidden state, named errors that say what is missing ("the entity has no 2D body"), a
line in `mce.d.luau` (so `script_check` types it) and in user guide chapter 27, and a test in `tests/script` that
calls it. A function that would need state between ticks is not added: the answer is data (a component) instead.

**Not in scope:** new VM features, coroutines, a networking API for scripts, editor UI.

## Steps

| # | Step | Model | State |
|---|---|---|---|
| 1 | 2D physics completeness: forces and torque, angular velocity, circle / shape overlap and shape casts, gravity and sleep controls | Sonnet | done |
| 2 | Navigation from Luau: `mce.find_path` over a grid built from the static colliders (cached per scene change), line of sight | Sonnet | not started |
| 3 | Game plumbing: save / load a slot, time scale, stop and fade sounds, camera shake, one-shot particles | Sonnet | not started |
| 4 | 3D bodies: overlap, move_and_collide and velocity for 3D, navmesh path | Sonnet (Opus review of anything that touches the physics step) | not started |
| 5 | Demo and gates: Dungeon gets chasing enemies (find_path), a 3D script-only blind test, guide recipes | Sonnet | not started |

**Gates:** every new function has a `tests/script` case and a `mce.d.luau` entry (a test lists the registered functions
against the definitions, so one cannot be added without the other); `local_ci.sh --task` green on every step.

## Step 1 as built

- New in `api_world.cpp`: `force2d`, `angular_velocity2d`, `set_angular_velocity2d`, `awake2d`, `set_awake2d`,
  `set_enabled2d`, `overlap_point2d`, `raycast_all2d`, `circle_cast2d`, `box_cast2d`; `impulse2d` takes an optional world-space
  point. All thin bindings over `World2D`; a missing body is a named error, never a silent no-op.
- The gate the milestone promised: `test_script_api` walks the real `mce` table inside a script and compares it, both ways, with the
  names `mce.d.luau` declares, so a function can no longer be registered without a typed declaration.
- Not done: gravity scale per body is data (`RigidBody2D.gravity_scale`), so no function; `World2D` joints stay C++ only.
