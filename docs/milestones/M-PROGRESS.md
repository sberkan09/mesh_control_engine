# M-PROGRESS: progression and goals in 09_openworld

**Status:** planned (2026-11-02). Closes "no progression or goal" in [M-OPENWORLD-GAPS](M-OPENWORLD-GAPS.md) (open-world gaps list).
Follows [M-WEAPONSWITCH](M-WEAPONSWITCH.md) and [M-MELEE](M-MELEE.md).

**Scope.** The game layer only: `samples/09_openworld` plus one new kit module (`progress`). No ECS, render or physics core, no
HLOD or floating origin. Missions (`missions.h`) stay as they are; goals count what the player does anywhere in the city.

## Design

`Progress3D` on the player holds counters the engine fills in: `kills` (a `Died3D` whose `killer` is the player and whose
victim is not the player), `missions` (a `MissionCompleted` for a mission whose `actor` is the player), `rides` (the player
sits in a car, counted once per seat entry) and `melee_hits` (the hits of its `MeleeSwung3D`). Counters are `u32` and
padding-free (`state_hash` hashes raw bytes). Cash earned is left out: the wallet has no income event to count.

`Goal3D` on its own entity: `stat` (a `MCE_ENUM` of the counters), `target`, `reward` (cash), `done`, and a `name` for the HUD.
System `kit_progress` (FixedUpdate_Pre, after `kit_missions`) updates the counters from this tick's
events, then marks every goal whose counter reached `target` as done, pays `reward` into the player's `Wallet3D`, shows a
`ShowCaption` ("Goal: ...") and pushes `GoalDone3D { goal, reward }`.

Goals are read in entity order, so a replay gives the same state. Everything is in components with `Save`, so a save keeps
progress.

## Steps (seam, feature, polish; each its own PR)

| Step | What | Status |
|---|---|---|
| 1 seam | `Progress3D`, `Goal3D`, `GoalDone3D`, system `kit_progress`, unit tests (each counter, a goal pays once, reward lands in the wallet, a dead or missing player, deterministic order). No sample change. | done |
| 2 feature | Sample: `Progress3D` on the player, five goals (first kill, 10 kills, a mission, a ride in a car, 5 melee hits) with cash rewards; a HUD list that shows each goal and ticks it off; a caption when one is done. | open |
| 3 polish | Save and load keeps progress (checked by the save test), MCP inspect check, user-guide section, `M-OPENWORLD-GAPS` strike-through. | open |

## Notes

- Step 1 adds two components, so the transcript golden component count and `capabilities.md` move with it.
- Linear: the workspace is at its free-issue limit, so there is no card; this file is the record.
