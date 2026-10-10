# 17. Troubleshooting

Symptoms, what they mean, what to do.

## Build and run

| Symptom | Cause | Fix |
|---|---|---|
| CMake does not see `projects/MyGame` | the project was created after the last configure | `cmake --preset <preset>` again |
| `EngineIdMismatch` when the game starts | the module was built by a different engine build (commit, build type, compiler or sanitizer) | rebuild both targets from the same tree: `cmake --build --preset <preset> --target my_game` |
| `ModuleNotFound` | no `lib<game>.so` beside the host and no build-tree fallback | build `my_game_game`, or pass `-module <path>` |
| "module install failed" | your `install()` returned an error | read the log line; a common one is a missing host resource (install order, section 6.8) |
| Window never appears, "No available video device" | no display (a container, SSH) | run headless (`-headless 1`), or under `xvfb-run` with Mesa (`LIBGL_ALWAYS_SOFTWARE=1`) |
| Two audio-device errors at start in a container | no sound card; the engine falls back to silence | harmless; `-audio.enabled 0` silences the log |
| `-frames` has no effect | the run is headless | use `-ticks N` |
| `-capture` fails with "cannot write" | the folder is not writable; a relative path is taken from the working directory, not the executable's folder | check the working directory, or pass an absolute path |

## Content and the cook

| Symptom | Cause | Fix |
|---|---|---|
| `unknown component type 'Spin'` | the stock cook, editor or MCP server runs without the game module, or the component is not in `types.cpp` | use `my_game_cook`, `my_game_editor`, `my_game_mcp`, or the stock tool with `-module` / `--module <lib<game>.so>` (`mce editor`, `mce mcp`, `mce cook`); include the header in `types.cpp`; rebuild |
| A PNG under `assets/` is a cook error | no sidecar | `my_game_cook projects/MyGame --adopt` (`mce cook MyGame --adopt`), or MCP `cook_run {"adopt": true}` |
| `EngineIdMismatch` when a tool or the host loads the module | the module was built against another engine build (another SDK, a reconfigured checkout) | rebuild the module (`mce build`, or the `<game>_game` target) |
| `find_package(MCE)` fails | the SDK prefix is not on `CMAKE_PREFIX_PATH` | `mce build` passes it; by hand, `-DCMAKE_PREFIX_PATH=<sdk>` or `-DMCE_DIR=<sdk>/lib/cmake/MCE` |
| `file:line:col: error: ... "guid:..." not found` | a reference to a GUID no asset has | `asset_list` or the Assets panel for the right GUID; check the sidecar moved with the file |
| Magenta checker instead of a texture | the texture failed to load or the reference is stale | the log names the asset; re-cook; check `srgb`/format; for a stale handle after a hot reload, references re-stamp on next use |
| A normal map or LUT looks wrong | imported as sRGB | `"srgb": false` in the sidecar |
| Scene loads but an entity keeps C++ defaults | the field name in the file does not match (or a `Transient` field was written) | `refl_describe_component`; the cook warns about unknown keys |
| "re-cook" in the log at load, `InvalidArgument` | a component's layout changed since the pack was cooked | cook again (the build does it when you build the host) |
| The game shows old content after MCP edits | MCP's `cook_run` wrote `cooked/`, not the pack the game loads | `cook_run {"pack": true}` or rebuild `my_game` |
| Deleting `cooked/` or `assets.mce` | they are outputs | cook again; results are byte-identical |

## Editor

| Symptom | Cause | Fix |
|---|---|---|
| "Open a project to edit a scene" | started without `-project` | pass `-project projects/MyGame` |
| Play-in-editor: the character does not move | play mode feeds no input | drive it from the real game (`my_game`, or Ctrl+F5) |
| Inspector is greyed out | play mode | Stop (F5) |
| HUD text is off-screen in play mode | the play view is the viewport's size, the label was authored for the window | ignore in the editor, or enlarge the viewport |
| "Recover unsaved changes" on open | the previous session ended without saving | Recover or Discard |
| Hot reload did not pick up a change | the watcher is off, or the file is outside `assets/` | File > Hot reload now; `-editor.hot_reload_interval 0.5` |

## Physics

| Symptom | Cause | Fix |
|---|---|---|
| A body ignores `LocalTransform` writes during play | physics owns bodied poses | `World2D::teleport` or velocity |
| `has_body` is false on the first tick | bodies are created by the first physics step | guard first-tick logic, as Kindle's systems do |
| A sensor fires for everything | its `mask` allows every layer | set `mask` to the player's layer bit |
| A character falls through a one-way platform | moving upward or sideways through it is intended | `one_way` blocks only downward sweeps |
| The crate does not collide | degenerate shape (zero size) or `density 0` on a dynamic body | fix the shape; dynamic bodies need density |
| Jitter on a moving platform | the platform is moved by teleport or transform | set a kinematic velocity that lands on the path |

## Determinism

| Symptom | Cause | Fix |
|---|---|---|
| The headless hash changes between identical runs | a system reads wall time, uses an unordered container, or touches a static | keep state in the World; use seeds on components |
| The hash changes after adding a field | component bytes are hashed | expected; compare runs built from the same code |
| Editor play hash differs from the player | the editor fed no input and the scene is unsaved | save, then compare `run_headless` or `my_game -headless 1` with the same tick count |

## MCP

| Symptom | Cause | Fix |
|---|---|---|
| Edits land on the wrong entities | ids restarted after `scene_open` | `scene_list_entities`, then `scene_undo` the batch |
| `run_headless` refuses | unsaved changes | `scene_save` |
| The server does not know a new component | its table comes from the build (compiled in, or the loaded module) | rebuild (`my_game_mcp`, or the module for `--module`) and restart it |
| `console_exec time_scale 2` refused | cheat convar | start the server with `--allow-cheats` |

## Shipped builds

| Symptom | Cause | Fix |
|---|---|---|
| "cheats are locked in this build" | a `Cheat` convar on the console or command line | intended; development builds allow it |
| A crash report has only addresses | the binaries are stripped | `scripts/symbolicate.py <report> --symbols <unzipped symbols>` |
| `ship.py --smoke` fails in a container | the smoke run needs a display | `--smoke-headless` |
