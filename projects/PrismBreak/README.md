# Prism Break

A small brick breaker, made with the MCE project template and the project editor's MCP server as a
dogfood test of the MCP workflow (`docs/research/mcp-dogfood.md` has the findings). A paddle, a
ball and a wall of 55 bricks: normal bricks take 1 to 3 hits and are tinted by the hits they have
left, prism bricks (the colour-cycling ones) split the ball in two, and steel bricks never break.
Clear every breakable brick to win; three lives.

Controls: A / D or the arrows move the paddle, Space serves (and starts over after a win or a
loss), Esc pauses, ` opens the console. Where the ball lands on the paddle sets its angle.

| Path | What |
|---|---|
| `PrismBreak.mceproj` | project identity: name, engine version, games, start scene |
| `assets/scenes/main.mcscene` | the level, authored through `prism_break_mcp` (`tools/author_level_mcp.py` is the record) |
| `assets/scenes/attract.mcscene` | the same level with the paddle on autopilot (`Paddle.autopilot`): it plays itself |
| `assets/textures/` | sprites drawn by `tools/gen_art.py` |
| `assets/sfx/`, `assets/fonts/` | reused repository / CC0 content (`CREDITS.md`) |
| `games/prism_break/module/` | the game module: `components.h` components, `prism_break.cpp` systems + `install`, `module.cpp` entry, `types.cpp` for the cook |
| `games/prism_break/host/` | the thin host executable (the engine's player runtime + this game) |

From the engine checkout:

    cmake --preset linux-gcc-debug                      # picks up projects/PrismBreak
    cmake --build --preset linux-gcc-debug --target prism_break prism_break_editor prism_break_mcp
    build/linux-gcc-debug/projects/PrismBreak/prism_break                                   # play
    build/linux-gcc-debug/projects/PrismBreak/prism_break -scene scenes/attract.mcscene     # watch it play itself
    build/linux-gcc-debug/projects/PrismBreak/prism_break_editor -project projects/PrismBreak  # edit
    python3 projects/PrismBreak/tools/author_level_mcp.py --mcp build/linux-gcc-debug/projects/PrismBreak/prism_break_mcp
    python3 scripts/ship.py --project projects/PrismBreak   # the ship zip in dist/

`author_level_mcp.py` rebuilds both scenes from the sketch at its top, then plays the attract scene
for 12 000 ticks through `run_headless` and reads the bricks back with `inspect`: it reports how many
breakable bricks are still standing (0: the autopilot cleared the wall). ctest
`prism_break_attract_clears` checks the same with the host.
