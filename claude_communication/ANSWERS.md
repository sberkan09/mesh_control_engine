# Answers

Fill in below. Keep the `## <section>` and `### <question id>` headings; add a `## <date> — <who>` line
at the top of each batch so it is clear when and by whom it was written. Attach long output as files
under `logs/` and link them.

---

## 2026-10-16 — project coordinator thread

Who/what: the project's thread session (a Claude session in the same project as the agents that run CI).
Two kinds of data below:
- **MEASURED** = I ran it myself in this container on `origin/dev_claude` @ `90cc11e2` (merge of #372, so it
  includes #363 release-build and #371 reorder/reclaim). The only local change was one appended comment line in
  `src/engine/core/src/log.cpp`, so that `--task` selects determinism + release-build + tidy (1 file, 1 insertion).
- **COORD** = facts given to me by the project coordinator session; **THREADS** = accumulated experience from
  8 long-running threads (about 1.5 weeks of daily CI), written from memory/their logs, no new measurements;
  raw notes in `logs/2026-10-16-thread-notes-*.txt` (m-access, m-gameplay3d, m-script, m-views-render, rutin-1,
  rutin-2, rutin-3, siradaki-milestone). Where the two disagree I say so.

### Headline findings (measured, read these first)
1. **Cold `--task` = 58 min wall** (4 cores): configure 155 s, walls 12 s, gcc-debug 1381 s, determinism 631 s,
   release-build 504 s, tidy 797 s. Warm tree/ccache numbers reported by threads are 10-25 min.
2. **The GPU tests are ~80 % of the ctest time.** `gpu` label = 2121.6 of 2577.8 sec*proc (68 of 208 tests);
   `ctest -LE gpu` (140 tests) takes **136 s** vs **687 s** for the full run. Slowest: cinema_demo_player_gpu 271 s,
   test_render_sdlgpu 220 s, sample_06_showcase_gpu 210 s, sample_04_diorama_gpu 162 s, solstice_reach_gpu 129 s.
   (Contradicts THREADS m-gameplay3d, who ran `ctest -LE gpu` by hand and saw ~60 s tests / ~130 tests.)
   Note: your proposal (4) is stronger than you thought — and the thread m-views-render warns a path filter on
   `render/` alone would have missed bugs (kit/player/editor code); filter on `src/engine/{render,rhi*,kit,ui}`, `src/player`, `src/editor`.
3. **The determinism stage is ~100 % build, not run.** A release-only 3-run of the longest subjects takes
   1.0-2.5 s each (court 3000 ticks 1.5 s; cubes 2.2 s; cubes-walk 2.5 s; net_rollback 0.96 s; boids 1.6 s;
   all hashes identical to the stage's). The stage's 631 s is building sample targets in both gcc trees.
   So running subjects in parallel (proposal 3) buys seconds; selecting what to *build* is what matters.
4. **tidy builds all of `linux-clang-debug` (2746 ninja steps, ~780 s) to tidy a handful of files.**
   `run-clang-tidy-18` on one file with an already built tree: 7 s. See F for the "configure only" caveat.
5. **ccache snapshot is nearly useless at present.** First build after restore: 10.6 % hits (357/3357),
   and 28 % of all calls (1308/4665) are **uncacheable: "Could not use precompiled header" 100 %** —
   caused by third-party SDL3 (`target_precompile_headers(SDL3-shared/-static)` in `build/_fetch/*/sdl3-src/CMakeLists.txt`),
   so every SDL3 TU is recompiled in every tree every time. Possible fix (not tested): `CCACHE_SLOPPINESS=pch_defines,time_macros`
   or disabling SDL3's PCH. Also: the snapshot on disk is from **2026-10-08** (8 days old) and the objects in it do not match
   current dev_claude (dev_claude moves every 10-30 min, 6+ threads).
6. **Relinking dominates tiny edits.** Touching one `.cpp` in `mce_core` then `cmake --build`: gcc-debug 94 s,
   gcc-release 66 s (everything that links mce_core relinks); building just that object: **3 s**.
7. **Disk reclaim fired 3 times in one cold `--task`** (lines under C3). Each removed a tree the next
   stage needed to rebuild or that is simply re-created later — cheap here because each was deleted after its last use,
   but see C3.
8. `--task` as run here executed **4 big stages in series and builds 3 trees** (gcc-debug 15 G, gcc-release 16 G,
   clang-debug 7.9 G) — 3 trees x 1700-2750 ninja steps each of mostly the same sources.

### A1
Cannot supply 5-10 `--task` summary tables from the others' runs: those logs live in the other containers/threads and only
fragments survive in their notes. What I have:

**MEASURED — one cold `--task`** (fresh container, ccache restored from snapshot, no `build/`; diff = 1 file, 1 line in
`src/engine/core/src/log.cpp`; stages auto-added: determinism, release-build; `logs/2026-10-16-coord-container-cold-task-run.txt`):

```
=== Summary (90cc11e2)
  walls                  PASS (12s)
  linux-gcc-debug        PASS (1381s)
  determinism            PASS (631s)
  release-build          PASS (504s)
  tidy                   PASS (797s)
  ccache: Cacheable calls:   3357 / 4665 (71.96%)
  ccache:   Hits:             357 / 3357 (10.63%)
  ccache:   Misses:          3000 / 3357 (89.37%)
real 58m11s   (+ restore 12 s, configure x3 155 s)
```

**THREADS — summary-line figures they remember** (warm unless said; their `--task` mostly did not yet include release-build):
- m-script: walls 7-10; gcc-debug 500-630 (hits 66-87 %), 566 s with 17 % hits; tidy 130-280, 420-460 when clang-debug cold;
  determinism 130-410. `--task` ~15-25 min.
- m-gameplay3d: walls 7, gcc-debug 416, tidy 282-296, determinism 143-159: ~14-15 min.
- m-views-render: walls 9, gcc-debug 489, tidy 351, determinism 174 (hits 42 %); `--quick` after merging dev_claude 633 s (38 %);
  first baseline of session, ccache restored (14 % hits): gcc-debug 1013 s.
- m-access: gcc-debug 470-650, tidy 170-370 (0-15 when no C++), determinism ~480; `--task` ~10-15 min.
- rutin-1: gcc-debug 631 and 1060; `--task` 25-35 min in practice (with determinism).
- rutin-2 (with release-build): gcc-debug 592-835, tidy 103 (85 % hits) .. 512-614, release-build 98 s warm / died at 442 s on disk cold; `--task` 20-25 min.
- rutin-3: gcc-debug 1515 s on a mostly cold tree; tidy 362-750 s.
- siradaki-milestone: gcc-debug ~530, tidy ~0 (no C++), configure 6-26 s; total ~9-10 min.
- `--batch` in one go: nobody finished one cleanly (rutin-1: ~1 h if the disk holds; m-script: >45 min; others never); typically run stage by stage.

Pattern: cold ~58 min (measured) vs warm 10-25 min (threads); the spread is almost entirely "which trees exist + ccache hit rate".

### A2
Warm vs cold. All below MEASURED, 4 cores, `-j4`. Full logs: `logs/2026-10-16-coord-container-cold-task-*.txt`, sub-steps `…-manual-substeps.txt`.

**Cold run (inside `--task`)**: configure of the three trees in parallel 155 s (gcc-debug alone: 108 s by hand);
`linux-gcc-debug` stage 1381 s = build ≈ 694 s + `ctest -j4` 687 s ("Total Test time (real) = 686.76 sec", 208/208 passed).
(Build = stage minus ctest; the script does not time them separately — see "Questions back".)

**Manual sub-steps of linux-gcc-debug** (done after the cold run, tree deleted so tree-cold; **caution: my manual
`cmake --preset` did not pass `-DCMAKE_CXX_COMPILER_LAUNCHER=ccache` that local_ci adds, so these builds ran WITHOUT ccache — i.e. the no-cache baseline**):
- configure: 108 s
- build, no ccache, tree cold: **715 s** (≈ same as the 694 s the cold run needed with a 10.6 %-hit ccache → the snapshot saved ~nothing)
- build again, nothing changed: **1 s** (warm tree no-op is fine)
- build after touching one `.cpp` in `mce_core`: **94 s** (relinks all dependents — see headline 6)
- ctest `-LE gpu` -j4 (xvfb): **136 s**, 140/140 passed; full ctest (cold run): 687 s
- GPU/non-GPU split: see headline 2 and A5.

Honest gap: I did not do a "fully warm tree + warm ccache `--task` end-to-end" run myself (the cold run's reclaim deleted the trees,
and re-running the full chain costs another hour). The THREADS numbers (10-25 min, A1) stand in for it.

### A3
**release-build** (gcc `RelWithDebInfo`, whole tree, 934+ → 672 ninja steps depending on what's already built):
- inside the cold `--task`: 504 s (determinism had already built part of the tree; ccache 10 % hits)
- manual (no ccache, tree deleted before): configure 118 s + build **981 s**; no-op rebuild 0 s; after touching log.cpp 66 s; single object 3 s.
- THREADS rutin-2: 98 s warm; cold run died at target 828/934 after 442 s (disk). siradaki-milestone: stage ran 370 s in a batch.

**tidy** (cold run, `linux-clang-debug`): stage 797 s = configure (done in the parallel configure, 18 MB tree) + build of the whole
clang-debug tree (2746 steps, ≈ 780 s) + clang-tidy on the changed TU (log.cpp, 531 warnings generated, ~7 s). Tree 7.9 GB.
Manual: `cmake --preset linux-clang-debug` re-configure 4 s; `run-clang-tidy-18` on one file 7 s (tree already built).
THREADS: warm 103-351 s, cold clang-debug 420-750 s, 0 s when no C++ changed.

### A4
- Stage total (cold run): 631 s. Almost all of it is *building* targets in the debug and release trees (e.g. glslang, `mce_player`, samples).
- Single subjects, **release binary only, 3 runs each** (so roughly half of what `determinism.py` does with debug+release; debug is slower):
  court `-ticks 3000`: 1.5 s; 04_cubes 600: 2.2 s; cubes walk: 2.5 s; `net_rollback -frames 900`: 1.0 s; `net_headless` 8 clients: 1.1 s; 04_diorama: 1.1 s; boids 20000: 1.6 s.
  I did not time the MarbleGarden win route or debug binaries. The three subjects you guessed are all ~1-3 s → parallelising runs is not worth it; limiting *builds* is.
- Non-hash failures (THREADS): yes — disk-full (glslang compile "No space left" writing /tmp/cc*.s; rutin-3, m-access, rutin-1: 14 s fail), and `determinism.py --args "-headless …"` failing argparse (value starting with `-`; use `--args="…"`). A truncated `.o` after an interrupted build gives "ranlib: file truncated" (rutin-3). No flaky hash mismatch reported by anyone.

### A5
- `logs/2026-10-16-coord-container-ctest-N.txt` (list), `…-cold-task-linux-gcc-debug.txt` (per-test seconds in the ctest part), `…-ctest-nongpu.txt`.
- 208 tests total; 68 labelled `gpu`: **2121.6 sec*proc**; `sdk` label: `sdk_install` 94 s (1 test; THREADS saw 57-122 s). Sum of all per-test times 2577.8 s.
- Slowest (s): cinema_demo_player_gpu 270.7, test_render_sdlgpu 219.6, sample_06_showcase_gpu 209.9, sample_04_diorama_gpu 161.5,
  solstice_reach_gpu 128.5, test_render_gl 108.9, sdk_install 94.2, solstice_reach_editor_gpu 76.8, solstice_reach_walk_gpu 73.6,
  sample_04_cubes_gpu 66.2, sample_05_ashen_court_gpu 62.5, sample_07_gameplay3d_player_gpu 60.3, marble_garden_editor_gpu 46.2.
  Non-GPU slowest: room3d_editor_play_selftest 36.6 s.
- Wall: ctest is run with `-j $(nproc)` = 4, so the GPU group (many 60-270 s tests, llvmpipe/lavapipe) sets the 687 s wall.
- THREADS: GL editor tests fail with "No available video device" outside local_ci (no Xvfb) — not real failures. Hardware-GPU behaviour is untested by anyone (llvmpipe only).

### B1
`logs/2026-10-16-coord-container-ccache-stats-after-cold-task.txt` (ccache 4.9.1, after the cold `--task`):
Cacheable 3357 / 4665 (71.96 %); hits 357 (direct 75, preprocessed 282) = 10.63 %; misses 3000;
**uncacheable 1308 (28 %), all "Could not use precompiled header"**; local storage 0.9 GiB of max 5.0 (18.6 %) at that moment,
10 392 files, writes 6282. "Cleanups performed/evicted" not shown by 4.9.1 `-v` at this size (nothing evicted as far as I can tell).
(local_ci sets `CCACHE_MAXSIZE=3G` itself; the 5.0 figure is from a shell without that variable.)
THREADS: hit rates seen 7-100 %: 100 % on an immediate re-run, 66-87 % warm after a small change, 12-42 % after a merge from
dev_claude touching widely included headers (kit/render), 0.7 % after `ccache -C`. Several threads ran `ccache -C` or `-M 1G; -c`
to reclaim disk (rutin-1/2), which destroys the benefit; siradaki-milestone says deleting ccache costs more than its 3 GB.

### B2
Snapshot present: yes, `/mnt/project-files/.cache/ccache-linux.tar.zst`, **305 419 789 B (291 MB)**, last write
**2026-10-08 20:29:46 UTC** (8 days old at measurement; the script only refreshes after a *green full run* when >12 h old — and a full run
essentially never completes; see D4). Restore here: `=== ccache restored … (12s)`; m-views-render saw ~6 s. A fresh session logs it on every first run
in an empty `~/.cache/ccache` (every new container). Note also: this container did **not** have ccache or zstd installed until
`scripts/ci/setup_env.sh` ran (15 s); the snapshot restore needs zstd → a container that skipped setup_env would silently build uncached.

### B3
10.63 % hits on the first full build (357/3357); 28 % uncacheable (SDL3 PCH). Matches THREADS (m-views-render: 14 % hits, gcc-debug 1013 s on the first baseline;
rutin-2: 13-21 %). Cold gcc-debug build 694 s with snapshot vs 715 s without ccache at all → **the snapshot is effectively stale/useless** for current
dev_claude. Likely reasons: age (8 days), different compile flags per tree, SDL3 PCH uncacheable, and dev_claude churn.

### B4
`/mnt/project-files` is a **network FUSE mount** (`rclone` filestore: `fuse.rclone`, `max_read=1048576`), COORD: shared by every session in the
project; listing cheap, first read of each file downloads it; writes from other sessions take seconds to show; last write wins; no locking.
Hardlinks/mmap not to be relied on. So it is OK for one file written rarely (the tarball) but **not** usable as concurrent ccache secondary
storage (ccache's file-per-object layout would mean ~10 k small files over FUSE with no locking). Not tested by me.

### C1
Measured here: `nproc` = 4 (Intel Xeon @ 2.80 GHz); RAM 15 GB total (free ~4-14 GB during builds); `/dev/vda` 252 G nominal, **~39 GB per-session write
allowance** (COORD; `df` Avail shows 0 at ~39 G used). Tree sizes after the cold run: gcc-debug 15 G (rebuilt later by me), gcc-release 16 G,
clang-debug 7.9 G, `_fetch` 0.9 G, ccache 1.0 G. THREADS quote other sizes: asan 11-20 G, clang-release 1.9-8.8 G; one reports **8 cores** (m-gameplay3d), another `-j4` (rutin-1: "rule is -j4 only") — so containers may differ.
Shared host or not: **don't know** (each agent has its own container; whether the VMs share physical CPUs I cannot see). Wall times did not obviously vary with other sessions' load.

### C2
COORD: about 6-8 threads run CI concurrently, each in its own container; every task runs `--task` at least once before its PR, often 2-3 times after merges from dev_claude.
THREADS: "usually 2 per task, sometimes 3" (m-access); "1 + 2-3 partial re-verifications" (rutin-1); "~10 `--task` for 6 tasks" (m-script); 1-3 (m-gameplay3d). Per-day total: don't know; ballpark 20-40.

### C3
Cold run here:
```
=== disk: 14 GB free (want 22); removing build/linux-clang-debug, a later stage rebuilds it (18M)
=== disk: 14 GB free, less than the 22 GB wanted; going on
=== disk: 11 GB free (want 22); removing the finished build tree build/linux-gcc-debug (15G)
=== disk: 13 GB free (want 22); removing the finished build tree build/linux-gcc-release (16G)
```
It fired 3 times (4 lines). Removing the 18 M clang-debug tree was harmless (then tidy had to build it); the other two deleted trees *after* their last use
in that run. But a **second `--task` right after** would rebuild gcc-debug (≈ 12 min) and release (≈ 8-16 min) from nothing — so the next re-run is cold again.
(The disk wanted 22 GB but a 3-tree `--task` needs ≥ 39 GB: gcc-debug 15 + gcc-release 16 + clang-debug 8.)
THREADS (pre-#371): disk-full 5-6 times in ~2 days per thread (m-access ~6, m-script 5, rutin-2 ≥ 5, rutin-1 every `--batch` x3, rutin-3 2 in one batch) and always misread as code failures; remedy by hand: run one stage at a time and `rm -rf build/<preset>` after it. None had tested #371 yet.

### C4
Installed in this container (before setup_env): ld.lld 18.1.3 (present), ninja 1.11.1, gcc 13.3.0, clang 18.1.3, cmake 3.28.3, xvfb-run, run-clang-tidy-18; **not present**: mold, ccache, zstd (installed by setup_env.sh: ccache 4.9.1, zstd 1.5.5), emcc/EMSDK. No mold anywhere; nobody has tried mold/lld/unity builds/PCH (siradaki-milestone: "not tried").

### D1
THREADS: typical loop = edit → build only the changed target(s) (`cmake --build … --target X`, `ctest -R …`; `check_walls.py` + `capabilities.py --check`) → one full `--task` → open PR → merge `origin/dev_claude` again (GitHub says "405 merge conflicts" for 20-60 s after a push; dev_claude moves 8-10 commits/hour) → re-run `--task` or a smaller check (walls + capabilities + `mcp_transcript` + non-GPU ctest, ~1-10 min). 1-3 `--task` runs per task. Re-runs are on the same warm tree **unless** disk reclaim or the agent deleted trees (then cold); ccache hit rate drops to 12-42 % after merges that touch widely included headers.

### D2
Ranking (THREADS, consistent across all 8): 1) **disk full** in tidy/asan/determinism/release-build (not code; looks like many failures); 2) golden/generated files after merging dev_claude: `tests/mcp/transcripts/demo_edit.golden.jsonl` (`mcp_transcript`; also a stale `mce_mcp` binary looks like a golden diff), `docs/engine/capabilities.md`, `scripts/ci/sample_coverage.allow`; 3) real compile/-Werror/-Wconversion/-Wshadow errors in new tests (gcc-debug) and clang-tidy findings gcc misses (tidy); 4) other threads' breakage on dev_claude (GPU samples red from a shader-warning change; mcp_transcript component count); 5) web-stage screenshot timeouts under software WebGPU (04_diorama, 04_cubes, 05_ashen_court) — rutin-3, environmental; no flaky test seen by anyone. Merge conflicts in CHANGELOG/ROADMAP/capabilities.md on nearly every PR (not a CI failure but blocks merging).

### D3
They **wait**. Agents (and the tool harness) block on the run; foreground commands over ~2-10 min get backgrounded or killed (two `--task` runs lost at 10-30 min: rutin-2), so they run it detached and poll the log for `=== Summary`. A container restart kills background stages. So **latency matters**, and machine time of 6-8 concurrent containers is secondary (each has its own CPU).

### D4
COORD: `--batch` is run by the thread that owns the task, every 3rd task and at milestone end, on the PR branch or merged dev_claude; a red batch is fixed by that thread next. In practice (THREADS) a one-go `--batch` never fit the disk: all 8 threads run it stage by stage with `rm -rf` between stages. Culprit PR finding: don't know of any bisection procedure; the thread that ran it fixes it. Consequence: a **green full run on current dev_claude essentially never happens**, so the ccache snapshot (saved only after a green full run) is never refreshed (it is 8 days old).

### D5
`web` **skips** here (no EMSDK/emcc; also no Playwright checked) and "passes" in ~0 s (THREADS: "web 0 (skipped, no emsdk)"; rutin-3 saw it run earlier for SBA-126 for many minutes and fail on disk after 54 s). Web/Android/iOS/Windows/macOS builds are therefore unverified locally.

### E1
Hard no / tried and dropped (THREADS): deleting ccache to save disk (siradaki-milestone: rebuild cost far higher); lowering `MCE_CI_MIN_FREE_GB` to 16 (asan `sdk_install` failed with 1 GB free; 22 chosen empirically); one-go `--batch` (never fits); `-j8` not tried (rule is `-j4`, rutin-1; m-gameplay3d's container had 8 cores). Not tried by anyone: mold/lld, unity builds, PCH, ninja `-j` tuning, ctest-by-label across stages. I found no ADR forbidding any of your 7 proposals (I did not search the ADRs exhaustively — "don't know"). Caveat from m-views-render on (4): render behaviour lives in kit/player/editor too.

### E2
COORD (Sami's project instructions): `local_ci.sh --task` before every task PR, `--batch` every 3rd task and at milestone end; PRs that change only documentation (*.md, nothing built/tested/read by scripts) skip CI. Hosted GitHub Actions is removed (cost). Real-world gap (THREADS): hosted CI is gone so nothing runs on the PR or the merge result; a green run on an older head says nothing about merged dev_claude, so threads re-merge and re-test right before merging (cheap check suggested by rutin-1: walls + capabilities + transcript golden, <1 min). So (7) a merge-queue style run would *add* safety, not just save time.

### E3
COORD: threads merge their own PRs into dev_claude after a green local run. PRs touching core systems (memory, threading, render, ECS, physics) written by Sonnet-class threads wait for coordinator review. `local_ci.sh` changes can be merged by a thread after a green run (#363 and #371 went this way today); no separate sign-off from the user required.

### F
Run on the warm release tree here (RelWithDebInfo, 4 cores):
- Full `cmake --build --preset linux-gcc-release` after `touch log.cpp` (warm tree): **66 s** (nothing-changed no-op: 0 s). Cold full build with no ccache: 981 s; in the cold `--task`: 504 s.
- Single object `ninja -C build/linux-gcc-release src/engine/core/CMakeFiles/mce_core.dir/src/log.cpp.o`: **3 s**. (Your `ninja -t query … | grep '\.o$'` one-liner returned an empty path on ninja 1.11.1; the target path is `<dir>/CMakeFiles/<target>.dir/<src>.o` as listed by `ninja -t targets all`.)
- Tidy: `cmake --preset linux-clang-debug` 4 s (re-configure on an existing tree); `run-clang-tidy-18 -p build/linux-clang-debug -quiet '…/log\.cpp$'` **7 s, rc 0**. **Caveat:** the clang-debug tree was already fully built (from the cold run), so I could *not* show whether a configure-only tree suffices; I did not try it on a fresh configure. Generated headers/`_deps` would be the likely failure — untested.

## Questions back
1. Should the timing of **build vs ctest inside `linux-gcc-debug`** be printed by the script (it only prints one number)? I derived 694 s + 687 s by subtracting ctest's own total.
2. Is the CCACHE PCH sloppiness fix (`pch_defines,time_macros`) / disabling SDL3's PCH something you want me to *test* in this container (it needs a ~12-min rebuild per tree)? I did not touch the build.
3. Do you want a **fully warm** `--task` measured end to end (needs ≥ 40 GB free for 3 trees; a rerun ≈ 25-40 min here), or are the THREADS warm numbers enough?
4. For proposal (5), do you need the *write path* tested on `/mnt/project-files` (many small concurrent files over FUSE)? I'd expect it to be unusable but have not measured.
