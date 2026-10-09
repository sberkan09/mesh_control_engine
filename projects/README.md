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
