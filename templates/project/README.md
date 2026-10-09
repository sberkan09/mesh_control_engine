# @PROJECT@

Made with the MCE project template (`scripts/new_project.py`). Layout (assets.md §4.4):

| Path | What |
|---|---|
| `@PROJECT@.mceproj` | project identity: name, engine version, games, start scene |
| `assets/` | the content you edit; the editor opens this directory. `cooked/` and `assets.mce` are build outputs |
| `games/@GAME@/module/` | the game module: `components.h` components, `@GAME@.cpp` systems + `install`, `module.cpp` entry, `types.cpp` for the cook |
| `games/@GAME@/host/` | the thin host executable (the engine's player runtime + this game) |

From the engine checkout:

    cmake --preset linux-gcc-debug                      # picks up projects/@PROJECT@
    cmake --build --preset linux-gcc-debug --target @GAME@
    build/linux-gcc-debug/projects/@PROJECT@/@GAME@     # play (Esc pauses, ` opens the console)
    python3 scripts/ship.py --project projects/@PROJECT@ # the ship zip in dist/

Add components to `components.h` (MCE_COMPONENT), systems to `@GAME@.cpp` (`install`), and author
them in scenes with the editor, which loads the game module to know its components:

    cmake --build --preset linux-gcc-debug --target mce_editor @GAME@_game
    build/linux-gcc-debug/src/editor/app/mce_editor -module build/linux-gcc-debug/projects/@PROJECT@/lib@GAME@.so -project projects/@PROJECT@

(On Windows, where game modules are not built, the project's own `@GAME@_editor` target compiles the
game in instead.) An MCP client (an AI agent) authors the same scenes through the MCP server,
`mce_mcp --module <that lib@GAME@.so> --project projects/@PROJECT@` (docs/engine/editor.md), and
`mce_cook --module <that lib@GAME@.so> projects/@PROJECT@ --pack` cooks them. In the windowed game the starter-kit input
map is live (WASD / arrows move, Space jumps, J fires); `@GAME@.cpp` reads it as the `InputState`
resource.
