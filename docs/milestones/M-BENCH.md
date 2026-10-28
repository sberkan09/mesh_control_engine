# M-BENCH — Headless benchmarks of the hot paths (milestone record)

**Status:** open (step 1 of 3). **Owner: the "Geliştirme rutini 3" thread.** Defined 2026-10-28 on the coordinator's suggestion (device-free performance work; no gameplay, outside M-WASTED and M-VISUAL). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Why.** `docs/architecture/ecs-gameplay.md` states NFR-T-01 (100k entities with transform and velocity iterate in under 1.5 ms) as "a Tracy-verified CI benchmark with a synthetic 100k-entity world", but the repository has no benchmark at all: M-PERF measures frames of a running game (counters, captures), nothing measures the ECS, the job system or the asset loader on their own. Without a number there is nothing to optimize against, and a regression in a hot path only shows up as a slow game later.

**Design.**
- `mce_bench` (`tools/bench`): a headless executable, `--quick` / `--json` / `--entities N` / `--repeats N`. Each case runs on a fresh world, `repeats` times, and reports the best time, in ms and ns per item. The JSON has `format: "mce-bench"`, `version: 1`, one row per case and a checksum so the work cannot be optimized away.
- A number is the machine's, so the tool gates nothing and the smoke test asserts no timing. Read numbers from a Release build; a Debug build only proves the tool runs. The scheduled integration run is the place to record them.
- Counter-style gates (allocations, bytes) stay in M-PERF; this milestone adds time cases only.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `mce_bench` with the ECS cases (iterate, spawn, destroy, add/remove) and the job-dispatch case, ctest smoke `bench_smoke` and the usage check | open (PR) |
| 2 | Asset cases: open a container and read a chunk, load a cooked scene into a world | planned |
| 3 | A concrete optimization from the numbers (the add/remove case moves a row between archetypes per call; its cost per entity is the first thing to read in a Release run), with the before and after in this record | planned |

**Gates:** `bench_smoke`, `bench_usage`.
