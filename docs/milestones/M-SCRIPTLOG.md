# M-SCRIPTLOG — Scripts can log at more than one level (milestone record)

**Status:** done 2026-10-19 (one step; added 2026-10-19; owner: the M-SCRIPT thread; device-free; no Linear issues: free limit, this record is the tracker).

**Why.** `mce.log` only wrote info lines, so a script could not report a problem the way C++ systems do, and a `.mctest` with `{"log_errors": 0}` / `{"log_warnings": 0}` (guide 14.3.1) could never
see a script's complaint. Touches `l_log` in `script.cpp` (logging only, no VM change) and the `mce` declaration.

## Steps

| # | Step | Model | State |
|---|---|---|---|
| 1 | `mce.log(msg, level?)` with `"info"` (default), `"warn"`, `"error"`; any other level is a script error | Sonnet | done in PR (this step) |

## Step 1 as built

- `l_log` picks `MCE_LOG_INFO/WARN/ERROR` by the second argument, same `[<name>] script: <msg>` text. Unknown level: `luaL_error` naming the allowed ones. `definitions.cpp` declares `level: string?`.
- Tests: "script: mce.log takes a level; an unknown level is a script error" (console ring levels, error text). Guide 27 `mce.log` row updated.
- Not done: a per-script mute and rate limiting (a script logging every tick floods the ring, as before).
