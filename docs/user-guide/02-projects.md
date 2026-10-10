# 2. Projects

A project is one self-contained folder: identity, content, gameplay code and a thin host. This
chapter explains every file the generator writes and what the build makes from it.

## 2.1 The folder

```
projects/MyGame/
  MyGame.mceproj                 # identity: name, title, engine version, games, start scene
  CMakeLists.txt                 # one call: mce_add_project(my_game ...)
  README.md                      # the commands for this project
  CREDITS.md                     # third-party content and licences (the shipped folder carries it)
  .gitignore                     # cooked/ and assets.mce are build outputs
  assets/                        # what you edit; the editor and the cook open this directory
    scenes/main.mcscene          # the start scene (text, JSON-like)
    textures/block.png           # a source file ...
    textures/block.png.mceasset  # ... and its sidecar: GUID + importer settings
    textures/ground.png (+ .mceasset)
    fonts/ProggyClean.ttf (+ .mceasset)   # the title label's font (MIT); any TTF works the same way
  games/my_game/
    module/components.h          # your components (MCE_COMPONENT), data only
    module/my_game.h             # declares my_game::install
    module/my_game.cpp           # your systems and install()
    module/module.cpp            # MCE_GAME_MODULE entry point
    module/types.cpp             # includes every component header, for the cook
    host/main.cpp                # the executable: mce::player::run with this game
```

Rules that keep the layout working:

- **`assets/` is the only tree you edit.** `cooked/` and `assets.mce` are outputs of the cook and can
  be deleted at any time; a full re-cook reproduces them byte for byte.
- **Every asset has a GUID**, either inline (native text assets such as scenes) or in a `.mceasset`
  sidecar next to a source file (PNG, WAV, TTF, glTF). References between assets are GUIDs, so
  renaming or moving a file inside `assets/` does not break anything as long as the sidecar moves
  with it (the editor does that for you).
- **Paths are relative to `assets/`** everywhere a tool asks for one (`-scene scenes/main.mcscene`).
- **The game module is data plus systems.** Keep `components.h` free of engine code other than
  `mce/refl` and `mce/types.h`, so the cook can compile it without your gameplay.

## 2.2 The project file

`MyGame.mceproj`:

```json
{
  "format": "mce-project",
  "version": 1,
  "name": "MyGame",
  "title": "My Game",
  "engine_version": "0.0.1",
  "games": ["my_game"],
  "start_scene": "0d4f...-...",
  "plugins": []
}
```

`start_scene` is the GUID of the scene the host plays when no `-scene` is given. The build reads it
at configure time, so after changing it, reconfigure. `games` lists the game ids the project
builds (one today). `plugins` is reserved.

## 2.3 The CMake call

`CMakeLists.txt` is one call:

```cmake
mce_add_project(my_game
    TITLE "My Game"
    GAME_SOURCES games/my_game/module/my_game.cpp
    MODULE_ENTRY games/my_game/module/module.cpp
    TYPES games/my_game/module/types.cpp
    HOST games/my_game/host/main.cpp
    INCLUDE_DIRS games/my_game/module)
```

| Argument | Meaning |
|---|---|
| `TITLE` | the window title |
| `GAME_SOURCES` | your systems; add files here as the game grows |
| `MODULE_ENTRY` | the file with `MCE_GAME_MODULE(...)` |
| `TYPES` | a file that includes every header declaring an `MCE_COMPONENT`; compiled into the cook, the editor and the MCP server so they know your components |
| `HOST` | the thin executable |
| `INCLUDE_DIRS` | include paths for the sources above |

The engine's root `CMakeLists.txt` adds every `projects/<name>/` that has a `CMakeLists.txt`, so a
new project needs one reconfigure (`cmake --preset <preset>`) and nothing else.

You may add more after the call. `projects/Kindle/CMakeLists.txt` adds a ctest that links the game
sources statically, mounts the cooked pack and plays the level with scripted input; copy that
pattern when you want gameplay tests.

## 2.4 The targets

For a game id `my_game` the build defines:

| Target | What it builds |
|---|---|
| `my_game` | the host executable. At run time it looks for `assets.mce` and `libmy_game.so` beside itself (the shipped layout) and falls back to the build tree's copies |
| `my_game_game` | the game module `libmy_game.so` (`.so` on macOS too: a CMake `MODULE` library; only `libmce_engine` is a `.dylib` there). On Windows modules are not supported yet, so the game sources link into the host |
| `my_game_cook` | `mce_cook` with your components. The build runs it over a copy of `assets/` into `build/<preset>/projects/MyGame/project/assets.mce` |
| `my_game_editor` | `mce_editor` with your components and your `install()` |
| `my_game_mcp` | `mce_mcp` with your components and systems |
| `my_game_headless_300` | a ctest: the host headless for 300 ticks |
| `my_game_gpu`, `my_game_editor_gpu` | ctests that need a display (registered with `MCE_GPU_TESTS=ON`) |

Without a game module the stock `mce_editor`, `mce_cook` and `mce_mcp` know only the engine's
components: opening a scene that uses `Spin` fails with "unknown component type". Either use the
project's own executables above, or hand the stock tools the module, which is what the SDK does
(`mce_editor -module build/<preset>/projects/MyGame/libmy_game.so -project projects/MyGame`,
`mce_mcp --module ...`, `mce_cook --module ...`; on Windows, where modules are not built, only the
project's own executables work).

## 2.4a Building against the installed SDK

The same folder builds on its own against an installed engine (chapter 1, section 1.9): the
template's `CMakeLists.txt` calls `find_package(MCE)` when `mce_add_project` is not already defined,
so `mce build MyGame` (or `cmake -S MyGame -B MyGame/build -DCMAKE_PREFIX_PATH=<sdk>`) works
from anywhere. The targets are then `my_game`, `my_game_game` and `my_game_content` only; the
build runs the SDK's `mce_cook --module` for the pack, and `mce editor`, `mce mcp` and `mce cook`
run the SDK's tools with the module loaded. The outputs land in `MyGame/build/` (`my_game`,
`libmy_game.so`, `project/assets.mce`).

## 2.5 Where runtime files go

| What | Where |
|---|---|
| Save slots (`-save <slot>`) | `~/.local/share/MCE/<app>/saves/` on Linux and macOS (`$XDG_DATA_HOME` if set), `%LOCALAPPDATA%\MCE\<app>\saves\` on Windows; `-save_dir` overrides |
| Crash reports | `~/.local/share/MCE/<app>/crashes/` or `%LOCALAPPDATA%\MCE\<app>\crashes\`; `-crash_dir` overrides, `-crash_reports 0` disables |
| Editor dock layout | `mce_editor.ini` in the working directory (`-editor.ini` changes it, empty disables) |
| Editor autosaves | `<Project>/.mce/autosave/<path under assets/>.autosave` |

`<app>` is the `app_name` the host passes, which the template sets to the project name.

## 2.6 Several games, several projects

Each project is independent: its own assets, its own module, its own executables. Nothing is
shared through an engine-global asset directory, which is what makes the ship zip a plain copy of
one project's outputs. To reuse content between projects, copy the files with their sidecars; GUIDs
stay the same, so references inside copied scenes keep resolving.
