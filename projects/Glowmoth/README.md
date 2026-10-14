# Glowmoth

Made with the MCE project template (`scripts/new_project.py`). Layout (assets.md §4.4):

| Path | What |
|---|---|
| `Glowmoth.mceproj` | project identity: name, engine version, games, start scene |
| `assets/` | the content you edit; the editor opens this directory. `cooked/` and `assets.mce` are build outputs |
| `games/glowmoth/module/` | the game module: `components.h` components, `glowmoth.cpp` systems + `install`, `module.cpp` entry, `types.cpp` for the cook |
| `games/glowmoth/host/` | the thin host executable (the engine's player runtime + this game) |
| `CREDITS.md` | third-party content (the template's ProggyClean font, MIT); `mce ship` / `ship.py` copy it into the shipped folder |

With the installed SDK (`mce` on your PATH; chapter 1 of the user guide):

    mce build Glowmoth                 # find_package(MCE) + build into Glowmoth/build
    mce play Glowmoth                  # play (Esc pauses, ` opens the console)
    mce editor Glowmoth                # the installed editor with libglowmoth loaded
    mce mcp Glowmoth                   # the MCP server for an AI agent (user guide chapter 14)
    mce ship Glowmoth --version 1.0    # the ship zip in Glowmoth/dist/

From the engine checkout:

    cmake --preset linux-gcc-debug                      # picks up projects/Glowmoth
    cmake --build --preset linux-gcc-debug --target glowmoth
    build/linux-gcc-debug/projects/Glowmoth/glowmoth     # play (Esc pauses, ` opens the console)
    python3 scripts/ship.py --project projects/Glowmoth # the ship zip in dist/

Add components to `components.h` (MCE_COMPONENT), systems to `glowmoth.cpp` (`install`), and author
them in scenes with the editor, which loads the game module to know its components:

    cmake --build --preset linux-gcc-debug --target mce_editor glowmoth_game
    build/linux-gcc-debug/src/editor/app/mce_editor -module build/linux-gcc-debug/projects/Glowmoth/libglowmoth.so -project projects/Glowmoth

(On Windows, where game modules are not built, the project's own `glowmoth_editor` target compiles the
game in instead.) An MCP client (an AI agent) authors the same scenes through the MCP server,
`mce_mcp --module <that libglowmoth.so> --project projects/Glowmoth` (docs/engine/editor.md), and
`mce_cook --module <that libglowmoth.so> projects/Glowmoth --pack` cooks them. In the windowed game the starter-kit input
map is live (WASD / arrows move, Space jumps, J fires), but the template's `glowmoth.cpp` does not
read it yet: a system that wants it reads the `InputState` resource (user guide chapter 11).
