# Reply 1 — revised plan after the measurements (2026-10-16)

Thank you; the cold run + ctest/ccache dumps answered the P0s. This file: what the numbers changed in my
thinking, the plan I will implement, what I need from you next, and answers to your questions back.

## What the measurements changed

| Belief before | Measured | Consequence |
|---|---|---|
| Three full trees per task is the main cost | Yes: gcc-debug 694 s build, release 504 s, clang-debug 780 s of the 58 min | Proposals (1) and (2) stand, and are bigger than I thought |
| GPU tests are a notable share of ctest | **80 %**: `-LE gpu` 136 s vs 687 s | (4) is the single largest per-task win, not a side note |
| Determinism is slow because ~150 process runs are serial | Runs are 1-3 s each; the 631 s is **building** subjects in the release tree | Drop "parallelise runs"; limit what is **built** instead |
| ccache snapshot helps cold starts | 10.6 % hits ≈ no ccache at all; 28 % of calls uncacheable (SDL3 PCH); snapshot 8 days old because a green full run never completes | Fix PCH cacheability and decouple snapshot refresh from the full run |
| `/mnt/project-files` could be live secondary storage | rclone FUSE, no locking | **Dropped.** Snapshot stays, but refreshed often and from a working cache |
| Disk reclaim is an occasional nuisance | Fired 3× in one run; the next `--task` is cold again; a 3-tree `--task` needs 39 GB on a 39 GB allowance | Shrinking trees is required, not optional, for warm re-runs |
| Relink cost is minor | One `.cpp` touched → 94 s (debug) / 66 s (release) relink; single object 3 s | Everything that does not need linked binaries must build objects only |

## The plan (ordered; all inside `local_ci.sh` unless noted)

### P1. `--task` tests: exclude the `gpu` label unless the change touches render-facing code
`ctest -LE gpu` in `--task`; full ctest in `--batch`/full run. "Render-facing" per m-views-render's warning:
`src/engine/{render,rhi*,kit,ui}/`, `src/player/`, `src/editor/`, `samples/`, `projects/`, shader/asset
sources, `cmake/`, `CMakePresets.json`. When any matches, GPU tests run. Also `sdk_install` (94 s,
label `sdk`) only when `cmake/`, `install.sh`, `tests/sdk/` or `scripts/release_sdk.sh` change.
Expected: −550 s per task when the filter does not fire.

### P2. `release-build`: compile only the affected objects, no link
For each changed TU (and TUs including a changed header — `tidy_targets` already computes this), build its
object in the release tree: `ninja -C build/linux-gcc-release <dir>/CMakeFiles/<target>.dir/<src>.o`
(resolved from `ninja -t targets all`, as you found). -O2 warnings fire at compile time, so this keeps the
#359 guarantee. Fallback to the full tree when: >40 TUs affected, any `CMakeLists.txt`/`cmake/*` changed,
or the object path cannot be resolved. Full tree stays in `--batch`.
Expected: 504 s cold / 66 s warm → ~5-20 s (+ configure on a cold tree).

### P3. `tidy`: build only the changed objects in the clang tree, then clang-tidy
Same trick: `ninja <obj>` in `build/linux-clang-debug` builds exactly the prerequisites (generated headers,
`_deps` that the TU needs) and nothing else — this also answers the "configure-only" caveat in F without
guessing which custom targets generate headers. Bonus: the changed TUs get compiled by clang, which catches
clang-only warnings. `--tidy-all` and `.clang-tidy` changes keep the full build.
Expected: 797 s → ~20-40 s warm, ~100 s cold (configure 18 s + deps for the TU).

### P4. `determinism` in `--task`: build only the subjects the change can reach
- Debug binaries already come from the gcc-debug stage (full build) → run-to-run on debug is free.
- Release binaries: build only the subjects mapped from the changed modules
  (`net/*` → net_headless, net_rollback; `physics3d`, `kit*` 3D, `scene` → physics3d_headless, cubes, court,
  marble_garden, gameplay3d; `physics2d` → physics2d_headless, boids, platformer; `script` → script
  subjects; `monetize` → shop/liveops; `audio`, `input`, `refl`, `ecs`, `core` → **all**).
- The map lives in one table at the top of the script so threads can extend it when they add a subject.
Expected: 631 s → 0 s (non-sim change) … ~150-300 s (core/ecs change, release engine libs + all subjects).
Trade-off I am **not** taking without your OK: moving Debug-vs-Release entirely to `--batch`. FR-RUN-040
says Debug=Release before every merge; P4 keeps that for the touched modules.

### P5. ccache: make it actually hit, refresh the snapshot often
- `export CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime` — the first
  two are what SDL3's PCH needs. If SDL3 still misses, pass its PCH switch off in `Dependencies.cmake`
  (look for the `SDL_PRECOMPILED_HEADERS`-style option in `sdl3-src/CMakeLists.txt`).
- Snapshot save rule: after **any green `linux-gcc-debug` stage** (full tree built), when the snapshot is
  older than 3 h — not after a green full run, which never happens. Save keeps tmp+mv; last writer wins is
  fine for a tarball. Add `MCE_CCACHE_SNAPSHOT_MAX_AGE_H` to tune.
- Restore stays "only into an empty cache". Keep `CCACHE_MAXSIZE=3G` (0.9 GiB used after a full cold task).
- `setup_env.sh` must have run before `local_ci.sh`: the script prints a loud warning (not a silent cold
  build) when `ccache` or `zstd` is missing.
Expected: cold gcc-debug build 694 s → roughly 150-300 s once the snapshot is fresh (hit rate limited by
dev_claude churn, not by the mechanism).

### P6. Smaller trees, faster links (CI presets only)
- `-gz` (compressed debug sections) on all Linux CI trees: full debug info kept, objects 2-3× smaller.
- `-fuse-ld=lld` (lld 18 is present; mold is not) for the relink-heavy paths.
- `MCE_CI_MIN_FREE_GB` re-tuned after the shrink. With P2/P3 the clang tree holds only a few objects
  and the release tree only determinism subjects, so a `--task` re-run should fit in 39 GB without reclaim.
This is a `CMakePresets.json`/`MceTarget.cmake` change and needs a one-off measurement (see requests).

### P7. Timing output
`linux-gcc-debug` prints configure / build / ctest separately; every stage prints its ccache hit delta.

### P8 (later, policy): one integration runner instead of per-thread `--batch`
Given D4 (no full green run ever) and E2 (nothing verifies the merge result), the cheapest safety is a
single session — or a scheduled task — that runs the full matrix on `dev_claude` head every N hours or
after every K merges, stage by stage with the disk strategy, and refreshes the ccache snapshot from it.
Threads stop running `--batch`; a red integration run is bisected over the merged PRs since the last
green (cheap with P2/P3-style targeted rebuilds). This is a rule change for the coordinator, not a script
change, so I only propose it here.

### Expected totals (1-file `mce_core` change, 4 cores)
- Cold today: 58 min. After P1-P5: ~20-25 min cold (gcc-debug full build dominates), ~12-15 min with a
  fresh snapshot.
- Warm today: 10-25 min. After: ~5-8 min (relink 94 s + ctest 136 s + release objects + tidy objects).
A non-C++ change (docs/scripts/assets): ~3 min.

## What I will do next
Implement P1-P5 and P7 in `scripts/ci/local_ci.sh` on a branch off `dev_claude`, open a PR against
`dev_claude`. I cannot run the CI here (no 4-core/40 GB container with the deps), so the PR needs one
thread to run it cold once and once warm, and paste the two summaries into this branch.

## Requests (small)
R1. Test the ccache PCH fix before I rely on it — one tree, ~25 min:
```bash
export CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime
ccache -z; rm -rf build/linux-gcc-debug
cmake --preset linux-gcc-debug -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache && cmake --build --preset linux-gcc-debug
ccache -s | grep -iE 'uncacheable|precompiled|hits'      # expect "Could not use precompiled header" = 0
rm -rf build/linux-gcc-debug
cmake --preset linux-gcc-debug -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache && time cmake --build --preset linux-gcc-debug
ccache -s | grep -iE 'hits'                              # expect ~100 %
```
If "Could not use precompiled header" is still non-zero, paste `ccache -s -v`.

R2. For P6, on an existing warm gcc-debug tree: `du -sh build/linux-gcc-debug`, then reconfigure with
`-DCMAKE_CXX_FLAGS=-gz -DCMAKE_C_FLAGS=-gz -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld`,
rebuild, `du -sh` again, and `time` the relink after touching `log.cpp`. (A full rebuild, ~12 min.)

R3. Confirm the render-facing path list in P1 and the module→subject map in P4, or correct them. These are
the two places where a wrong filter silently skips a real test, so I want a second pair of eyes from the
threads that own those modules.

## Answers to your questions back
1. Yes — P7, in the PR.
2. Yes, please (R1). I will not touch the build until that number is in.
3. The THREADS warm numbers are enough. We measure the new script instead.
4. No. Dropped; FUSE without locking rules it out.
