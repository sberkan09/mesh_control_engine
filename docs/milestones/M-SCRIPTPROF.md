# M-SCRIPTPROF — Seeing what scripts do: profile, budget errors, log source, traces (milestone record)

**Status:** active (added 2026-10-18; owner: the M-SCRIPT thread; device-free; no Linear issues: free limit, this record is the tracker).

**Why.** A script that is slow, runs out of its step budget or prints into a crowded log is hard to diagnose today: `ScriptInfo.last_steps` is the only number, the budget
error says "step budget exceeded (10000 per tick)" with no line, `mce.log` lines do not say which script wrote them, and `mce inspect` shows values after a run but
not how they moved. A game author (or an agent) writing Luau needs the same tools a C++ author has: a profile, a located error, a traceable log and a time series.

**Not in scope:** an interactive debugger (breakpoints, stepping: it needs VM hooks and a device UI), a profiler for the engine's own systems (M-PERF covers it).

## Steps

| # | Step | Model | State |
|---|---|---|---|
| 1 | Per-script step profile: `ScriptInfo` keeps peak, total and ticks run; `run_headless` returns it as `scripts`; `mce profile <project> [--ticks N]` prints a table with the budget share | Sonnet (Opus review: touches the update loop in `script.cpp`) | not started |
| 2 | A budget or memory failure names the script line it stopped at (`file:line`), like any other script error | Sonnet (Opus review: VM interrupt) | not started |
| 3 | `mce.log` lines carry the script name (`script <name>: ...`) so a console filter and `run_headless`'s log tail show the source | Sonnet | not started |
| 4 | `mce inspect --trace`: sample entity fields every N ticks through `run_headless`'s `trace`, printed as series | Sonnet | not started |

Each step ends with a "Step N as built" section here, guide 27 / `docs/engine/script.md` updated, and a `changelog.d/SCRIPTPROF-<n>-<slug>.md` fragment.
