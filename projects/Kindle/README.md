# Kindle

The UC-D proof game (ROADMAP M9, `docs/milestones/M9-ucd.md`): one small 2D level made with the
MCE project template and the project editor's MCP server. It reuses Ember Step's mechanics (the
platformer controller, coins, spikes, the goal flag, crumbling blocks, a moving platform) as authored
components, and adds one mechanic: **colour switches**. Touching a switch swaps which block colour
is solid. Red starts solid; the red wall needs the first switch, the blue bridge over the pit needs
it too, and the red stairs to the flag need the second.

Controls: A / D or the arrows run, Space jumps (again off a wall), J dashes, Esc pauses, ` opens the
console. Gamepad: left stick / d-pad, A, X.

| Path | What |
|---|---|
| `Kindle.mceproj` | project identity: name, engine version, games, start scene |
| `assets/scenes/main.mcscene` | the level, authored through `kindle_mcp` (`tools/author_level_mcp.py` is the record) |
| `assets/textures/ember/` | Ember Step's sprites, exported from `samples/03_platformer/art.cpp` |
| `assets/textures/switch/` | the switch and the red / blue blocks (`tools/gen_switch_art.py`) |
| `assets/textures/backdrop/`, `assets/sfx/`, `assets/fonts/` | reused repository / CC0 content (`CREDITS.md`) |
| `games/kindle/module/` | the game module: `components.h` components, `kindle.cpp` systems + `install`, `module.cpp` entry, `types.cpp` for the cook |
| `games/kindle/host/` | the thin host executable (the engine's player runtime + this game) |
| `tests/test_kindle.cpp` | ctest `kindle_level`: the switch mechanic and a scripted run to the flag |

From the engine checkout:

    cmake --preset linux-gcc-debug                      # picks up projects/Kindle
    cmake --build --preset linux-gcc-debug --target kindle kindle_editor kindle_mcp
    build/linux-gcc-debug/projects/Kindle/kindle                               # play
    build/linux-gcc-debug/projects/Kindle/kindle_editor -project projects/Kindle  # edit
    python3 projects/Kindle/tools/author_level_mcp.py --mcp build/linux-gcc-debug/projects/Kindle/kindle_mcp
    python3 scripts/ship.py --project projects/Kindle --smoke --smoke-headless   # the ship zip in dist/
