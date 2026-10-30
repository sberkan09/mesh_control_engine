# M-MISSIONKILL — kill and interact jobs

Status: step 1 done (2026-10-30). Design: [M-OPENWORLD-GAPS](M-OPENWORLD-GAPS.md) section G.

`Objective` gets `team` (u8, kill: 0 counts any death, else only a dead `Health3D` of that team) and `by_actor` (u8, kill: only the mission actor's kills); they replace the old u16 pad, so the size stays 120. A Kill step whose resolved target is already dead or gone is done. `samples/09_openworld` gets three jobs: Hit (kill a named man at (165, 9), then reach a drop), Cop killer (kill three police yourself, then reach a drop; raise the wanted level first) and Pickup (press E at a package, then reach a drop). Tests: `tests/kit/test_missions.cpp` (two cases), `test_mcp_openworld` (six missions, two Kill and one Interact objective).

## Limits

Team 2 includes police cars. Kill progress is not on the mission HUD. The end to end run of Hit and Pickup in the MCP test is not done (it needs aimed fire and an Interact press); the kit logic is covered by the unit tests. Interact shares E with getting into a car.
