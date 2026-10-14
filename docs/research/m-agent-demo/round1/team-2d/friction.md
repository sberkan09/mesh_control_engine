# Friction backlog

Collected by mce unknown from 1 project(s).

## blocking (3)

### assets (1)

- A scene AssetRef that loops back (win/lose -> title -> level1) fails the whole cook as a dependency cycle (Glowmoth, level, run_headless)
  - expected: a game loop title -> level1 -> level2 -> win -> title (and level -> lose -> title) is authorable as data: scene refs that are navigation targets, not load dependencies
  - actual: cook error: dependency cycle: level1 -> level2 -> win -> main -> level1 [cook.graph]; every run_headless/render on any scene fails until one back edge is removed
  - workaround: leave win/lose MenuScreen.next_scene empty and ask code for a way back to the title that is not an AssetRef (e.g. restart the start scene)
  - details: No docs mention weak/navigation scene references; any multi-scene game with a menu loop hits this.

### mcp-server (1)

- asset_schema with a type reproducibly breaks the stdio JSON-RPC stream (second time, ParticlePreset) (Glowmoth, art, asset_schema)
  - expected: asset_schema {"type": "ParticlePreset"} returns one JSON line
  - actual: The response has a line break after ~2.7 KB (client JSONDecodeError at line 2 col 1); the stdio bridge dies and the session must be restarted
  - workaround: asset_create the file with no content to get the template, then asset_read it to see the fields

### module-reload (1)

- module_build reports reloaded:true but the server keeps the old module: every reload copies to the same libglowmoth_loaded_<pid>_0.so name (Glowmoth, code, module_build)
  - expected: after module_build, refl_describe_component shows the new components (recipe new-component-live step 3)
  - actual: rebuilt:true reloaded:true, the log says loaded glowmoth v1 from build/libglowmoth_loaded_5270_0.so each time (same name as the first load), and refl_describe_component Moth says unknown component; entity_create with the new components fails
  - workaround: trying to force the server off the cached copy name; otherwise the server has to be restarted
  - details: dlopen of a path already loaded returns the old image (a module with STB_GNU_UNIQUE symbols never unloads), so the copy needs a fresh name per reload

## major (11)

### mcp (3)

- asset_schema {"type":"mctilemap"} killed the MCP session (Glowmoth, level, asset_schema)
  - expected: the .mctilemap fields, or an error naming the accepted spelling
  - actual: the reply was not one JSON line (bridge saw extra data at line 2 col 1 / char 1646); the bridge and the mce_mcp process both exited, losing the session
  - workaround: restarted the bridge with the same serve command; avoiding asset_schema with an extension without the dot
- asset_schema with any type ("Tilemap" too) breaks the stdio framing and the session dies (Glowmoth, level, asset_schema)
  - actual: reply split at char ~1645 into two lines, bridge exits; same with type Tilemap as with mctilemap. Without type it works
  - workaround: restart the bridge; learn fields from asset_create template + asset_read instead
- run_headless log tail has no game-module log lines (Glowmoth, level, run_headless)
  - expected: MCE_LOG_INFO/WARN/ERROR from the game module (e.g. glowmoth: lantern lit, scene change errors) in log.tail and counted in warnings/errors
  - actual: tail only shows the editor cook line and the run start/exit; a scene change that leaves 0 entities shows warnings:0 errors:0 and no reason
  - workaround: infer from metrics.entities and inspect

### mcp-server (2)

- asset_schema {"type": ".mcsheet"} broke the MCP stdio stream (a non-JSON second line on stdout), which killed my bridge (Glowmoth, art, asset_schema)
  - expected: One JSON-RPC line with the sprite sheet schema and template
  - actual: The client got a response that was not one JSON line (JSONDecodeError at line 2 col 1); the bridge process exited and the session lost its open scene
  - workaround: Restarted the bridge with the same command line; author .mcsheet from the example in guide 7.3 instead of asset_schema
- docs_search returns invalid UTF-8 (a snippet cut inside a multi-byte character), which breaks the JSON-RPC stream (Glowmoth, art, docs_search)
  - expected: Snippets truncated on a UTF-8 character boundary so every response is valid UTF-8 JSON
  - actual: docs_search {"query": "font sidecar size atlas pixel"} returned byte 0x94 at an invalid position (an em dash cut in half); the stdio client could not decode it and the bridge exited. The earlier asset_schema crash (non-JSON at line 2) looks related.
  - workaround: Restart the bridge; avoid queries whose snippets hit em dashes; read whole docs with resources/read instead

### content-generators (1)

- gen_sprite cannot be steered toward a specific subject (moth, bat, lantern) and has no animation frames (Glowmoth, art, gen_sprite)
  - expected: A way to ask for a winged/flying silhouette or a recognisable subject, and variants that are animation frames of one sprite (e.g. wings up/down) rather than unrelated seeds
  - actual: kind is only creature/character/ship/item/blob; variants are seed+1, seed+2... i.e. different creatures, so a flap cycle cannot be generated; no way to import hand-drawn pixel art except writing PNGs by hand, which the workflow forbids
  - workaround: Search many seeds and kinds for a silhouette that reads as a moth/bat; use one frame plus code-side bobbing/flip for motion

### cook (1)

- An AssetRef<Scene> field that links scenes into a loop (title -> level -> win -> title) fails the whole cook as a dependency cycle (Glowmoth, code, cook_run)
  - expected: a scene may name the next scene to go to without the cook treating it as a load dependency (a weak reference, or scenes excluded from cycle checks)
  - actual: assets/scenes/level1.mcscene: error: dependency cycle: level1 -> level2 -> win -> main -> level1 [cook.graph]; every tool that cooks (run_headless, render_frame) then fails
  - workaround: next_scene / lose_scene are FixedString fields holding "guid:<uuid>" or a path, resolved at run time against the AssetSystem (parse_guid, else AssetInfo.path)

### module (1)

- module_build in a second agent server reloads the module but its new components stay unknown (Glowmoth, level, module_build)
  - expected: after another agent built new components (Moth, Lantern...), my module_build maps the new .so and refl/entity_create know them, as the workflow says
  - actual: module_build returned ok, rebuilt:false, reloaded:true, but refl_describe_component Moth and entity_create with Lantern say unknown component type; twice
  - workaround: touch the module source to force a real rebuild, else restart this agent server

### module-reload (1)

- module_build says reloaded:true but the new components are not registered (stale module stays loaded) (Glowmoth, art, module_build)
  - expected: After another agent rebuilt the module, module_build in my server maps the new build so MenuScreen, Moth, HudText... are known
  - actual: module_build returns ok, rebuilt:false, reloaded:true, yet refl_list_components still lists only the old Spin/Bob and every cook fails with unknown component type MenuScreen/HudText. The reloaded copy is libglowmoth_loaded_<pid>_0.so each time (same path), which looks like dlopen returning the cached old handle.
  - workaround: Restart the MCP server (my bridge) so it loads the current libglowmoth.so at startup

### scenes (1)

- No run-time scene change, and a game module cannot reach the asset system to do one itself (Glowmoth, code, module_build)
  - expected: a data or code way to go from one scene to the next (title -> level 1 -> level 2 -> win/lose), e.g. a SceneChange event or request resource the player acts on
  - actual: guide 4.5: no built-in scene switching at run time; scene::load needs an asset::AssetSystem, and guide 6.10 says the template host does not hand it to the module
  - workaround: read the AssetSystem pointer from kit::SequenceRegistry::assets() (install_stock sets it), load_sync the Scene, destroy every entity of every World table through Commands and ecs::load_scene inside FixedUpdate_Pre (recorded as Commands)
  - details: every multi-level game made from the template needs this; the workaround relies on an unrelated resource

### testing (1)

- Game-module log lines (MCE_LOG_INFO / ERROR from the module) never reach run_headless log tails or test_run log_contains / log_errors (Glowmoth, code, test_run)
  - expected: log_contains "lantern lit 3/3" passes when the module logs it; log_errors counts the module errors
  - actual: the module lines appear only on the server stderr; log_contains fails and log_errors stays 0 even when the module logs an error
  - workaround: assert on component fields (GameRules.lanterns_lit, complete) instead of log text
  - details: looks like the module has its own spdlog default logger instead of the engine one

## minor (7)

### docs (2)

- engine_capabilities features list contradicts its own component list (Glowmoth, level, engine_capabilities)
  - expected: features[] reach for sprite animation, camera follow/shake and one-shot sfx to say data (AnimatedSprite, CameraFollow2D, CameraShake2D, SfxTrigger are all in components[])
  - actual: features say reach=code: "no AnimatedSprite component yet", "follow and shake are game code", "M-DATA-REACH step 4 adds SfxTrigger" while components[] lists AnimatedSprite, CameraFollow2D, CameraShake2D, SfxTrigger
  - workaround: trust components[] and refl_describe_component over the features text
- engine_capabilities says AnimatedSprite, camera follow and one-shot SfxTrigger need code, but the components exist (Glowmoth, code, engine_capabilities)
  - expected: features list agrees with the components list
  - actual: features: "no AnimatedSprite component yet", "follow and shake are game code", "M-DATA-REACH step 4 adds SfxTrigger"; components list has AnimatedSprite, CameraFollow2D, SfxTrigger and refl_describe_component describes them
  - workaround: trust refl_describe_component over the features list

### project-check (2)

- project_check reports a tileset texture as unused although .mctilemap assets reference it (Glowmoth, art, project_check)
  - expected: textures/meadow_tiles.png counted as used: three .mctilemap files name it as tileset and scenes use those tilemaps
  - actual: info unused assets/textures/meadow_tiles.png: no scene, prefab, material, model or project source references this asset
  - workaround: Ignore the finding
- No way to mark decorative text (a drop shadow) so the contrast check stops warning (Glowmoth, art, project_check)
  - expected: A drop-shadow TextLabel behind a title is decorative; either a flag to exempt it or a shadow/outline option on TextLabel itself
  - actual: Title Shadow labels on title/win/lose warn 1.1:1 contrast; the only fixes are to delete the shadow or make it bright (no longer a shadow)
  - workaround: Leave the warnings (they do not fail pass)

### ship (2)

- project_check flags a deliberate drop-shadow TextLabel for low contrast (Glowmoth, level, project_check)
  - expected: a way to mark a label decorative (or check the shadow against the text it backs)
  - actual: warning: Title Shadow contrast 1.1:1 on every menu scene
  - workaround: accept the warnings
- project_check says the tileset texture is unreferenced although three .mctilemap assets use it (Glowmoth, level, project_check)
  - expected: tilemap tileset references count as uses
  - actual: info: assets/textures/meadow_tiles.png no scene, prefab, material, model or project source references this asset
  - workaround: ignore the info finding

### testing (1)

- module_test finds no tests in an SDK project: the documented <game>_headless_300 smoke test is not registered (Glowmoth, code, module_test)
  - expected: module_test runs glowmoth_headless_300 (guide 14.3 step 6, module_test description)
  - actual: ok:false, tests: [], ctest -N: Total Tests: 0 in the project build tree
  - workaround: ran build/glowmoth -headless 1 -ticks 300 -input <script> by hand to check the real host
