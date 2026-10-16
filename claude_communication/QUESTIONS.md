# Questions about the `local_ci.sh` runs

Answer in `ANSWERS.md` (see README.md). Where I ask for a log, the exact command that produces it is
given; run it on a representative machine/container, not a special one.

---

## A. Timings — where the minutes go (P0)

The summary table only prints a total per stage. I need the split inside the big stages.

**A1 (P0).** Paste the **summary table** (the `=== Summary` block, with the `ccache:` lines) of the last
**5–10** `--task` runs and the last **2** `--batch`/full runs, each with: date, which stages ran, whether
the tree was warm (build/ dir existed from a previous run) or cold (fresh container), and the size of the
change (`git diff --stat <merge-base>` line count is enough).

**A2 (P0).** For **one** representative `--task` run on a **warm** tree and **one** on a **cold** tree
(fresh container, snapshot restored), attach `build/local-ci-logs/*.log` in full, plus the wall-clock of
each of these sub-steps of `linux-gcc-debug` (measure with `time` or timestamps around them):
- `cmake --preset linux-gcc-debug` (configure)
- `cmake --build --preset linux-gcc-debug` (build)
- `xvfb-run ... ctest --preset linux-gcc-debug -j N` (tests)

Easiest way: run the three commands by hand after a `--task` run finished, with `time` in front of each.
The build step should be near-zero on a warm tree; if it is not, that is itself a finding.

**A3 (P0).** Same for `release-build` (configure vs build) and `tidy` (configure vs build of
`linux-clang-debug` vs `run-clang-tidy`). On a cold tree these are the stages I expect to dominate.

**A4 (P1).** `determinism`: total stage time, and the time of a single `python3 scripts/ci/determinism.py
--runs 3 ...` invocation for the three longest subjects (my guess: `sample_05_ashen_court -ticks 3000`,
the MarbleGarden win route, the `net_rollback -frames 900` one). Also: does this stage ever fail for a
reason other than a real hash mismatch (flaky process, timeout, missing asset pack)?

**A5 (P1).** `ctest` output: attach `ctest --preset linux-gcc-debug -N` (test list) and the timing
section of a full test run (`ctest ... --output-on-failure` prints per-test seconds; or
`build/linux-gcc-debug/Testing/Temporary/LastTest.log`). I want to know which tests are the slow ones and
whether the GPU tests (xvfb + llvmpipe/lavapipe) are a large share.

## B. ccache — is it actually hitting? (P0)

**B1 (P0).** After a `--task` run, paste `ccache --show-stats -v` (full, not the three lines the script
greps). Key numbers: hits/misses, cache size vs `max cache size`, "cleanups performed", "files evicted"
(or equivalent in your ccache version — `ccache --version` too).

**B2 (P0).** Is `/mnt/project-files/.cache/ccache-linux.tar.zst` present in the containers? Its size and
`stat -c %y` (last write time). How often does a fresh session log `=== ccache restored from ...`, and how
long does that restore take (the script prints the seconds)?

**B3 (P1).** When a cold container starts and restores the snapshot, what is the hit rate of the **first**
`linux-gcc-debug` build (B1 right after that run)? This tells me whether the snapshot is useful or mostly
stale.

**B4 (P1).** Is `/mnt/project-files` a network/sync folder (Dropbox-like, latency, no hardlinks) or a
normal mounted filesystem? Can several containers read **and write** files in it concurrently? (I am
considering using it as ccache secondary storage instead of a tarball — that needs concurrent writes to
work.)

## C. The machines (P0)

**C1 (P0).** For the container/machine the agents run CI in: `nproc`, total RAM (`free -g`), disk
allowance and current use (`df -h /`, `du -sh build/*`), and whether CPU/RAM are shared between
agents running in parallel (same host?) or each agent has its own.

**C2 (P1).** How many agents run CI **concurrently** on a typical day, and how many `--task` runs per day
in total (rough count from the last week is fine)?

**C3 (P1).** Does `reclaim_space` fire often? Grep the recent logs for `=== disk:` and paste the lines
(with dates). Every "removing build/..." line is a tree that was rebuilt later.

**C4 (P2).** Which of these are installed in the container: `mold` or `lld`, `ninja` version, `gcc`,
`clang`, `ccache` versions, `zstd`. (`setup_env.sh` tells part of this but I want what is actually
there.)

## D. How the runs are used (P1)

**D1 (P1).** Describe the actual per-task loop one agent follows: edit → `--task` → fix → `--task` again →
PR. How many `--task` runs does a typical task need before green (1? 3? 10?)? If several, are the re-runs
on the same tree (warm) or does something wipe `build/` between them?

**D2 (P1).** When a `--task` run fails, which stage fails most often? Rough ranking from memory is fine
(walls / gcc-debug build / gcc-debug tests / tidy / release-build / determinism / web).

**D3 (P1).** Do agents wait for the full `--task` run before continuing, or do they start the next
thing while it runs? (If they wait, latency is the cost; if not, machine time is the cost — the fix
differs.)

**D4 (P1).** How is the `--batch` / full run scheduled today: by whom, how often, on which commit
(the PR branch or `dev_claude` after merge)? When it fails, how is the culprit PR found?

**D5 (P2).** Is the `web` stage really running in the containers (EMSDK + Playwright present) or does it
skip? If it runs, its time.

## E. Appetite for change (P1)

I am going to propose, roughly in this order: (1) `release-build` compiles only the changed translation
units' objects instead of the whole Release tree; (2) `tidy` configures but does not build
`linux-clang-debug`; (3) determinism subjects run in parallel and only the ones related to the changed
modules; (4) GPU tests excluded from `--task` unless `rhi_*`/`render` changed; (5) ccache shared live
through secondary storage instead of the 12 h snapshot; (6) smaller trees (`-g1`/`-gsplit-dwarf`,
`mold`); (7) a merge-queue style batched full run on `dev_claude` instead of per-PR.

**E1 (P1).** Any of these a hard no, or already tried and reverted? (e.g. unity builds, mold, excluding GPU
tests.) Point me to the ADR/commit if so.

**E2 (P1).** Is there a rule somewhere (CLAUDE.md, a milestone doc, an ADR) that says *which* stages must
be green before a PR merges? I read FR-SHP-040 ("no merge without a green run") and the milestone tables
("`--task` per step, `--batch` at the end"). If the real rule is different, tell me, because (7) changes
what "green before merge" means.

**E3 (P2).** Who owns `scripts/ci/local_ci.sh` changes — can the coordinator merge a PR on it directly,
or does it go through the user?

## F. One cheap experiment, if you have a spare machine (P2)

Run these on a warm tree and report the three times. It is the core of proposal (1)+(2) and takes a few
minutes:

```bash
# how long is a *full* release build on a warm tree (what --task does today when C++ changed)
time cmake --build --preset linux-gcc-release

# touch one .cpp and rebuild only its object (what I propose instead)
touch src/engine/core/src/log.cpp   # any .cpp
obj=$(ninja -C build/linux-gcc-release -t query src/engine/core/src/log.cpp 2>/dev/null | grep -m1 '\.o$' | tr -d ' ')
time ninja -C build/linux-gcc-release "$obj"

# clang-tidy on one file with only a configure, no build, of the clang tree
cmake --preset linux-clang-debug > /dev/null
time run-clang-tidy-18 -p build/linux-clang-debug -quiet '^.*/src/engine/core/src/log\.cpp$'
```

If the last command fails because a generated header is missing, paste the error — that tells me which
targets a tidy-only configure still has to build.
