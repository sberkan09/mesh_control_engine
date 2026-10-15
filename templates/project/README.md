# @PROJECT@

Made with the MCE project template (`scripts/new_project.py`). Layout (assets.md §4.4):

| Path | What |
|---|---|
| `@PROJECT@.mceproj` | project identity: name, engine version, games, start scene |
| `assets/` | the content you edit; the editor opens this directory. `cooked/` and `assets.mce` are build outputs |
| `games/@GAME@/module/` | the game module: `components.h` components, `@GAME@.cpp` systems + `install`, `module.cpp` entry, `types.cpp` for the cook |
| `games/@GAME@/host/` | the thin host executable (the engine's player runtime + this game) |
| `assets/ui/themes/` | starter UI themes, `pixel`, `clean` and `story` (user guide 12, "Themes"): name one as a `.mcui` document's `"theme"`, and delete the ones (and their fonts) the game does not use |
| `CREDITS.md` | third-party content (the template's fonts: ProggyClean, MIT, and the themes' OFL fonts); `mce ship` / `ship.py` copy it into the shipped folder |

With the installed SDK (`mce` on your PATH; chapter 1 of the user guide):

    mce build @PROJECT@                 # find_package(MCE) + build into @PROJECT@/build
    mce play @PROJECT@                  # play (Esc pauses, ` opens the console)
    mce editor @PROJECT@                # the installed editor with lib@GAME@ loaded
    mce mcp @PROJECT@                   # the MCP server for an AI agent (user guide chapter 14)
    mce ship @PROJECT@ --version 1.0    # the ship zip in @PROJECT@/dist/

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
map is live (WASD / arrows move, Space jumps, J fires), but the template's `@GAME@.cpp` does not
read it yet: a system that wants it reads the `InputState` resource (user guide chapter 11).
