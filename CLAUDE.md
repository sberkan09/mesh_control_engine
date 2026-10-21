# Working in this repository

Short rules for Claude sessions and humans. The engine itself is described in [README.md](README.md) and
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md); the plan is [docs/ROADMAP.md](docs/ROADMAP.md).

## Running CI

PRs target `master`. GitHub Actions (`.github/workflows/ci.yml`) runs `scripts/ci/local_ci.sh --task` on every PR on
standard hosted runners (free for this public repository); `.github/workflows/integration.yml` runs the full matrix
on `master` every 6 hours, refreshes the ccache and pushes `ci/regen-<sha>` for stale generated files. The same script
runs locally; the gate has three levels.

| Level | When | Command |
|---|---|---|
| PR gate | once per PR, on the PR's own base | `scripts/ci/local_ci.sh --task` |
| Pre-merge | right before merging, if `master` moved since the `--task` | `scripts/ci/local_ci.sh --premerge` |
| Integration | the full matrix, on the `master` head, by the integration workflow only | `scripts/ci/integration_run.sh` |

- `--task` runs once. After merging `master` into the branch do **not** run it again; run `--premerge`
  (walls, `capabilities --check`, the MCP transcript golden and `ctest -R` on the PR's own tests, 1-3 min). It also
  prints whether `master` changed files the PR changes too; only an overlap makes a `--task` re-run necessary.
- Threads never run `--batch` or the full matrix. The integration runner does (scheduled), refreshes the ccache
  snapshot and regenerates the generated files; if it goes red, the thread of the PR it names fixes forward.
- Generated files (`docs/engine/capabilities.md`, `scripts/ci/sample_coverage.allow`, the transcript golden
  `tests/mcp/transcripts/*.golden.jsonl`) are the integration runner's to regenerate. A stale one after merging
  `master` is not a reason to re-run `--task`.

### Container discipline

- Run `scripts/ci/setup_env.sh` before the first CI call (cloud sessions do it on start).
- `git fetch origin master` before `--task`: without the merge base every scoped stage runs in full and tidy
  checks the whole tree. A stage named on the command line always runs in full.
- Run `--task` detached and poll, never as a foreground command:
  `nohup scripts/ci/local_ci.sh --task > build/local-ci-logs/run.log 2>&1 &`, then wait for `=== Summary` in the log.
- Never delete or shrink the ccache (`ccache -C`, `-M`); the script's `reclaim_space` frees disk by removing
  finished build trees, which the cache rebuilds cheaply.
- Do not restart the container while a run is in flight.
- A message reaching your thread (from the user, the coordinator or a peer) restarts the container and kills detached
  runs, whatever `nohup` says (observed: every restart matched a message time to the second). So start `--task`, then
  do not expect a message-free half hour: after each wake check `uptime -s`, and if the run died continue with
  `--task --resume` (once #408 lands) instead of starting over. Long measurements belong in the scheduled
  integration run, which gets no messages.

## Change rules that keep builds cheap

- **Changelog:** add `changelog.d/<ticket>.md`, never edit `CHANGELOG.md` (see `changelog.d/README.md`).
  The coordinator updates the ROADMAP status tables in batches; a PR does not edit them.
- **Wide headers:** a header included by many translation units (the walls stage prints a warning above 150) ships as
  its own small seam PR, merged first. Prefer changing a `.cpp` to changing a header; forward-declare; do not grow
  umbrella headers. Check with `scripts/ci/include_fanout.py`.
- **Slow tests:** a new test over 60 s ships a smoke variant (fewer ticks or frames, one scene) that `--task` runs,
  and the full version is labelled `slow` with `mce_mark_slow(<test>)` (`cmake/MceTarget.cmake`); the integration
  run executes it (`local_ci.sh slow`). Adding a determinism subject means adding it to the module-to-subject map (once that map exists in
  `local_ci.sh`, a row in it). A caller that passes `-LE <expr>` to `ctest --preset` must include `slow` in it.
- **Task shape:** concurrent threads work on disjoint modules; a task that touches three modules is split; stack PRs
  (seam, then feature, then polish).
