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
them in scenes with the project's editor, which knows the game's components (the stock `mce_editor`
does not):

    cmake --build --preset linux-gcc-debug --target @GAME@_editor
    build/linux-gcc-debug/projects/@PROJECT@/@GAME@_editor -project projects/@PROJECT@

An MCP client (an AI agent) authors the same scenes through the project's MCP server, `@GAME@_mcp
--project projects/@PROJECT@` (docs/engine/editor.md). In the windowed game the starter-kit input
map is live (WASD / arrows move, Space jumps, J fires); `@GAME@.cpp` reads it as the `InputState`
resource.
