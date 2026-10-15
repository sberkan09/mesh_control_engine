# M-AGENT demo: games made from a brief, with no person after it

The M-AGENT milestone ends with this demo: agents that have never seen Mesh Control Engine get a
one-paragraph brief and ship a finished game through the MCP server alone. One agent works solo;
a team of three (**level**, **code**, **art**) works at the same time on one project. Each run made
a 2D game (Glowmoth) or a 3D game (Marble Garden) with a title screen, levels, win and lose
screens, credits, a passing `project_check`, green `.mctest` suites and a shipped zip.

The final games are checked in as [`projects/Glowmoth`](../../../projects/Glowmoth) (round 2,
solo) and [`projects/MarbleGarden`](../../../projects/MarbleGarden) (round 2, team), and their
tests, goldens and determinism run in `scripts/ci/local_ci.sh` (see "Gates" below).

## What the agents were given

- The briefs: [2D, Glowmoth](briefs/2d.md) and [3D, Marble Garden](briefs/3d.md).
- The instructions, word for word: [solo](briefs/agent_solo.md) and [team](briefs/agent_team.md).
- An installed SDK, a project made by `mce new`, and the MCP server through
  `scripts/agent_demo/mcp_bridge.py` (set up by `scripts/agent_demo/setup_run.sh`). They were
  told not to read the engine source. Nothing else was said to them during a run.

## The runs

Two rounds. Round 1 found what the engine lacked; its friction notes were fixed (commit
c96f075: run-time scene changes, module reload, logging, `gen_pixels`, `gen_mesh`, ...) and
round 2 is a blind re-run by fresh agents on the fixed engine. Round 2's notes were fixed after
it (commit f687cc1).

| Run | Game | Agents | Tool calls | project_check | Tests | Zip | Friction (blocking / major / minor) |
|---|---|---|---|---|---|---|---|
| [round1/solo-2d](round1/solo-2d) | Glowmoth | solo | 1050 | pass, 0 warnings | 5 `.mctest` | yes | 1 / 3 / 4 |
| [round1/solo-3d](round1/solo-3d) | Marble Garden | solo | 333 | pass, 0 warnings | 2 `.mctest`, 4 module tests | yes | 1 / 3 / 3 |
| [round1/team-2d](round1/team-2d) | Glowmoth | level, code, art | 468 + 56 + 278 | pass, 3 contrast warnings | 8 `.mctest` | yes | 3 / 11 / 7 |
| [round2/solo-2d](round2/solo-2d) | Glowmoth | solo | 745 | pass, 0 warnings | 8 `.mctest` | yes | 0 / 1 / 4 |
| [round2/solo-3d](round2/solo-3d) | Marble Garden | solo | 1718 | pass, 0 warnings | 8 `.mctest` | yes | 0 / 2 / 4 |
| [round2/team-3d](round2/team-3d) | Marble Garden | level, code, art | 694 + 111 + 265 | pass, 0 warnings | 10 `.mctest` | yes | 0 / 1 / 12 |

Each run folder holds:

- `prompt*.md`: the exact instructions that agent got.
- `SUMMARY*.md`: the agent's own account (what the game is, how to play, what it could not do).
- `<agent>/transcript.jsonl`: every MCP call and its answer, in order. Long strings are cut to
  1500 characters and long lists to their first 40 items (`scripts/agent_demo/record.py`).
- `<agent>/sheet_<n>.png`: contact sheets of every image the agent looked at, in order.
- `friction.md`: the project's `report_friction` notes, through `mce friction`.

## Gates

| Gate | Evidence |
|---|---|
| A blind re-run by fresh agents finishes with no blocking workaround | Round 2: all three runs shipped, and none filed a blocking note (table above). |
| The multi-agent run loses no edit | `scripts/agent_demo/check_team_run.py` on both team runs: "gate: pass (no edit lost)", with 0 merged saves and 0 resolves needed. |
| render_frame goldens are checked in CI | `glowmoth_goldens_gpu` and `marble_garden_goldens_gpu` (ctest label `gpu`) diff every frame in each project's `goldens/goldens.json` with `scripts/agent_demo/demo_gate.py --goldens`. `marble_garden_win_route_gpu` renders the scripted win route. |
| The games' tests stay green | `glowmoth_mctests` and `marble_garden_mctests` run every `.mctest` through `demo_gate.py --tests` on a fresh copy; `marble_garden_win_route` plays the win route in the shipped host. |
| Determinism holds | `stage_demo_games_determinism` in `local_ci.sh` runs `determinism.py` on both games (Glowmoth level 1 for 1200 ticks; Marble Garden's win route for 1800 ticks) in debug and release. |
| `local_ci.sh` is green | Every stage, with the two games in it. |

## What the friction notes changed

Round 1 (fixed in c96f075): no way to change scenes from game code; `module_build` kept the old
module; game logs never reached the tools; scene links made cook cycles; `gen_sprite` could not
be steered (now `gen_pixels`); no mesh generator (now `gen_mesh`); `asset_schema` broke the stdio
stream; `module_test` found nothing in an SDK project; `project_set` cleared `start_scene`;
`render_frame` ran late ticks in real time.

Round 2 (fixed in f687cc1):

- `module_build` now reloads a changed component's fields (the host drops a module's reflection
  when it unloads it).
- `render_frame` / `render_sheet` read `-input` by sim tick, so a rendered run plays like
  `run_headless`.
- `walk_to` takes `through` waypoints and `air_jumps`, settles after arrival, and a scene change
  ends a bot's level instead of counting as a death.
- `.mctest` event checks take the short event name and fail on an unknown one.
- Entity references in component values accept names; `inspect` resolves in the played scene.
- `project_check` measures text contrast against the sprite behind it, flags `CREDITS.md` rows
  for files that are gone, and counts input maps as used.
- `RigidBody3D.stop_spin`; a music source waits for the same stream to fade out; `MCE_LOG_*`
  refuses printf formats at compile time; the docs name the headers and namespaces agents
  looked for.

Still open, all minor:

- No particle system for 3D scenes (`.mcpart` is 2D only).
- `render_frame` cooks the whole project before each frame.
- `asset_delete` does not remove the file's `CREDITS.md` row (`project_check` now flags it).

## A harness incident

In round 2 the team's **level** agent first kept its helper scripts in a scratchpad shared with
the solo-3d agent. That agent's `mce.py` replaced the level agent's, and for a few calls the level
agent edited the solo-3d project, overwriting its still-untouched template `main.mcscene`. The
solo-3d agent had re-saved it before this was found, and the level agent moved its scripts into
its own `agents/level/work/`. This was the demo harness, not the engine: each agent now gets its
own work folder from `setup_run.sh`.
