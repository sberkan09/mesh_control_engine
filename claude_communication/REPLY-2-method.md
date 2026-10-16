# Reply 2 — the development method, not the script (2026-10-16)

REPLY-1 is about "how many minutes per run". This file is about "how many runs per task, and how many of
them are wasted". From your answers, the second question currently costs more than the first. Everything
below is a proposal for the coordinator; none of it is in my PR. For each item, please answer
**accept / reject / later**, with a line of reasoning if rejected, under `## Reply 2 decisions` in
ANSWERS.md.

## M1. Stop verifying integration per thread; verify it once, on `dev_claude` — HIGH

Evidence (D1, C2): `dev_claude` moves 8-10 commits/hour; each thread merges it into the branch before the
PR and re-runs `--task`; "usually 2, sometimes 3" `--task` runs per task come from that. More than half of
a task's CI minutes verify other people's commits, and the result is stale 20 minutes later anyway.

Proposal:
- A PR goes green with **one** `--task` on its own base. No re-run after merging `dev_claude` unless the
  merge touched files the PR also touches (git can tell: a non-empty intersection of the two diffs).
- Integration is verified by **one integration runner** on `dev_claude` head (REPLY-1 P8): a session or a
  scheduled task, every N hours or every K merges, stage by stage with the disk strategy. It also refreshes
  the ccache snapshot. Threads stop running `--batch`.
- **Merge windows**: the coordinator merges the batch of green PRs together, then one full run covers all
  of them. N PRs → one full run instead of N. A red run is bisected over that batch (cheap with targeted
  rebuilds); the thread whose PR is at fault fixes forward.

## M2. Kill the files that conflict on every PR — HIGH

Evidence (D2): CHANGELOG / ROADMAP / `docs/engine/capabilities.md` conflicts "on nearly every PR";
generated goldens (`tests/mcp/transcripts/demo_edit.golden.jsonl`, `capabilities.md`,
`scripts/ci/sample_coverage.allow`) are the #2 failure after merging `dev_claude`. Every conflict is one
more merge and one more CI run.

Proposal:
- CHANGELOG: one fragment file per PR (`changelog.d/<ticket>.md`), concatenated by a script at release /
  milestone end (towncrier model). No two PRs touch the same line.
- ROADMAP / milestone status tables: updated by the coordinator in batches (in the merge window), not by
  each thread in its PR.
- Generated files (`capabilities.md`, the sample-coverage allow list, transcript goldens): regenerated and
  committed by the integration runner after the merge window, not hand-merged by threads. The PR check
  stays (`--check`), but a stale golden after a merge is the runner's job, not a reason to re-run `--task`.

## M3. Seam PRs for wide headers — MEDIUM

Evidence (B1): ccache hit rate drops to 12-42 % for everyone after a merge that touches kit/render headers.
One thread editing a widely included header makes the next build cold in 7 other containers.

Proposal:
- The M-MAC "seam PR" rule becomes general: a change to a header included by many TUs ships as its own
  small PR, merged first; feature PRs come after it. One cold build for everyone instead of one per
  feature PR that happens to carry it.
- `check_walls.py` (or a new `scripts/ci/include_fanout.py`) prints, for each changed header, how many TUs
  include it; above a threshold (say 150) the PR description must say why. Visible cost changes behaviour.
- Coding rule: prefer changing a `.cpp` to changing a header; forward-declare; split umbrella headers
  (`mce/kit.h`-style) when a feature needs one more type.

## M4. A cost budget for tests and determinism subjects — MEDIUM

Evidence (A5, A4): single GPU tests of 271 s (`cinema_demo_player_gpu`), 220 s, 210 s — one test longer
than the entire non-GPU suite (136 s). Determinism subjects grow by one or two per milestone (court,
cubes, marble garden, ember script …) and every one of them runs on every task. Nothing ever gets
cheaper; the stage time grows linearly with the milestones.

Proposal:
- Any new test over 60 s must ship a CI "smoke" variant (fewer ticks, lower resolution, one scene) for
  `--task`; the full version carries a `slow` label and runs only in the integration run.
- Adding a determinism subject means adding a row to the module→subject map (REPLY-1 P4) — it is a PR
  checklist item, so the per-task selection stays correct.
- Once a quarter, the 10 slowest tests are reviewed: shorten, merge, or move to `slow`.

## M5. Task shape and assignment — MEDIUM

Small PRs that touch one module are where targeted CI (P2-P4) pays off and where conflicts are rare.
- Assign concurrent threads to disjoint modules where the roadmap allows; the file-ownership idea from
  M-MAC already exists — use it per milestone.
- A task that touches three modules is usually two cheap runs if split, one expensive run if not. Split.
- Stack PRs (seam → feature → polish) rather than one PR that is all three.

## M6. Container discipline, written down — LOW effort, real savings

From the thread notes: `ccache -C` / `-M 1G` to free disk (next build +15-20 min), trees deleted by hand,
foreground `--task` runs killed at 10-30 min, background stages killed by container restarts, sessions that
skipped `setup_env.sh` building uncached.

Proposal — a short "Running CI" section in CLAUDE.md:
- Never delete or shrink the ccache; let `reclaim_space` handle disk.
- `--task` is always run detached (`nohup … > build/local-ci-logs/run.log &`) and the thread polls for
  `=== Summary`; never as a foreground command.
- Don't restart the container while a run is in flight.
- `setup_env.sh` has run before the first CI call (the script will also warn).

## M7. Define the gate explicitly — LOW effort

Today's rule: `--task` before every PR, `--batch` every 3rd task. In practice threads also run a
home-made 1-minute check after merging `dev_claude` (walls + capabilities + transcript golden). Make it
official so nobody invents their own and nobody runs a full `--task` for it:
- `--task` on the PR's base → green.
- `--premerge` (new, ~1-3 min: walls, capabilities, transcript golden, `ctest -R` on the PR's own test
  targets) right before merging, if `dev_claude` moved.
- Full matrix: integration runner only (M1).

## Priority

M1 and M2 cut the number of runs; M3 cuts cold builds; M4 stops the growth; M5-M7 are hygiene. M1, M2,
M7 are coordinator decisions; M3, M4 are coding rules; M6 is habit. If only one is accepted, make it M1:
it is where the minutes are.
