# MCP blind test: Lamplighter

*2026-10-09.* The third round of making a game through the editor's MCP server, after Prism Break
(`mcp-dogfood.md`) and Undertow (`mcp-dogfood-undertow.md`). The difference this time: the
developer had **never seen the engine source**. The question was how the engine holds up when it is
used the way an installed engine is used (the user has headers, docs and binaries, not `src/`), and
what is missing for that model.

Result: a fresh session made a small, complete, playable game (**Lamplighter**) in about 11 minutes
of agent time (54 tool calls, 102 MCP calls), with the gameplay module compiling on the first try
from the headers and `docs/engine/`. Most of the friction is SDK packaging (dead doc links, no
sample, no `--help`), the headless input gap the earlier rounds already reported, and one new MCP
trap: entity ids change when a scene is reopened.

Screenshots, the friction log, the transcript and the project are in the project files under
`mcp-blind-test/`.

## The engine today is used from the checkout

There is no installed form of the engine yet. Game development assumes the repository:

- `scripts/new_project.py` stamps `templates/project` into `projects/<Name>/`, and the root
  `CMakeLists.txt` adds every `projects/<name>/` to the engine's own build.
- `mce_add_project` (`cmake/MceProject.cmake`) builds, per project, the host, the game module, a
  cook, an editor (`<game>_editor`) and an MCP server (`<game>_mcp`). The game's reflected
  components are compiled into the editor, the cook and the MCP server, so these tools are
  rebuilt from engine source for every project.
- `scripts/ship.py --project` packages the game for players (host + module + `assets.mce`). It
  must run inside the checkout (`ship: --project must be inside the engine checkout`).

What the "install it like a normal engine" model needs and does not have:

| Missing | Today | Note |
|---|---|---|
| An installable engine package | No `install()` rules, no `find_package(MCE)` config | Already listed as a gap in `docs/milestones/M8-project-template.md` |
| A stock editor, cook and MCP server that load the game | Per-project executables with the game linked in | Needs the editor on the `mce_engine` umbrella (one refl registry), also an M8 gap |
| A public header set | Headers live in `src/**/include/`, next to the implementation | They copied out cleanly into one `include/mce/` tree for this test |
| A project CLI | `cmake --preset`, `scripts/*.py` from the checkout | The test used a small `mce` wrapper (below) |
| SDK docs | `docs/engine/` links ADRs, milestones and samples | See finding 3 |

Gameplay is C++, so an SDK user still needs a compiler and CMake (as with Unreal); the SDK is the
headers, prebuilt libraries, the tools and a CMake package.

## How the test was set up

Building a real SDK was out of scope, so the installed engine was simulated in the cloud container:

- `sdk/include/mce/`: every `src/**/include/` tree merged. `sdk/docs/`: `docs/engine/` and
  `docs/architecture/` only (no ADRs, milestones, research or roadmap, as a shipped SDK would not
  carry them). `sdk/README.md`: what is installed and the command line.
- `sdk/bin/mce`: `build`, `headless`, `play` (Xvfb), `editor`, `ship` and `mcp`. Each runs the
  engine build tree's binaries for the project; `build` runs `cmake --build` on the checkout and
  rewrites engine source paths in its output to `sdk/include/` (or `<sdk>`) so no source path leaks.
- The project, made by `scripts/new_project.py MyGame`, was moved out of the checkout to its own
  directory (symlinked back for the build).
- MCP: a fully autonomous nested Claude Code session was refused by the container's safety checks,
  so the developer was a fresh-context subagent, and its MCP connection was `mce mcp-start`,
  `mce mcp-tools` and `mce mcp-call NAME JSON`: a bridge that keeps one `my_game_mcp` process (open
  scene, undo stack) alive between calls. Tool discovery is the same information a connected client
  gets (`tools/list`), but a native MCP connection was not tested.
- Blindness was by instruction (no reads under the checkout, `/root`, `/mnt`, `/tmp`, `/proc`, no
  searches of `/`), then audited: none of the 54 tool calls in the transcript names a forbidden path
  or `nsenter`/`unshare`.

The task given: a small complete game (start state, goal, win, lose, keyboard), scenes through
MCP, gameplay in the module, a friction log, screenshots, a passing build and headless run.

## The game

`Lamplighter`, top-down 2D: light the 6 street lamps of a town square before dawn (45 s) by walking
up to each; three ghosts patrol paths and a fourth hunts you; a ghost's touch or dawn loses. Title
state, HUD (lamps lit, countdown), restart on Space or J. Six components and six systems
(`components.h` +90 lines, `my_game.cpp` +272), two scenes authored entirely through MCP (`main` and
`autoplay`, the same level with a scripted pilot for headless checks), art from a stdlib-only Python
PNG writer, a system font adopted as an asset.

## What worked

- **The module compiled on the first try from the headers and docs alone** (`mce/gameplay.h`,
  `Frame::query<...>().each`, `add_system(..., after)`). `docs/engine/kit.md`'s "Building a 2D game
  on the kit" section (HUD via `TextLabel` + `Parallax{0,0}`, the view size at 48 px/m) answered most
  layout questions.
- The MCP surface was learnable cold: `entity_create` with a components object, `component_set`
  with property paths, `"$<id>"` entity refs, one undo step per call, errors that name the fix
  ("no entity with id 24 in the scene (scene_list_entities lists them)").
- `cook_run {"adopt": true}` turned new PNG and TTF files into assets in one call.
- `run_headless` + `inspect` on a game state component verified win, both losses and the restart
  loop without a window, as in the earlier rounds.
- Restarting the MCP server after a build that added components worked as the SDK README said.

## Findings

Items marked (R1 n) / (R2 n) confirm an open item of Prism Break / Undertow.

| # | What | Cost here | Suggested fix |
|---|---|---|---|
| 1 | **No input in headless runs** (R1 3, R2 7). A keyboard game sits on its title screen headless; `run_headless` says "Recorded input is not wired yet". The tester added `Game.autoplay`, an AI pilot and a second scene; it is the third game to ship a test-only mechanic. | The largest time cost. | `run_headless` `input`: per-tick action states. |
| 2 | **Entity ids are not stable across `scene_open`.** In the session that built the scene, ids ran 6..28 (they keep counting past `scene_clear`: verified, a create after a clear got 25 when the previous was 24). After reopening the same file they were 1..23. A tuning batch written against the old ids tinted the wrong entities and was saved; `scene_undo` ×7 recovered it. `docs/architecture/editor-mcp.md` calls them "persistent `EntityId`". | One bad save. | Address entities by name in `component_set` / `scene_get_entity`, or keep ids stable per file; at least fix the doc. **New.** |
| 3 | **SDK docs point at things an SDK does not ship.** `docs/engine/README.md` links `docs/adr/` and `docs/milestones/`; every page links ADRs; `kit.md` points at Kindle's `components.h` and `samples/03_platformer/game/`; pages cite `src/...` files. The project template's README gives checkout commands (`cmake --preset`, `scripts/ship.py`). | Most of the reading time. | An SDK doc set without internal links, and one sample game shipped with the SDK. **New** (only visible without the repo). |
| 4 | **Player flags are undiscoverable.** No `-help`: `-help` warns "unknown convar" and runs the game. `-frames` is ignored headless (it ran 600 ticks); `-ticks N` is mentioned only in `modules.md`. | Two wasted runs. | A `-help` that lists convars with their descriptions (they exist in `MCE_CVAR`), and a warning when a windowed-only convar is set headless. **New.** |
| 5 | **The MCP cook does not write the pack**, so `play` after MCP edits showed the old template scene until `mce build`. | One failed capture. | Say so in the `cook_run`/`run_headless` docs, or have the project's play/build always recook. |
| 6 | **`-capture` with a relative path fails** ("cannot write 'shots/title.ppm'") even from the project directory. | One failed capture. | Resolve against the working directory, or say which base it uses. **New.** |
| 7 | **No font in the template**, and the kit docs do not say how `TextLabel` gets one (found in `assets.md` that `.ttf` is a cook source; copied a system font). | Small. | A default font in the template (R1 8). |
| 8 | **No tiled sprites** and no MCP tilemap painting: the floor became one 384×192 PNG. | Small. | A `Sprite` tiling mode or a tilemap tool. |
| 9 | **`scene_save` save-as keeps the scene `name`**, and overwriting an existing file with `force` gives it a new GUID, which would silently break anything that referenced it. | None here. | Keep the target file's GUID on overwrite; take a name argument. **New.** |
| 10 | **`run_headless` plays only the open scene**, and there is no delete tool, so trying a variant meant save-as, run, reopen, delete by hand. | Small. | `run_headless` `scene` argument; an asset delete tool. |
| 11 | **Screenshots are wall-clock paced:** `play -frames 1800` ran 660 ticks under Xvfb, so catching a moment took trials (R1 4 / R2 6: no `render_frame`). | A few runs. | `render_frame`, or `-capture_tick N`. |
| 12 | **Ship smoke needs a display:** `ship --smoke` runs the packed game windowed and fails headless containers ("No available video device"); the zip is written. Every windowed run also logs two audio-device errors. | None. | `--smoke-headless` (exists in `ship.py`) as the SDK default here. |

The tester's top five by time: 1, 2, 3 + 4, 5 + 6, 7 + 8.

## What this says about the installed model

The engine itself was not the obstacle: a newcomer with only headers and docs wrote working
systems on the first compile and authored scenes through MCP without help. What a real SDK needs
first is packaging work rather than engine work: the CMake package and install rules, an editor and
MCP server that load a game module instead of being rebuilt per project, an SDK doc set with a
sample game, and a CLI with `--help`. On the MCP side, findings 1 and 2 are the ones worth fixing
before more games are made this way.
