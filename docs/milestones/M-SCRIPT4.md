# M-SCRIPT4 — Closing the open ends of M-SCRIPT3 (milestone record)

**Status:** active (added 2026-10-18; device-free; no Linear issues: the workspace is at its free limit, this record is the tracker).
Plan: this record (the coordinator adds the ROADMAP status row).

**Why.** M-SCRIPT3 ended with a list of things it wished for or did not test (see "Wished for and open" and each "Step N as built" in
[M-SCRIPT3.md](M-SCRIPT3.md)). Each is small and in the scripting layer, so they are taken in order as separate PRs.

**Not in scope:** new VM features, compiling mod source at load (a separate decision), anything that needs a device.

## Steps

| # | Step | Model | State |
|---|---|---|---|
| 1 | End-to-end test that a `Perceiver` hears a script's `mce.make_noise` (M-SCRIPT3 step 5 open work) | Sonnet | not started |
| 2 | Slot-handle test for a Vec4 / Color array field, and the ScriptVars demo scripts moved onto named slots (M-SCRIPT3 step 10 open work) | Sonnet (Opus review if `script.cpp` changes) | not started |
| 3 | Scene-editable per-script constants (a reflected component the script reads by name, instead of literals in the source) | Sonnet | not started |
| 4 | `mce.overlap_circle2d` / a 2D overlap query without physics, next to `mce.near` | Sonnet | not started |
| 5 | Script projects' ctest target calls `mce test` instead of `demo_gate.py` (M-SCRIPT3 step 9 open work) | Sonnet | not started |

Each step ends with a "Step N as built" section here, a guide/`docs/engine/script.md` update and a `changelog.d/SCRIPT4-<n>-<slug>.md` fragment.
