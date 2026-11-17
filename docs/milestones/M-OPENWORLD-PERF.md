# M-OPENWORLD-PERF — Cost of the camera chain on the city (measurement record)

**Status:** done (2026-11-17), measurement only, no engine change. **Owner: routine 2 thread (the M-VISUAL line).**
No Linear issues (free limit); this record is the tracker.

**Why.** The camera chain (#1286 camera-only layer, #1287 `BoxCollider3D` mask, #1291 orbit re-sweep, #1288 camera-only canopy
colliders on every tree) took the `09_openworld` city from 2098 to 2901 entities after 900 ticks of `drive.input` (+38 %). Nobody
had measured what that costs.

**Method.** `mce_player -headless 1 -ticks 900 -scene scenes/city.mcscene -input drive.input -perf_capture x.mcperf`, RelWithDebInfo
(`linux-gcc-release`), the same 4-core container, 3 runs per side, cooked from `330c4512` (before: wheels, no camera layer) and
`33eea232` (after: the whole chain). The state hash is identical across the runs of one side (before `9ddbf335…`, after
`748b7be7…`), so the work is the same every run and only timing varies. Headless has no GPU, so this is the sim side; the added
colliders are camera-only and add no draws.

**Result (sim ms per tick, p50 / p95 over three runs; budget 16.6 ms at 60 Hz).**

| | p50 | p95 |
|---|---|---|
| before | 1.25 to 1.32 | 1.53 to 1.88 |
| after | 1.35 to 1.43 | 1.87 to 1.96 |

Sim p50 grew about 10 % (0.12 ms) for 38 % more entities, and the whole tick stays near one tenth of the budget. Totals of the
systems that moved, per 900 ticks (median of three):

| system | before ms | after ms |
|---|---|---|
| `kit3d_physics` | 437 | 497 |
| `kit3d_bodies` | 38 | 73 |
| `kit_crowd` | 53 | 74 |
| `transform_propagate_early` | 171 | 188 |
| `kit3d_nav` | 333 | 329 |

`kit3d_physics` gained 14 % and `kit3d_bodies` (it creates the bodies of the new colliders) doubled from a tiny base: 0.04 ms per
tick. `kit_crowd` moved because the walkers' route differs after the hash changed, not because of the trees.

**Verdict.** No regression worth a fix: the chain's physics cost is 0.07 ms per tick, the tick fits the budget ten times over, and
the largest system (`kit3d_nav`) did not move. No plan step is attached.

**Not measured.** Windowed frame time with a GPU (the container has none; the colliders are not drawn, so none is expected) and a
phone. `perf_run` on a Mac or a RunPod GPU would add the render column if someone wants it.
