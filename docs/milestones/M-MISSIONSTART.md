# M-MISSIONSTART — mission givers and wanted-level failure

Status: step 1 done (2026-10-29). Small gameplay milestone for the GTA-like slice (the "3-4 missions" item of the feasibility
assessment): a mission starts where the player walks into a marker, and heat can ruin it.

## Design

`MissionMarker { mission, radius (3 m), enabled }` (size 16) on a placed entity. `kit_missions` checks the markers before
the missions: if the marked `Mission` is Idle and its `actor` is within `radius`, `start` is set, so it starts the same tick.
`MissionWantedFail { level (3) }` (size 4) on a Mission entity: while the mission is Active and its actor has a `Wanted3D`
at `level` or above, the mission fails with `MissionFailed` reason 3 (reasons 0 to 2 were time limit, fail_on_death and a
missing objective). Both components are registered by `install_missions`; the wanted level is read with `try_get`, so a
scene without `install_wanted3d` simply never fails that way.

## Limits

One mission per marker; no marker visuals (use a `MapMarker` beside it); no "mission offered" prompt or key press; a failed
mission does not re-arm the marker (set `state` back to Idle yourself).
