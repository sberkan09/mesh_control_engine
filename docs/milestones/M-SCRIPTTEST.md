# M-SCRIPTTEST — Checks a script-driven game could not state in a `.mctest` (milestone record)

**Status:** done 2026-10-19 (steps 1-2) (added 2026-10-19; owner: the M-SCRIPT thread; device-free; no Linear issues: free limit, this record is the tracker).

**Why.** `.mctest` (guide 14.3.1) can check a field, `alive`, an `event`, `scene`, `log_contains`, `log_errors` / `log_warnings` and `feel`. A script-driven game still has
two things it cannot pin: how many entities of a kind exist at the end ("the spawner made 3 enemies, and the pickup removed one"), and that a script stays cheap
(`mce profile` shows the numbers, but nothing fails when a change doubles them). Both are one check each and live in the MCP module only.

**Not in scope:** a Luau test framework (`describe`/`expect` inside scripts: scripts stay stateless and side-effect-checked through components), wall-clock checks.

## Steps

| # | Step | Model | State |
|---|---|---|---|
| 1 | `{"count": "Component", <op>}`: the number of live entities with that component at the end of the run (or after `tick`), spawned ones included | Sonnet | done in PR #615 |
| 2 | `{"script": "scripts/a.luau", "peak_steps": <op>}` (also `avg_steps`, `updates`): checks the run's `ScriptInfo` profile (M-SCRIPTPROF) so a budget regression fails a test | Sonnet | done in PR (this step) |

Each step ends with a "Step N as built" section here, guide 14.3.1 updated, a test, and a `changelog.d/SCRIPTTEST-<n>-<slug>.md` fragment.

## Step 1 as built (count check)

- `Condition::What::Count` in `agent_sim.cpp`: `{"count": "<Component>", <op>: n}` counts the entities in the world's tables that hold the component (`world.find_component(type_hash)`), so entities the run
  spawned are in; evaluated during the run like the other world checks (`tick` works, and `until` accepts it). A missing comparison or an unknown/non-component name is a spec error.
  The check reads `count <Component>` in the report. No header outside the MCP module changed.
- Test: "playtest: a count check reads how many live entities have a component" (`tests/mcp/test_mcp_playtest.cpp`: a satisfied and a failed check, a zero count, both spec errors).
- Not done: counting by entity name or tag, and a count restricted to one scene (a scene change leaves the old entities counted until they are gone).

## Step 2 as built (script step check)

- `Condition::What::Script`: `{"script": "<path or tail>", "metric": peak_steps|avg_steps|total_steps|updates, <op>: n}` reads `HeadlessRun::scripts` after the run (`eval_after` takes the profile span; MCP-internal
  signature, no wide header). The script matches by full name or by the end after a `/`; an unknown name has no value and fails with the scripts that ran listed. A `tick` on it is refused like any non-world check.
- Test: "test_run: a script check pins a scene script's step profile" in `tests/mcp/test_mcp_script.cpp` (passing, failing, unknown script, both spec errors). A script's first update runs on the
  second tick, so 5 ticks give 4 updates: the test pins `>= 3`, not the exact number.
- Not done: a share-of-budget metric (`peak_steps / budget_steps`), failing on any script over a percentage without naming it.
