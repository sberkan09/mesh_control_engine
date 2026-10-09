# @PROJECT@

Made with the MCE project template (`scripts/new_project.py`). Layout (assets.md §4.4):

| Path | What |
|---|---|
| `@PROJECT@.mceproj` | project identity: name, engine version, games, start scene |
| `assets/` | the content you edit; the editor opens this directory. `cooked/` and `assets.mce` are build outputs |
| `games/@GAME@/module/` | the game module: `@GAME@.h` components, `@GAME@.cpp` systems + `install`, `module.cpp` entry, `types.cpp` for the cook |
| `games/@GAME@/host/` | the thin host executable (the engine's player runtime + this game) |

From the engine checkout:

    cmake --preset linux-gcc-debug                      # picks up projects/@PROJECT@
    cmake --build --preset linux-gcc-debug --target @GAME@
    build/linux-gcc-debug/projects/@PROJECT@/@GAME@     # play (Esc pauses, ` opens the console)
    python3 scripts/ship.py --project projects/@PROJECT@ # the ship zip in dist/

Add components to `@GAME@.h` (MCE_COMPONENT), systems to `@GAME@.cpp` (`install`), and author
them in scenes with the editor (`mce_editor projects/@PROJECT@`).
