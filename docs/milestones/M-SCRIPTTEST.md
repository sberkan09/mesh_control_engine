# M-SCRIPTTEST — Checks a script-driven game could not state in a `.mctest` (milestone record)

**Status:** planned (added 2026-10-19; owner: the M-SCRIPT thread; device-free; no Linear issues: free limit, this record is the tracker).

**Why.** `.mctest` (guide 14.3.1) can check a field, `alive`, an `event`, `scene`, `log_contains`, `log_errors` / `log_warnings` and `feel`. A script-driven game still has
two things it cannot pin: how many entities of a kind exist at the end ("the spawner made 3 enemies, and the pickup removed one"), and that a script stays cheap
(`mce profile` shows the numbers, but nothing fails when a change doubles them). Both are one check each and live in the MCP module only.

**Not in scope:** a Luau test framework (`describe`/`expect` inside scripts: scripts stay stateless and side-effect-checked through components), wall-clock checks.

## Steps

| # | Step | Model | State |
|---|---|---|---|
| 1 | `{"count": "Component", <op>}`: the number of live entities with that component at the end of the run (or after `tick`), spawned ones included | Sonnet | not started |
| 2 | `{"script": "scripts/a.luau", "peak_steps": <op>}` (also `avg_steps`, `updates`): checks the run's `ScriptInfo` profile (M-SCRIPTPROF) so a budget regression fails a test | Sonnet | not started |

Each step ends with a "Step N as built" section here, guide 14.3.1 updated, a test, and a `changelog.d/SCRIPTTEST-<n>-<slug>.md` fragment.
