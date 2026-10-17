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
| 2 | Navigation from Luau: `mce.find_path` over a grid built from the static colliders (cached per scene change), line of sight | Sonnet | done |
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

## Step 2 as built

- `mce.find_path(from, to, options?)` in `api_world.cpp`: a `kit::NavGrid` over the bounds of the static colliders plus a free margin,
  marked with `kit::mark_static_colliders`, searched with `kit::Pathfinder`. Options `cell`, `radius`, `diagonal`, `smooth`; bad values are
  named errors, and a grid over 4 million cells asks for a bigger `cell`.
- The grid is a world resource keyed by `kit::static_colliders_hash` (new, in `pathfinding.cpp`) plus cell and radius, so a level that
  changes (a door destroyed, a ProcLevel reroll) is noticed on the next call and nothing else rebuilds it.
- Points outside the grid are open ground: the search runs from the nearest edge cell and the real point is added at that end.
- Line of sight needs no new call: `mce.raycast2d` / `circle_cast2d` already answer it, and `smooth = true` already drops the waypoints
  a straight walk does not need.

## Step 0: the bytecode trust boundary (SBA-198)

**Finding (M-FUZZ 3):** `luau_load` / `ScriptHost::add_bytecode` do not survive hostile bytecode (debug: `lvmload.cpp:691` assert;
release: out-of-bounds constant read). The pack CRC only detects corruption.

**Decision (coordinator):** no verifier of our own (upstream Luau treats bytecode as trusted too). Bytecode is trusted only when
the game's own build cooked it.

**Design.**
- `asset::MountOptions::allow_code` (default **false**, so a new mount site is safe by default). A root mounted without it has its `Script` manifest entries
  dropped at mount, with a warning: they neither load nor override the game's script of the same GUID.
- Trusted (`allow_code = true`): the player's game pack, the editor project's own cook, `run_headless`'s `pack` (every caller
  passes the project's cooked folder). Data-only: mod packs (ADR-0034), base packs (a mod project's game), liveops fetched config.
- `add_bytecode` documents that it trusts its input; `compile()` / `add_source` is the path for anything else.
- Not done on purpose: compiling user or mod *source* at load time. The shipped player has no compiler by design
  (`player_no_luau_compiler`), so a mod that wants scripts needs that decision first (a separate milestone).

**Test:** `tests/asset/test_mods.cpp` "a mod pack cannot carry script bytecode": a mod's new and overriding Script entries never
enter the table. The `MCE_FUZZ_BYTECODE=1` case calls `add_bytecode` directly (a trusted caller) and stays opt-in; the
mutated-bytecode route that matters in the product is now closed before it reaches `luau_load`.

## Step 3 as built

- `mce.save(slot)` writes now (`save::save_game`, a read of the world, safe mid-tick). `mce.load(slot)` queues: `SceneChange::load_slot` is
  applied by `scene::apply_change`, the one seam the player, the headless run and the editor's play mode already call after a tick,
  so no host changed. A bad slot is logged and the world is left alone; the host's tick counter is not restored (only the player
  does that, at startup). Slot names are validated, so a script cannot reach another path.
- `mce.set_time_scale` / `time_scale` wrap the `time_scale` cvar, clamped to 0.1..4 (0 would freeze the loop and the script with it).
- `mce.shake` and `mce.burst` are thin over `CameraShake2D/3D` and `ParticleBurst`.
- Sound stop and fade: no new call; destroying an `AudioSource` entity already fades its voice out (`fade_s`), documented as a recipe.
