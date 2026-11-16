# M-WORKFLOW — What blind test 3 found in agent authoring (milestone record)

**Status:** active (added 2026-10-16; steps 1 to 8 done). Plan: ROADMAP §M-WORKFLOW; Linear SBA-180 … SBA-184.

**Why:** blind test 3 had three agents build a game (Lantern Keep) over MCP from the SDK and filed GitHub #377 to
#383. #377 to #381 are bugs and workflow holes in code and documents we own; #382 (Luau, UiFlow) belongs to
M-SCRIPT and M-UI-DATA2, #383 (content, texture compression, draw calls) to M-VISUAL2, M-PERF and M-AUDIO2.

**Not in scope:** new content libraries, texture compression, new engine features.

## Steps

| # | Step | Linear | Model | State |
|---|---|---|---|---|
| 1 | `NavAgent3D` reaches its speed on a `CharacterController3D` with the default acceleration (#377) | SBA-180 | Opus | done |
| 2 | `scene_open` reloads, `scene_save` refuses a changed file, broken links reported (#378) | SBA-181 | Sonnet | done |
| 3 | Cook errors name the fix; `cook_run` `paths` and `skip_errors`; stale pack warning (#379) | SBA-182 | Sonnet | done |
| 4 | Headless `-capture` falls back to offscreen; `-capture_tick`; save folder documented (#380) | SBA-183 | Sonnet | done |
| 5 | Stale documents fixed (#381), the two-server test, the close | SBA-184 | Sonnet | done |
| 6 | Perceiver forward documented and visible over MCP (`perception_debug_lines`), stale baked component defaults, test bots that crouch and act (#391) | not filed (Linear at its free limit; this table is the tracker) | Sonnet | done 2026-10-17 by the routine 3 thread: 6a #451, 6c #457, 6b (this PR) |
| 7 | Luau entity lookup by name, UiFlow reacting to signals and `GameState`, `pause` keeping audio, cutscenes not holding scripts, UI scale, Retry not reloading the title (#382, added on Sami's word 2026-10-16) | not filed | Opus | done 2026-10-17 (routine 3 thread): PR 1 #465, 2 #477, 3 #481, 8 #468, 9 #473, 10 #486, PR 5 `UiFlowRules` #494, PR 6+7 `keep_audio` #517, PR 4a `console::set_defaults` + player `defaults.cfg` #527, PR 4b-1 `.mcdefaults` -> `CHNK_CVARS` #541, PR 4b-2 player loads the chunk, one `.mcdefaults` per project, Undertow ships `ui.reference_height = 720` (this PR); open: the sample split of the Undertow title scene (decision note item 6) was left out |
| 8 | Animation clips play from a shared library on any skinned mesh with a matching skeleton (no merging into each glTF); a scene whose entities reference assets but render empty says why (#409) | not filed | Opus | done 2026-10-18 (routine 3 thread): #554 sidecar `animations_from` binds a shared library's clips by bone name at cook (no run-time change; guide 8.9); the same PR makes Kit3D say once why a scene with MeshRefs draws none; open: a hand-written scene that rendered empty was not reproduced, so the note covers the causes visible at run time (no mesh set, no Transform, mesh not loaded) |

**Gates:** existing replay hashes unchanged; each fix has a regression test; `local_ci.sh` green on every step.

## Step 5 — #381, SBA-184 (done)

Each claim of the issue was checked against the code before a document was touched.

- **1. 3D particles (stale, fixed).** User guide 8.8 said `ParticleEmitter` and `.mcpart` draw in 2D scenes only.
  `ParticleEmitter3D` (kit3d.h, extracted to `render::ParticleEmitter` in `kit3d_extract.cpp`, test `kit3d: Foliage3D,
  Decal3D and ParticleEmitter3D ...`) works and 8.1 already listed its fields. 8.8 now says how to use it (a burst when the
  entity first appears and on each `burst_version` bump, `emitting`, `rate` 0 for bursts only, render-side, unlit,
  `ground_height` is a world y) and that the 2D emitter and `.mcpart` are still 2D only.
- **2. `engine_capabilities` (stale, fixed).** Two rows of `kFeatures` (`tools_docs.cpp`) said UI and scene changes are
  `code`: "HUD and menus ... `.mcui` is M-UI-DATA" and "scene changes". Now `ui` is data (`UiScreen` + `.mcui`, the `ui_*`
  tools, `UiFlow`) and scene changes are data through `UiFlow.load_on` (Luau `mce.load_scene` and
  `scene::request_change` for rules); a new row lists what the tools author (scenes, UI documents, timelines, 2D
  skeletons, behaviour trees, terrain). The tool description says the same. `docs/engine/capabilities.md` is generated
  from components and tools, not from these rows, and `capabilities.py --check` passes unchanged.
- **3. Shipping (fixed).** Chapter 15 now opens with the SDK form (`mce build`, `mce ship`, what it takes and does not
  take: `--version`, `--out`, `--target web`, `--steam`; no `--preset`, desktop `--smoke` or symbols zip) and keeps
  `scripts/ship.py` and `projects/` as the engine-checkout form; the checklist gives both.
- **4. Fixed arrays (fixed).** The scene reader (`scene_importer.cpp`, `write_field`) requires an array of exactly the
  field's `count` elements; a shorter one is `expected an array of 8 elements`. Nothing in the code takes "the first
  `count`". The wording said so for `CameraRail3D.points` (user guide 13.6 row, the field's `Help` text in
  `cinematic.h`, `docs/engine/kit.md`); `TimelinePlayer.bindings` and `Blackboard.entities` had no sentence at all, so
  13.5 and 13.8 now state the rule, and the value table in 4.1 names the three arrays.
- **5. `ui_preview` echoing the wrong document (real bug, fixed).** `ui_preview` held a `const ProjectAsset*` across
  `render_ticks`, whose `cook` rescans the project and frees the asset records, then read `doc->path` and `doc->guid`
  for the reply: a dangling pointer, so the echoed name could be another document's (or garbage). It now copies the path
  and GUID before the cook. The GPU case of `test_mcp_agent` asserts the reply names `ui/menu.mcui`, then previews a
  second document and the first again and checks each reply names its own document and GUID. I did not reproduce the
  wrong name before the fix (it depends on the allocator reusing the freed block), so the assertion pins the contract,
  not a seen failure.
- **6. Conventions (documented; one warning added).** User guide 8.6 has a "Conventions that are easy to guess wrong" list,
  8.7 one bullet, all read from the code: the sky texture is equirect with +Y up, its centre column facing +X and u
  growing toward +Z (not mirrored, no rotation setting); a decal projects along its entity's local -Y (u along +X, v
  along +Z; turn it +90 degrees about X for a wall facing +Z); `BoxCollider3D` is an oriented box that follows the
  entity's rotation and scale (a scale-only change after the first tick does not rebuild it; moving or turning does);
  point and spot shadow lookups are pushed one to three texels off the surface (a texel is about `distance / 256` m at
  the default 512 tile), so slabs thinner than that leak. New test `kit3d: a BoxCollider3D follows the entity's
  rotation` pins the box behaviour. **Decal limits:** 64 decals and 3 distinct textures per frame were already
  documented (8.1, 8.6) and counted in `Scene3D::stats().decals_dropped`; the renderer was silent. `Forward3D::bind_decals`
  now logs `visible decal(s) dropped` once per process when the count is above 0 (a log line only: no draw, no hash
  changes). Not done: a real fix for thin-slab leaks (a bias change moves every shadow in every golden) and a per-frame
  or per-scene warning (the log is once per process).
- **7. Importer errors (documented; one message sharpened).** User guide 5.2 has the three rules: glTF buffers must be
  inside a `.glb` or base64 `data:` URIs (the cook message already says so, now also in the guide), `asset_write` never
  takes `format` / `version` / `guid` in either mode, and data textures (normal, roughness, LUT) need `"srgb": false`. The
  `asset_write` refusal now ends `kept from the file: leave them out of 'content'` (`asset_tools.cpp`, pinned in
  `test_mcp.cpp`). The `scene_save` conflict refusal also names `overwrite: true`. Not changed: the glTF message (it
  already names the fix) and the adopt-time `srgb: true` default (changing it moves cooked bytes).
- **8. Two-server test.** `tests/mcp/run_transcript.py` drives one `mce_mcp` process, so the case is a C++ test in
  `test_mcp_team.cpp` (`two servers on one scene ...`) over two `Server` objects that share only the project folder, which
  is how `test_mcp_team` already models two agents (the processes of blind test 3 differ only in the pipe). Alice adds
  Lantern and Door, Bob Guard and Key, both save: the file has all four and the six originals, each server sees the
  other's entities (Alice after `scene_open`). Then both set `Ball`'s restitution: Bob's save is refused naming
  `"Ball" components/Collider2D`, writes nothing and names `overwrite`; `overwrite: true` saves his copy, keeping all four
  merged entities. Not done: a true two-process transcript (it needs a multi-server runner).
- **Checked by running it.** On linux-gcc-debug (`-j4`): `test_mcp_team` (new two-server case, 137 assertions in it),
  `test_mcp`, `test_mcp_agent` and `test_mcp_agent_gpu` (software GL; the `ui_preview` cases with the new assertions),
  `test_kit` (new `BoxCollider3D` rotation case) and `mcp_transcript` pass; the transcript golden was re-blessed (only the
  `engine_capabilities` description line changed). `check_docs.py`, `check_walls.py` and `capabilities.py --check` pass;
  `docs/engine/capabilities.md` was regenerated (three components now name `04-scenes-and-prefabs.md` as the first guide
  chapter that mentions them). **Not run:** `local_ci.sh`, the full ctest, Release; no test exercises the new
  decal-dropped log line (it is a log call beside code `test_render3d_scale` already covers).

## Step 4 — #380, SBA-183 (done)

- **What was already there** (the issue was filed against SDK build `ffa87c7c`). `-capture_tick N` and
  `-capture_ticks a,b,c` exist: the sim holds on the tick and the capture is the first frame after it, with
  `-input` played by sim tick; `-platform.video_driver offscreen` was in the command-line table (16.1) and a
  one-line hint in the troubleshooting table; the save folder and `-save_dir` were in 2.5 (`$XDG_DATA_HOME` or
  `~/.local/share`, `%LOCALAPPDATA%`). `save::root()` is `crash::user_data_root()/MCE/MCE/saves`: the engine reads
  `XDG_DATA_HOME` itself and does not use `SDL_GetPrefPath`, so SDL's variables do not matter.
- **What was missing and is now done.** (1) `-capture` / `-capture_tick(s)` with no display still ended in "No
  available video device". Now `player.cpp` (`init_platform_or_offscreen`) retries `platform::init` on the offscreen driver with
  `LIBGL_ALWAYS_SOFTWARE=1` (a value already set wins) when a capture was asked for and SDL could not open a video
  device, and prints `no display for the capture: drawing offscreen with software GL`. An explicit
  `-platform.video_driver` is never replaced. The layer is the player (SDL init), so a project's own host and
  `mce play` get it; the Python CLI sets nothing. (2) The player logs `save folder <path>` (info) once at start.
  (3) Docs: user guide 16.8 (the recipe, which frame each flag gives, the title-menu case), 2.5 (the `<data>` table per
  OS with the override), the 16.1 rows and the troubleshooting row.
- **`-frames` with `-capture`** is the Nth rendered frame, whatever tick the sim is on: `-frames 30` was tick 2 offscreen
  here. That is the "does not land on a fixed tick" of the issue; 16.8 says so and points to `-capture_tick`. The
  behaviour is unchanged (it is the CI smoke the GPU tests use).
- **`timeline_preview` over a title menu: not changed, documented.** The tool starts the real player on the scene
  with `timeline.scrub`; a title menu is drawn by the game (its code or UI document) as a normal part of the scene's
  run, and the engine has nothing that tells it from a HUD, so there is no safe way to leave it out. The tool already
  takes `input` and `convars` to press it away or switch it off; 16.8 and the guide say so.
- **Not done:** a convar that hides all game UI for captures; the capture falls back only when init fails, so a
  display that exists but is dead (`DISPLAY` set to nothing listening) fails as before and needs
  `-platform.video_driver offscreen`.
- **Checked by running it.** In this container (no `DISPLAY`, no GPU) on linux-gcc-debug: `mce_player -pack
  samples/skel2d_demo/project/assets.mce -capture_tick 120 -capture shots/a.ppm` before the change failed with "No
  available video device"; after it prints the fallback line, writes a 1280x720 PPM (2,764,816 bytes) and exits `0
  after 120 ticks`; two runs of `-capture_tick 60` are byte-identical (`cmp`); `-capture_ticks 60,120 -capture
  "t_{tick}.ppm"` writes both; `-frames 30` ends on tick 2; `LIBGL_ALWAYS_SOFTWARE=1 ... -platform.video_driver
  offscreen` and `xvfb-run -a ... env LIBGL_ALWAYS_SOFTWARE=1 ...` work; `-platform.video_driver x11` still fails with
  "x11 not available"; `XDG_DATA_HOME=...` and `-save_dir` move the logged folder. New ctest: `player_save_folder`
  (headless: logged once, XDG, `-save_dir`) and `player_capture_no_display` (GPU label; `DISPLAY`, `WAYLAND_DISPLAY` and
  `SDL_VIDEODRIVER` unset: fallback line, `exit 0 after 30 ticks`, a 320x180 PPM of 172,815 bytes, explicit driver
  respected); both pass.

## Step 3 — #379, SBA-182 (done)

- **What was already there.** The cook already isolates failures: a failed or unregistered file blocks only the
  assets that depend on it, everything else cooks, and a failed asset's last output keeps serving. What an agent
  met was `ok: false`, a pack that was not written, and, for `run_headless`, `render_frame` and the other tools that
  cook the whole project first, a refusal over one unrelated file.
- **The missing-sidecar error names the fix** (`registry.cpp`): `cook_run {"adopt": true}` over MCP,
  `mce cook <project> --adopt` on the command line (the flag exists; `mce build` stops on the same error).
- **`cook_run` `paths`** (`CookOptions::paths`, `mce_cook --only`): files or folders under `assets/`. The scan skips
  the errors of files outside them (and `adopt` does not touch them), the graph keeps the named assets and everything
  they depend on, the rest get `AssetStatus::Skipped`. Skipped assets stay in the manifest with their last output, so
  `cooked/` stays whole. **A subset never writes the pack:** the pack must hold one consistent cook of the whole
  project, so with `paths` it is not rewritten and the report says so (`pack_written: false`, `pack_note`). An entry
  that matches no asset is an error.
- **`skip_errors`** (`CookOptions::skip_errors`, `mce_cook --skip-errors`): the cook always cooked what it could; this
  adds `CookReport::failed_files` (every file with an error and its first message, uncapped, in the tool result as
  `failed_files`). The pack is still written only by a run with no errors: a pack missing a file the game references
  would not load, so `skip_errors` does not relax that.
- **Stale pack warning** (`tools/mce_cli/mce.py`, `stale_pack_warning`): `mce play` and `mce headless` on a game
  project print, on stderr, that `build/project/assets.mce` is older than the newest file under `assets/` and name
  `mce build`. Script projects cook before they run and do not need it.
- **Unknown component or field** (`scene_importer.cpp`, the editor's scene reader): the error adds that the loaded
  module is probably older than the source, to rebuild it (`mce build` or `module_build`) and restart the MCP server
  (`mce mcp <project>`) unless it was started with `--module`; `mce cook` loads the module fresh each time.
- **Docs:** user guide 5.4 "When a cook fails" (the table), 14 "When a cook fails" and the `cook_run` row, 16.4, 17.
- **Not done:** `run_headless`, `render_frame`, `ui_preview` and `timeline_preview` still cook the whole project and
  fail on any error; they do not take `paths`. A `paths` entry that names an unadopted file is its own error (adopt
  first); a file outside the paths that a named asset reaches only by path text is reported as a missing reference.
- **Checked by running it.** `test_cook` has four new cases: the missing-sidecar text names both fixes and
  `failed_files` lists it once; `skip_errors` over an unadopted file and a corrupt PNG cooks the five good assets,
  lists both bad files and writes no pack; `paths: ["materials"]` cooks the material and the texture it needs, skips
  the wav, font and scene, ignores an unadopted file elsewhere, writes no pack and says so, a full cook afterwards
  cooks exactly the rest, a later one-file subset keeps the manifest hash, a path that matches nothing is an error;
  an unknown component and an unknown field both carry the rebuild-and-restart text. `test_mcp` has `cook_run paths
  and skip_errors` over the tool. `cli_stale_pack` (Python) covers the warning function: no warning for a newer
  pack, one naming the newest source and `mce build <project>` for an older one, hidden folders ignored. The MCP
  transcript golden was re-blessed (the `cook_run` description). `ctest -R "mcp|cook|asset|editor|import|cli_stale"`
  passed on linux-gcc-debug except `ai_demo_editor_bt`, which needs a display and fails the same way without this change.

## Step 2 — #378, SBA-181 (done)

- **What was already there.** M-AGENT step 7 already hashed the file at open and save and merged a teammate's
  save by entity, so `scene_save` from a stale copy did not drop their entities. What was still wrong:
  a copy of the scene parked by `scene_switch` came back stale on the next switch, a save that could not be
  merged said nothing about which entities were at stake, and the nulled links were silent.
- **`scene_open`** re-reads the file from disk every time (also when the scene is already open) and drops
  the copy `scene_switch` parked for that file; a parked copy with unsaved edits needs `force`.
- **`scene_save`**: when the file changed on disk it still merges by entity (their new entities are kept,
  unlike a plain refusal). When it cannot merge (no common version, unparsable), it refuses with the entities
  on disk the copy lacks and the ones only the copy has, and names the two ways out: `scene_open {force:true}`
  or `overwrite: true`. `overwrite` is new; it saves over the file like `force` does, without touching
  the save-as rule `force` also has.
- **`entity_delete`** returns `nulled_links` (owning entity id and name, component, field path, the id it
  pointed at) and `deleted_children`, plus a warning, when references from entities that stay became null.
- **Not done: the `project_check` rule.** The reflection data has no required flag for entity-reference
  fields and a null link in a saved file is indistinguishable from an unset one, so there is nothing to check;
  the delete-time report is the detection.
- **Checked by running it.** `ctest -R mcp`: `test_mcp_team` has four new cases on two servers sharing one
  project: a teammate's saved entity and its child show up after `scene_open` and a stale-then-reloaded save drops
  nothing; `scene_open` replaces a parked copy; an unmergeable save is refused naming the entity, `overwrite`
  saves it, `scene_open {force:true}` recovers; deleting `Ball` reports `Camera.CameraFollow2D.target`, a subtree
  delete reports its child and the link to it. The MCP transcript golden was re-blessed (tool descriptions).

## Step 1 — #377 (done)

- **Cause.** Not the nav agent's acceleration: `kit3d_bodies` treated any change of the entity's pose as a
  teleport and zeroed the controller's velocity. `NavAgent3D` with `face_motion` (the default) turns the entity
  every tick, so the controller restarted its acceleration ramp every tick and walked at one tick's worth of
  `acceleration` (about 0.13 m/s of 3.5). With `acceleration` 0 the ramp does not exist, which is why that hid it.
- **Fix.** A rotation-only change still teleports the capsule but keeps `velocity`; only a moved position resets it.
- **Tests.** Two cases in `test_nav3d.cpp`: a steady walk reaches the agent's speed at accelerations 8, 20 and 60,
  and a patrol whose post keeps moving (the agent turns every tick) covers 3 s at nearly full speed. User guide 9.
- **The 20 m test (SBA-180 follow-up).** A third case walks the agent the issue's 20 m to arrival in two legs (10 m
  along +X, then a retarget onto a 10 m diagonal) at `acceleration` 14 and 0: the eased walk keeps over 90% of the
  agent's speed mid-leg and arrives within 1.5 s of the instant one. Without the fix it takes 11.25 s against 7.57 s.
