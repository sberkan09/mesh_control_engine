# modules — the mce_engine shared library, game modules, the module host and the player runtime

This page covers two directories. `src/module/` holds the game-module boundary: the loader
`mce_module_host`, the `mce_engine` umbrella shared library, the `mce_module_api` interface target
and the `mce_add_game_module()` CMake function (scripting.md §2, §3.1). `src/player/` holds the
player runtime `mce_player_runtime` (`mce::player::run`) and the `mce_player` executable, the L6
host for editor-authored scenes.

Dependencies, from the CMakeLists files:

- `mce_module_host` (static): public `mce_ecs`, private `mce_platform`; it also exposes the generated
  include directory that holds `mce/engine_version.h`.
- `mce_engine` (shared): every library in `MCE_ENGINE_LIBS` linked whole (`--whole-archive` on ELF,
  `-force_load` on macOS): `mce_core`, `mce_refl`, `mce_asset`, `mce_ecs`, `mce_physics2d`,
  `mce_platform`, `mce_input`, `mce_rhi`, the RHI backend, `mce_rhi_default`, `mce_render`,
  `mce_ui`, `mce_audio`, `mce_scene`, `mce_ecs_scene`, `mce_gameplay_kit`, `mce_module_host`. Its
  interface carries their compile requirements only (`$<COMPILE_ONLY:...>`). `mce_physics3d` is not
  in the list.
- `mce_player_runtime` (static): `mce_engine` where game modules are supported, otherwise the static
  libraries directly (`mce_scene`, `mce_gameplay_kit`, `mce_physics2d`, `mce_audio`, `mce_render`,
  `mce_platform`, `mce_ui`, `mce_module_host`, `mce_rhi_default`).

Linked by: `mce_player`, every project host made by `mce_add_project` (`cmake/MceProject.cmake`),
`mce_devhost`, `test_module`, and samples that load modules (`03_platformer`).

## Public surface

`src/module/include/mce/module.h`, the contract a game module implements:

- `ModuleInfo`: `name`, `engine_id`, `module_version` (the game's own save-format version) and
  `install(World&)` returning `ResultV`.
- `MCE_GAME_MODULE(name, version, install)`: defines the single exported symbol `mce_game_module`
  (`kModuleEntrySymbol`), stamped with `MCE_ENGINE_ID`.
- `EngineId` (u64). `MCE_ENGINE_ID` is generated at configure time by `cmake/EngineId.cmake`: the
  first 64 bits of SHA-256 over `git describe --always --dirty --tags`, build type, compiler id and
  version, and the ASan setting.

`src/module/include/mce/module_host.h`, the host side:

- `load_game_module(path, world)`: copy-and-load (maps `<stem>_loaded_<N><ext>`), checks the engine
  id, runs `install`. Errors: `ModuleNotFound`, `IoError`, `ModuleMissingEntry`,
  `EngineIdMismatch`, `AlreadyExists` (one module per World), `ModuleInstallFailed`.
- `unload_game_module(module)`: unmaps and deletes the copy; asserts the World is already destroyed.
- `LoadedModule`, `engine_id()`, `loaded_module_count()`.

`src/module/include/mce/gameplay.h`: the one include a game module is meant to use. It pulls in core,
the ECS and transforms, input ECS types, kit components, physics2d components and module, audio
components, and `mce/module.h`. Renderer, RHI, platform, asset internals and the cook library are
not part of it.

`src/module/src/engine_dll.cpp`: only gives `mce_engine` a translation unit of its own.

`src/player/include/mce/player/player.h`:

- `PlayerDesc`: app name, window title, pack and module file names (looked up beside the
  executable) with build-tree fallbacks, and `static_module` for platforms without modules.
- `run(argc, argv, desc)`: parses the command line into convars and runs headless or windowed.
  `src/player/main.cpp` is `return mce::player::run(argc, argv);`.

What `run` does (`src/player/src/player.cpp`): mounts the pack, picks the scene (`-scene` GUID or
source path, else the first scene), installs the `InputState` resource (`input::install`),
`physics2d`, the audio bridge, particles and sequences, installs the game (static module, or
`-module` / `PlayerDesc::module`), then `scene::load`. It draws the kit components, offers the Esc
pause overlay and the developer console (backtick or F1), pauses and mutes on focus loss, loads and
writes `-save` slots, and with `-headless 1 -ticks N` prints the World state hash.

Input for the game module (M9): windowed, the runtime samples the starter-kit map
(`input::default_map_chunk`, map "Gameplay": `Move` on WASD / arrows / left stick / d-pad, `Jump`
on Space / A, `Fire` on J / left mouse / X, `Pause` on Escape / Start, `Look` on the mouse) once per
fixed tick through `AppDesc::input_source` and publishes it before `World::tick`, so a module's
systems read `f.resource<input::InputState>()` (`move`, `jump_pressed`, `jump_held`,
`fire_pressed`, ...). Events go to the input system while the console is closed; key releases and
focus loss always do. Headless runs publish nothing, so the resource stays zeroed and `-headless`
hashes depend only on the scene and the tick count. A game cannot ship its own map through the
pack yet (see Known gaps); actions the default map lacks read as zero (`has_action` is false).

## Rules and invariants

- Hosts that load modules link `mce::mce_engine` and no static engine library. A second static copy
  would be a second engine instance (convar registry, log, refl registry, allocator state).
- The host installs the engine systems before loading; `install` registers only the game's own
  components, events, systems and resources, in an explicit order.
- Module rules (scripting.md §2.4, comment in `module.h`): no state outside the ECS (no mutable
  statics, no `MCE_CVAR`), no work in static initializers, main thread only.
- Teardown order: the World holds code pointers into the module, so destroy the World before
  `unload_game_module`. After a failed install the library stays mapped for the process lifetime and
  the World must be discarded.
- Game modules are built with hidden visibility; only `mce_game_module` is exported. Modules and the
  umbrella link with `--no-undefined` except under ASan (and, for modules, on macOS).
- Build rpaths are relative (`$ORIGIN`, `@loader_path` on macOS) so a shipped folder never loads
  from the build tree (`scripts/ship.py`).
- Windows: `MCE_GAME_MODULES_SUPPORTED` is OFF and `mce_add_game_module` is a no-op until the
  export ledger exists. Hosts link the game in and use `PlayerDesc::static_module`.
- UI gameplay wall: module sources under `games/*/module/` may not include `mce/ui/`.

## How it is tested

- `test_module` (`tests/module/`, built only when modules are supported): a host linked against
  `mce_engine` loads synthetic modules built from `test_game_module.cpp` in four variants (good, no
  entry, install fail, stale engine id). Cases: "module: load, install, tick, unload", "module:
  reload at the play boundary reproduces the state hash", "two live copies of one module map
  distinct images", "one module per world", "failures are diagnosable errors, never crashes",
  "install failure is reported and keeps the image mapped". It also checks that host and module see
  one convar registry and agree on component ids.
- `devhost_module_300`: `mce_devhost -module <modtest_good> -ticks 300` (shares a resource lock with
  `test_module`). `devhost_headless_10k` runs the app loop with no module.
- `abi_spike`: the M1 throwaway ABI spike in `tests/abi_spike/`.
- `ui_no_imgui_symbols`: no `ImGui` symbol in `libmce_engine` or `mce_player` (Linux).
- Player: `scene_demo_player_600` (headless state hash), `player_crash_report` (`-crash_test 1`
  then symbolication; Unix, not under ASan), and GPU runs `scene_demo_player_gpu` and
  `scene_demo_player_console_gpu`.
- Project template: `template_probe_headless_300`, `template_probe_gpu`, `template_probe_editor_gpu`.
- Projects: every project gets the same three (`kindle_headless_300`, `kindle_gpu`,
  `kindle_editor_gpu` for `projects/Kindle`); a project may add its own tests in its
  `CMakeLists.txt` after `mce_add_project` (Kindle's `kindle_level` links the game sources
  statically with `mce_add_test`, mounts the project's cooked pack and plays the scene with scripted
  `InputState`s).

None of the non-GPU tests above need a display.

## Design records

- [Scripting architecture](../architecture/scripting.md): §2.1 module contract, §2.2 host loader,
  §2.3 `mce/gameplay.h`, §2.4 module rules, §3.1 one engine DLL plus one game DLL, §6 tests.
- [ADR-0021 The M3 game-module loader and mce_engine](../adr/0021-game-module-loader-m3.md)
- [ADR-0024 The runtime scene path](../adr/0024-runtime-scene-path.md) decision 6 (`mce_player`).
- [ADR-0029 Play-in-editor](../adr/0029-play-in-editor.md) §2 hot reload of a game module.
- [ADR-0018 Portable build](../adr/0018-portable-build.md).
- Milestones: [M8 project template](../milestones/M8-project-template.md),
  [M8 packaging v2](../milestones/M8-packaging.md), [M-MAC](../milestones/M-MAC.md) (`.dylib`
  modules), [M6 gameplay](../milestones/M6-gameplay.md) (focus loss and save on quit).

## Known gaps

- `mce/gameplay.h` is a convention, not a wall: a module target still sees every engine include
  directory (ADR-0021 "Not in this change", comment in `gameplay.h`).
- No Windows game modules: the `mce_engine.dll` export ledger (MCE_API decorations and explicit
  instantiations, finding F33) is not built.
- Projects build inside the engine checkout; an installed engine package (`find_package(MCE)`)
  does not exist (M8 project template).
- The stock `mce_editor` does not load game modules; each project gets its own `<game>_editor`
  executable (M8 project template, ADR-0029 §1.7), and its own MCP server `<game>_mcp`.
- The player runtime's input map is the engine's default map. There is no input-map asset a project
  can cook into its pack and no `PlayerDesc` field for one, so a game needing other actions (Ember
  Step's `Dash`) falls back to the default ones (Kindle dashes on `Fire`). Rebinding persistence
  (`InputSystem::save_bindings`) is not wired into the runtime either.
- Crash reporting gaps from packaging v2: the Windows path has not been run, Windows `abort()`
  leaves no report, POSIX unwinds only the crashing thread, and reports are not uploaded.
