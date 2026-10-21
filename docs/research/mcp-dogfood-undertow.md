# MCP dogfood 2: Undertow

*2026-10-09.* The second round of making a game through the editor's MCP server, after Prism Break
(`mcp-dogfood.md`). The same AI developer (Claude, a Linux cloud container, no GPU), the same rules:
`scripts/new_project.py`, gameplay in the project's game module, every scene edit through the
project's MCP server (`undertow_mcp`). The goal this time was a game that leans on much more of the
engine (lit 2D, physics joints, particles, audio buses, sequences, post effects, a HUD), so that the
MCP surface is tested where Prism Break's 65 entities did not reach it. This page records what
worked, what was fixed, and what is still open, with the Prism Break items it confirms.

The game: `projects/Undertow/`, a cave-diving salvage drone. Four zones of flooded cave carved from
lit rock (a cobble normal map under a headlamp and coloured lights), five reactor cores to find and
tow back to the dock on a physics rope, crushers on paths, drifting mines, lasers behind timed
switches, currents that push the drone and the core it is towing, a hull that impacts wear down and
checkpoints repair, energy for boosting. Particles for thrust, explosions, sparks, current flow and
bubbles; two audio buses (music loop, SFX) with the engine hum's gain driven by the game; a delivery
fanfare and a win zoom as code-defined sequences calling back into the game; vignette, a teal LUT and
chromatic aberration on impacts; a four-label HUD. `scenes/attract.mcscene` is the same level with
the drone on an `Autopilot` that tows the first core home: the headless check and the determinism test.

## How it was made

| Step | Tool | Hand-written? |
|---|---|---|
| Project skeleton | `python3 scripts/new_project.py Undertow --title "Undertow"` | no |
| Components and systems | `games/undertow/module/` (19 components, 1 007 lines of C++) | yes, by design |
| Art (21 PNGs incl. a tileable normal map and a 256×16 LUT) | `tools/gen_art.py` (numpy) | yes: no art tool in the loop |
| Sounds (11 WAVs incl. a 16 s music loop) | `tools/gen_sfx.py` (numpy synthesis) | yes |
| Font | ProggyClean copied from Kindle | yes (open item 8 of round 1) |
| Sidecars for 33 new files | `cook_run` with `adopt` | no |
| Import settings of the three data textures (normal maps, LUT: `srgb: false`) | `asset_set_import_settings` (new, below); before it, the script patched the sidecars on disk | no (was: yes) |
| 11 particle presets (`.mcpart`) | written by the script as JSON files | yes (gap 1 below) |
| Two scenes, 724 entities each | `tools/author_level_mcp.py`: 2 800 MCP tool calls, 3 s (4 248 before `scene_clear`) | no `.mcscene` was hand-edited |
| Start scene | `Undertow.mceproj` `start_scene` edited by hand | yes (open item 2 of round 1) |
| Verification | `run_headless` with `inspect` on `RunStats` (a ReadOnly/Transient component the game mirrors its state into), ctest `undertow_level` (9 cases), Xvfb captures with `undertow -frames N -capture` | no |
| Ship | `python3 scripts/ship.py --project projects/Undertow --smoke --smoke-headless` | no |

The authoring script is the designer's document: rooms are rectangles carved from a 100×56 grid, the
hazards are lists of positions, and the whole level (rock colliders, the lit boundary tiles, the
interior fills, the crystals, the pods, the hazards, the particle flows) is generated from them. It
runs in about three seconds and prints an ASCII map at the end. Rerunning it rebuilds both scenes in
place (same GUIDs, since the `.mceproj` names them).

## What worked

- Everything Prism Break found good held at ten times the entity count: `entity_duplicate` +
  `component_set` with a property path built 1 266 copies; errors stay precise; one undo step per
  call means `scene_undo` after an `entity_create` with six components is one step.
- `run_headless` + `inspect` on a game-owned stats component is a complete verification channel: the
  script reads `cores`, `deaths`, `hull`, `tethered` after 2 400 ticks and knows whether the autopilot
  towed a core home. The same hash comes out of `undertow_mcp` and `undertow_level`'s harness.
- The engine surface the game asked for was all there and composed without surprises: a
  `DistanceJoint` rope between two dynamic bodies that a current pushes around, a sensor dock that
  parks the pod and disables its body, kinematic crushers on a path colliding with the drone,
  `ParticleSystem::burst` for explosions, `SequenceRegistry` sequences with `register_call` into the
  game, `Post2DComponent` pulsed from the camera system, `Light2D` on a child of the drone.
- `cook_run` diagnostics with file/line made the two mistakes of the round (a `.mcpart` guid with a
  `guid:` prefix; a scene referencing a deleted texture) one-line fixes.
- `scripts/ship.py --smoke --smoke-headless` just worked on a project with two scenes, a font, 11
  sounds and 11 presets (5 MiB zip, smoke run 300 ticks).

## Fixed in this round

Both are MCP tools in `src/editor/mcp/src/tools.cpp` (PR #73):

1. **No bulk delete.** Re-authoring a scene a `.mceproj` names (so `scene_new` is wrong: it makes a
   fresh GUID, round 1 item 2) meant `scene_open` then one `entity_delete` per entity: 724 calls per
   scene, 1 448 of the script's 4 248 calls, a third of its time. `scene_clear` deletes every entity
   of the open scene as one undo step.
2. **Nothing sets import settings, and `adopt` writes none.** `cook_run` with `adopt` writes
   `"settings": {}`, so a normal map and the colour LUT were cooked as sRGB (the texture importer's
   default) and lit 2D came out flat: the normals were gamma-bent, the LUT remapped the wrong values.
   It took a screenshot to notice. `asset_set_import_settings` (by path or `guid:`) rewrites the
   sidecar's `settings` object, keeps its other members, and rescans; the next cook validates the keys
   as before and recooks (the settings hash changed).

## Friction, still open

Items marked (R1 n) confirm an open item of round 1; the rest are new.

| # | What | Cost here | Suggested fix |
|---|---|---|---|
| 1 | **No tool authors a non-scene native asset.** The 11 particle presets (`.mcpart`) are JSON the script writes to disk with a hand-made UUID; the same holds for materials, sequences, UI layouts. The only MCP write outside the scene is now the sidecar's settings. | 11 files, one wrong `guid:` prefix found by the cook. | `asset_write` (path + JSON) that validates through the importer and assigns the GUID, or a per-type `preset_create`. |
| 2 | **Nothing warns that a lit sprite's normal map is sRGB.** Fix 2 makes it settable, but the silent default stays: `Sprite.normal_map` on a texture cooked with `Srgb` is a flat picture, and no stage (cook, editor, `run_headless`) says so. | A screenshot round. | At cook: a scene referencing a texture in a normal-map slot (or a `Post2DComponent.lut`) whose chunk is sRGB is a warning. Or: `adopt` guesses `srgb: false` for `*_n.png` / `*_lut.png`. |
| 3 | **Effective values cannot be read.** `scene_get_entity` returns only the fields the document authored (a `Drone` left at its defaults is `{}`) and `refl_describe_component` carries no defaults, so a client cannot know what `thrust` is without `run_headless` + `inspect`. | The script carries the defaults as constants. | `refl_describe_component` with each field's default; `scene_get_entity` with `"effective": true`. |
| 4 | **Enum-like `u8` fields.** `AudioSource.bus` is a plain `u8` (1 = music, 2 = sfx); the layer index and mask of `RigidBody2D` are numbers. The schema says "integer" and the client has to know the engine's table. | Read the audio docs. | Reflect well-known ids as enums (`AudioBus`), or a `refl` annotation carrying named values. |
| 5 | **A stale scene blocks the whole pack.** (R1 1, from the other side.) A scene that references a deleted asset makes `cook_run` fail for the project; the script tolerates scene-only errors before it re-authors and cooks once after both scenes, which is wrong in the general case. | A script workaround. | A cook option to skip failing scenes with a warning, or `cook_run` `{"only": ["scenes/main.mcscene"]}`. |
| 6 | **`render_frame` is still missing** (R1 4). The one bug of the round that mattered to the player (fix 2, flat lighting) was invisible to every tool; screenshots came from `undertow -frames 240 -capture x.ppm` under Xvfb, plus temporary "tour" scenes the script authored just to point the camera. | A script, Xvfb, a PPM converter. | The stretch item, with a camera override argument. |
| 7 | **No scripted input in `run_headless`** (R1 3). The game grew an `Autopilot` component and an attract scene to be testable headless, like Prism Break did; both games now carry a test-only mechanic in their shipped module. | ≈ 80 lines of game code. | `run_headless` `input`: per-tick action states. |
| 8 | **A project whose content does not cook breaks the engine build** (R1 6). Hit again on the first build after `new_project.py`: the template scene names textures the project had deleted. | One build. | As in round 1. |
| 9 | **The `.mceproj` is write-only for MCP** (R1 2): `start_scene` was edited by hand. | One line. | `project_set_start_scene`. |
| 10 | **No prefab or hierarchy tools.** The scene is flat: every thruster, light and emitter is an entity with a reference to its parent, positioned by the game. A prefab for "a pod with its light" would have saved 5 × 3 entities and would be the natural MCP ask. | Flat scenes. | `prefab_create` from an entity (the editor has prefabs), `entity_create` with `parent`. |
| 11 | **Every project re-implements the stdio client** (R1 5): Undertow's `Mcp` class is Kindle's with timings and a tools/list probe. | ≈ 90 lines. | `scripts/mcp_client.py`. |

## Numbers

- MCP tool calls for both scenes, the sidecars, the attract check and the main check: 2 800 (one
  script run, 3.2 s; before `scene_clear`, 4 248 calls, 3.4 s). Per tool: `component_set` 1 318,
  `entity_duplicate` 1 266, `entity_create` 182, `scene_clear` 2, `cook_run` 3, `run_headless` 2,
  `asset_set_import_settings` 1 (a sidecar reset to empty settings for the check).
- Each scene: 724 entities, 74 archetype tables (76 live in the attract run), 36 component types in use.
- `run_headless` on the attract scene, 2 400 ticks: `state_hash` `ad2011bedb696925` through
  `undertow_mcp`; the ctest harness gives the same hash twice in a row. The autopilot tows core 1
  home at tick 1 065.
- Game code: `components.h` 273 lines, `undertow.h` 52, `undertow.cpp` 682; tests 352; the authoring
  script 594; art and sound generators 393 + 198.
- Hand edits outside game code, art and the authoring script: one line in `Undertow.mceproj`; 11
  `.mcpart` files written by the script.
- Ship: `Undertow-0.0.1+<sha>-linux-x64.zip`, 5.0 MiB, smoke run 300 ticks OK.
