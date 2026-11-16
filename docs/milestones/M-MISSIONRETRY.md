# M-MISSIONRETRY — retry a failed mission

Status: step 1 done (2026-10-29). Follow-up to [M-MISSIONSTART](M-MISSIONSTART.md): a failed mission could not re-arm its marker.

## Design

`MissionRetry { delay (3 s), left }` (size 8) on a Mission entity. When the mission is Failed, `kit_missions` counts `delay`
seconds down in `left`, then sets the mission Idle, clears `start`, and walks the objective chain from `Mission.objective`
(capped at 64) setting each to Pending. The marker (or `start`) can then start it again. Registered by `install_missions`.

## Limits

No event is pushed on the reset; a marker the actor still stands on restarts the mission at once; no cost or cool-down besides
`delay`; Complete missions are not reset.
