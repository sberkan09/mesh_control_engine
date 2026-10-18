# M-SCRIPT4 — Closing the open ends of M-SCRIPT3 (milestone record)

**Status:** active (added 2026-10-18; device-free; no Linear issues: the workspace is at its free limit, this record is the tracker).
Plan: this record (the coordinator adds the ROADMAP status row).

**Why.** M-SCRIPT3 ended with a list of things it wished for or did not test (see "Wished for and open" and each "Step N as built" in
[M-SCRIPT3.md](M-SCRIPT3.md)). Each is small and in the scripting layer, so they are taken in order as separate PRs.

**Not in scope:** new VM features, compiling mod source at load (a separate decision), anything that needs a device.

## Steps

| # | Step | Model | State |
|---|---|---|---|
| 1 | End-to-end test that a `Perceiver` hears a script's `mce.make_noise` (M-SCRIPT3 step 5 open work) | Sonnet | done in PR (this step) |
| 2 | Slot-handle test for a Vec4 / Color array field (done); the ScriptVars demo scripts moved onto named slots (dropped, see "Step 2 as built") | Sonnet | done in PR (this step) |
| 3 | Scene-editable per-script constants (a reflected component the script reads by name, instead of literals in the source) | Sonnet | done in PR (this step) |
| 4 | `mce.overlap_circle2d` / a 2D overlap query without physics, next to `mce.near` | Sonnet | not started |
| 5 | Script projects' ctest target calls `mce test` instead of `demo_gate.py` (M-SCRIPT3 step 9 open work) | Sonnet | not started |

Each step ends with a "Step N as built" section here, a guide/`docs/engine/script.md` update and a `changelog.d/SCRIPT4-<n>-<slug>.md` fragment.

## Step 1 as built (perceiver hears a script noise)

- Test "a perceiver hears a script's mce.make_noise within loudness x hearing" in `test_script_api.cpp`: a script calls `mce.make_noise` at tick 2 from the real
  `ai_perception` system (`install_perception`); a listener at 5 m hears it, one at 30 m does not, one at 30 m with `hearing` 4 does, a deaf one and one on the
  noise's own team do not (`Perceiver.hearings`). No engine code changed; the gap was only the missing test.
- Not done: a behavior tree reacting to `heard_at` after a script noise (the tree side is covered by `test_ai_perception.cpp`).

## Step 2 as built (four-number slot test)

- Test "a slot handle on a four-number array field reads and writes that element" in `test_script_api.cpp`, over a test component with `math::vec4 c[3]`:
  `mce.field("Palette", "c", 2)` reads and writes the second element as four numbers and leaves its neighbours alone. No engine code changed (`script.cpp` untouched, so no Opus review).
- Dropped: moving Dungeon, Pursuit, EmberScript and the missions sample onto named slots. It would change their cooked scripts (pack sizes, determinism baselines) for
  readability only; M-SCRIPT3 step 10 already documents the named-slot idiom for new scripts.

## Step 3 as built (`ScriptParams` and `mce.param`)

- `kit::ScriptParams` (`kit/components.h`, 121 TUs include it, under the 150 warning): `names` (a 96-character string) lists the name of `v[0]`, `v[1]`, ... separated by
  spaces or commas, `v` holds eight numbers. Chosen over eight name fields or an array of structs because a string plus a number array reflect, save and edit
  everywhere `ScriptVars` already does. It carries no `Save` flag: scenes author it, scripts never write it.
- `mce.param(e, name, default?)` in `api_world.cpp` returns the matching number; without a default a missing component or name is a named error; the first eight names count. Declared in
  `mce.d.luau`. Tests: "mce.param reads the scene's named per-entity constants" (two entities, one script; defaults; both errors).
- Shown in `projects/EmberScript`: the enemy's patrol and chase speeds come from a `ScriptParams` on the enemy with the old values as defaults, so behaviour is unchanged
  (the sample coverage gate needs a sample that names the component).
- Not done: a compile-time check of the name literal against the entity's list (it is per entity, so only the run time knows); more than eight values per entity.
