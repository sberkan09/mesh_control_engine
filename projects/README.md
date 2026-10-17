# projects/

Projects made with the project template live here, one directory each:

    python3 scripts/new_project.py MyGame        # or scripts/new_project.sh / scripts\new_project.bat

The engine's CMake adds every `projects/<name>/` that has a `CMakeLists.txt`. After a reconfigure,
these targets exist: `<game>` (the host), `<game>_game` (the game module), `<game>_cook`,
`<game>_editor` and `<game>_mcp` (the editor's MCP server with the game's components). Ship a project with `python3 scripts/ship.py --project projects/<name>`.
The layout and the rules are in `docs/architecture/assets.md` §4.4; the record is
`docs/milestones/M8-project-template.md`.

`Kindle/` is the UC-D proof game (ROADMAP M9, `docs/milestones/M9-ucd.md`).

`PrismBreak/` is the MCP dogfood game: a brick breaker authored through `prism_break_mcp` (`docs/research/mcp-dogfood.md`).

`Undertow/` is the second MCP dogfood game: a lit-2D cave-diving drone with a rope, currents and hazards, authored through `undertow_mcp` (`docs/research/mcp-dogfood-undertow.md`).

`SolsticeReach/` is the 3D graphics showcase: an island walked through eight lighting regimes, with procedural content and a scene authored through `solstice_reach_mcp` (`docs/design/solstice-reach.md`).

`Undelivered/` is the narrative showcase: a light novel on the 2D kit, its script compiled to tables and its save game the whole run state (`docs/design/undelivered.md`).

The third round, `docs/research/mcp-blind-test.md`, made a game without the engine source (an SDK-like setup); that project is kept in the project files, not here.

`Glowmoth/` and `MarbleGarden/` are the M-AGENT demo games (`docs/research/m-agent-demo/`): each was made
from a one-paragraph brief by AI agents over MCP, with no person after the brief. Glowmoth (a 2D
pixel-art platformer) by one agent; Marble Garden (a 3D marble game) by three agents sharing the project
(level, code and art). Their `.mctest` suites and render goldens run in local CI through
`scripts/agent_demo/demo_gate.py`.

`Pursuit/` is the 3D script-only demo (M-SCRIPT2): a chase over a navmesh written in Luau and data, with no C++. `Dungeon/` is its 2D twin: procedural levels (ProcLevel) with chasers.
