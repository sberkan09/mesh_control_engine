# MarbleGarden

Made with the MCE project template (`scripts/new_project.py`). Layout (assets.md §4.4):

| Path | What |
|---|---|
| `MarbleGarden.mceproj` | project identity: name, engine version, games, start scene |
| `assets/` | the content you edit; the editor opens this directory. `cooked/` and `assets.mce` are build outputs |
| `games/marble_garden/module/` | the game module: `components.h` components, `marble_garden.cpp` systems + `install`, `module.cpp` entry, `types.cpp` for the cook |
| `games/marble_garden/host/` | the thin host executable (the engine's player runtime + this game) |
| `CREDITS.md` | third-party content (the template's ProggyClean font, MIT); `mce ship` / `ship.py` copy it into the shipped folder |

With the installed SDK (`mce` on your PATH; chapter 1 of the user guide):

    mce build MarbleGarden                 # find_package(MCE) + build into MarbleGarden/build
    mce play MarbleGarden                  # play (Esc pauses, ` opens the console)
    mce editor MarbleGarden                # the installed editor with libmarble_garden loaded
    mce mcp MarbleGarden                   # the MCP server for an AI agent (user guide chapter 14)
    mce ship MarbleGarden --version 1.0    # the ship zip in MarbleGarden/dist/

From the engine checkout:

    cmake --preset linux-gcc-debug                      # picks up projects/MarbleGarden
    cmake --build --preset linux-gcc-debug --target marble_garden
    build/linux-gcc-debug/projects/MarbleGarden/marble_garden     # play (Esc pauses, ` opens the console)
    python3 scripts/ship.py --project projects/MarbleGarden # the ship zip in dist/

Add components to `components.h` (MCE_COMPONENT), systems to `marble_garden.cpp` (`install`), and author
them in scenes with the editor, which loads the game module to know its components:

    cmake --build --preset linux-gcc-debug --target mce_editor marble_garden_game
    build/linux-gcc-debug/src/editor/app/mce_editor -module build/linux-gcc-debug/projects/MarbleGarden/libmarble_garden.so -project projects/MarbleGarden

(On Windows, where game modules are not built, the project's own `marble_garden_editor` target compiles the
game in instead.) An MCP client (an AI agent) authors the same scenes through the MCP server,
`mce_mcp --module <that libmarble_garden.so> --project projects/MarbleGarden` (docs/engine/editor.md), and
`mce_cook --module <that libmarble_garden.so> projects/MarbleGarden --pack` cooks them. In the windowed game the starter-kit input
map is live (WASD / arrows move, Space jumps, J fires), but the template's `marble_garden.cpp` does not
read it yet: a system that wants it reads the `InputState` resource (user guide chapter 11).
