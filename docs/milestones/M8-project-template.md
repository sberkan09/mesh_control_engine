# M8 — Project template + new-project generator (milestone record)

**Scope:** ROADMAP M8, "project template + one-command new-project generator (finding F17)". The requirement is a directory scaffold with `<project>.mceproj`, `assets/`, `module/` and `host/` and a CMake skeleton (ARCHITECTURE CF17, assets.md §4.4), plus a working empty game module, such that `new_project <name>` produces a project that compiles, cooks and ships. The M9 gate precondition is that UC-D is built on this template.

**Measured on:** the Linux CI container, Debug build.

## What the generator writes

`python3 scripts/new_project.py MyGame` (or `scripts/new_project.sh` / `scripts\new_project.bat`) stamps `templates/project` into `projects/MyGame`. The game id is `my_game`. GUIDs are fresh for each project, or reproducible with `--seed`.

| Path | What |
|---|---|
| `MyGame.mceproj` | project identity: name, title, engine version, games, start scene, plugins |
| `CMakeLists.txt` | one `mce_add_project(my_game ...)` call (`cmake/MceProject.cmake`) |
| `assets/scenes/main.mcscene` | a camera, a ground, a falling crate and two entities that use the game's own components (`Spin`, `Bob`) |
| `assets/textures/*.png` (+ `.mceasset`) | two generated 16×16 tiles |
| `games/my_game/module/` | `components.h` (data only, `MCE_COMPONENT`), `my_game.{h,cpp}` (systems + `install`), `module.cpp` (`MCE_GAME_MODULE`), `types.cpp` (the types for the cook) |
| `games/my_game/host/main.cpp` | the thin host: `mce::player::run` with the module and pack |
| `README.md`, `.gitignore` | how to build, play and ship; `cooked/` and `assets.mce` stay out of git |

## What `mce_add_project` builds

| Target | Role | How |
|---|---|---|
| `<game>_game` | the game module `lib<game>.so` | `mce_add_game_module`; on Windows, where there are no modules yet, the sources link into the host and `PlayerDesc::static_module` installs them |
| `<game>` | the host | `mce_player_runtime` (new: `mce/player/player.h`, the whole of `mce_player` as a function). It loads the module beside it, else from the build tree, and the same for the pack. `mce_player` is now three lines over it and links the `mce_engine` umbrella, so `-module` works there too |
| `<game>_cook` | the project's cook | `mce_cook` plus `types.cpp`, so scenes that use the game's components cook. This closes the gap noted in `tools/cook/engine_components.cpp` |
| `<game>_content` | `build/<preset>/projects/<name>/project/assets.mce` | `<game>_cook --pack` over a copy of `assets/` |
| `<game>_editor` | the project's editor | `mce_editor`'s main plus the game sources. Its components show in the inspector and the in-editor cook, and play-in-editor runs `install()` through the new `editor::set_play_game_install` |
| tests | `<game>_headless_300`, `<game>_gpu`, `<game>_editor_gpu` | the host headless; windowed with the GL debug layer; the editor opening, cooking and playing the scene |

`scripts/ship.py --project projects/<name>` builds the targets and stages the host, `lib<game>.so`, `libmce_engine.so` and `assets.mce`. It checks that the folder is self-contained (`ldd`), writes `README.txt` and `VERSION.txt`, zips the result and optionally smoke-runs it from the staged copy.

## Evidence

| Gate | Evidence |
|---|---|
| The generator produces a project that compiles, cooks and runs, in every CI preset | **Template probe:** at configure time the root CMake runs `new_project.py TemplateProbe --seed 1` into `build/<preset>/template_probe/` and adds it like any project. Every preset therefore builds the host, module, cook, pack and editor. `template_probe_headless_300` passes (`300 ticks, 5 entities, state_hash 858909c294ed1037`), and `template_probe_gpu` and `template_probe_editor_gpu` pass with 0 GL errors. A template change that stops compiling, cooking or running fails CI. |
| ...and ships through the one-command pipeline | `new_project.py ShipCheck` → `ship.py --project projects/ShipCheck --smoke --smoke-headless`: `Ship-Check-0.0.1+<commit>-linux-x64.zip` (2.4 MiB: host, `libship_check.so`, `libmce_engine.so`, `assets.mce`). The staged copy ran 300 ticks headless, and windowed with 0 GL errors, loading the module from its own folder. |
| The game's components are authored in scenes and run | The template scene uses `Spin` and `Bob`. The project cook accepts them; plain `mce_cook` and `mce_editor` reject them with `unknown component type 'Spin'`, which is the gap closed here. In the project editor, play-in-editor rotates the spinner (screenshot). |
| Gameplay never includes `mce/ui` | `check_walls.py` scans `projects/` and `templates/` too; the UI gameplay wall covers `games/*/module/`. |

## Gaps

- **Projects build inside the engine checkout** (`projects/<name>`). An out-of-tree project needs an installed engine package (`find_package(MCE)`), which does not exist yet.
- **The project editor is a per-project executable** (`<game>_editor`), not the stock `mce_editor` loading the module. Loading a module into the editor needs the editor itself on the `mce_engine` umbrella (one refl registry). That is a bigger move, and nothing in M9 needs it.
- **Windows** builds the static path (`PlayerDesc::static_module`), as the samples do until the export ledger. It has not been run on Windows in this container.

## Screenshots

`/mnt/project-files/m8-project/template-game.png` (the host), `/mnt/project-files/m8-project/template-editor-play.png` (the project editor in play mode)
