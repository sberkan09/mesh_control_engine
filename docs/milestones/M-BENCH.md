# M-BENCH — Headless benchmarks of the hot paths (milestone record)

**Status:** step 3 in review (steps 1-2 done; more cases and optimizations can follow as new steps). **Owner: the "Geliştirme rutini 3" thread.** Defined 2026-10-28 on the coordinator's suggestion (device-free performance work; no gameplay, outside M-WASTED and M-VISUAL). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**Why.** `docs/architecture/ecs-gameplay.md` states NFR-T-01 (100k entities with transform and velocity iterate in under 1.5 ms) as "a Tracy-verified CI benchmark with a synthetic 100k-entity world", but the repository has no benchmark at all: M-PERF measures frames of a running game (counters, captures), nothing measures the ECS, the job system or the asset loader on their own. Without a number there is nothing to optimize against, and a regression in a hot path only shows up as a slow game later.

**Design.**
- `mce_bench` (`tools/bench`): a headless executable, `--quick` / `--json` / `--entities N` / `--repeats N`. Each case runs on a fresh world, `repeats` times, and reports the best time, in ms and ns per item. The JSON has `format: "mce-bench"`, `version: 1`, one row per case and a checksum so the work cannot be optimized away.
- A number is the machine's, so the tool gates nothing and the smoke test asserts no timing. Read numbers from a Release build; a Debug build only proves the tool runs. The scheduled integration run is the place to record them.
- Counter-style gates (allocations, bytes) stay in M-PERF; this milestone adds time cases only.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `mce_bench` with the ECS cases (iterate, spawn, destroy, add/remove) and the job-dispatch case, ctest smoke `bench_smoke` and the usage check | done (#832) |
| 2 | Asset cases on the `.mce` container: write (4 KiB chunks, MB/s), open with both CRC checks (MB/s), `find` by GUID for every chunk. Loading a cooked scene into a world stays out: it needs the scene loader, a core-adjacent dependency | done (#833) |
| 3 | The optimization the numbers pointed at (below) | open (PR) |

**Gates:** `bench_smoke`, `bench_usage`.

## Release numbers (clang Release, this container, 2026-10-28; best of 7)

| Case | Items | Result |
|---|---|---|
| `ecs_iterate` | 100,000 | 0.16 ms (1.6 ns per entity); NFR-T-01 asks for under 1.5 ms, so there is room |
| `ecs_spawn` | 100,000 | 17 ms (170 ns per entity) |
| `ecs_destroy` | 100,000 | 2.3 ms (23 ns per entity) |
| `ecs_add_remove` | 100,000 | 24 ms (244 ns per entity, both moves) |
| `jobs_run_wait` | 100,000 | 1.5 ms (15 ns per job, inline execution until the job runtime lands) |
| `container_write` | 1,000 x 4 KiB | **243 ms, 16 MB/s** before step 3; **29 ms, 133 MB/s** after |
| `container_open` | 1,000 x 4 KiB | 10 ms, 365 MB/s |
| `container_find` | 1,000 lookups | 0.5 ms (0.5 us each; linear in the chunk count) |

**Step 3.** `container_write` was 24 times slower than `container_open` on the same bytes and grew quadratically: `Array::resize` grows to exactly the size asked, and `ContainerWriter::add` resized once per chunk, so every chunk copied the whole buffer. `add` now reserves with the array's own 1.5x step first (a change in `container.cpp` only; the output is byte-identical, the container tests and the whole `test_cook` binary pass, and a new test writes 3,000 chunks of uneven sizes). At 10,000 chunks the write takes 296 ms; the quadratic version would take about 24 s.

**Findings left for later steps.**
- `Array::resize(n)` and `resize(n, fill)` call `reserve(n)`, which reallocates to exactly `n`. Any loop that grows an Array by resizing it one step at a time has the same quadratic copy. `emplace_back` has the 1.5x policy, `resize` does not. Changing it is a core container header change (wide), so it is a seam PR with the coordinator's review, after a grep of the call sites.
- `ContainerView::find` is a linear scan over the TOC: 4.6 us per lookup at 10,000 chunks, 46 ms to find every chunk once. The cooked project asks for chunks by GUID at load; a sorted index would make it logarithmic. Not urgent at the sizes a game ships today; the benchmark is there to say when it becomes urgent.
